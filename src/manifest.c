#define _GNU_SOURCE
#include "manifest.h"
#include "sha256.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/inotify.h>

/* ----------------------------------------------------------- small helpers */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

/* cut a line at the first '#' that is not inside quotes */
static void strip_comment(char *s)
{
    int q = 0;
    for (; *s != '\0'; s++) {
        if (*s == '"' || *s == '\'')
            q = !q;
        else if (*s == '#' && !q) {
            *s = '\0';
            return;
        }
    }
}

/* strip one layer of surrounding quotes, in place */
static char *unquote(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
                   (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        return s + 1;
    }
    return s;
}

static void manifest_clear(struct manifest *m)
{
    m->kind[0] = '\0';
    m->name[0] = '\0';
    m->has_replicas = 0;
    m->replicas = 0;
    m->image[0] = '\0';
    buf_reset(&m->argv);
    m->argc = 0;
}

static void argv_add(struct manifest *m, const char *item)
{
    buf_append(&m->argv, item, strlen(item) + 1);  /* keep the NUL */
    m->argc++;
}

/* ----------------------------------------------------------- parsing */

static int manifest_valid(const struct manifest *m)
{
    if (m->name[0] == '\0')
        return 0;
    if (KindFromName(m->kind) < 0)
        return 0;
    return 1;
}

static void set_scalar(struct manifest *m, const char *key, char *val)
{
    val = unquote(trim(val));
    if (strcmp(key, "kind") == 0) {
        snprintf(m->kind, sizeof m->kind, "%s", val);
    } else if (strcmp(key, "name") == 0) {
        snprintf(m->name, sizeof m->name, "%s", val);
    } else if (strcmp(key, "image") == 0) {
        snprintf(m->image, sizeof m->image, "%s", val);
    } else if (strcmp(key, "replicas") == 0) {
        m->has_replicas = 1;
        m->replicas = strtol(val, NULL, 10);
    } else if (strcmp(key, "argv") == 0) {
        /* empty here means a block list follows; handled by the caller */
    } else {
        WARN("unknown manifest key '%s' ignored", key);
    }
}

/* parse an inline list body (without the brackets) into argv */
static void parse_inline_list(struct manifest *m, char *body)
{
    char *p = body;
    while (*p != '\0') {
        char *comma = strchr(p, ',');
        if (comma != NULL)
            *comma = '\0';
        char *item = unquote(trim(p));
        if (*item != '\0')
            argv_add(m, item);
        if (comma == NULL)
            break;
        p = comma + 1;
    }
}

int manifest_parse(const char *text, size_t len,
                   int (*emit)(const struct manifest *m, void *ctx), void *ctx)
{
    char *copy = xmalloc(len + 1);
    memcpy(copy, text, len);
    copy[len] = '\0';

    struct manifest m;
    buf_init(&m.argv);
    manifest_clear(&m);

    int in_doc = 0;
    int in_argv_list = 0;      /* collecting block-list items for argv */
    int rc = 0;

    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save);
         line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        strip_comment(line);
        char *t = trim(line);

        if (strcmp(t, "---") == 0) {
            if (in_doc && manifest_valid(&m)) {
                rc = emit(&m, ctx);
                if (rc != 0)
                    goto done;
            } else if (in_doc) {
                WARN("skipping manifest document with no valid kind/name");
            }
            manifest_clear(&m);
            in_doc = 0;
            in_argv_list = 0;
            continue;
        }

        if (*t == '\0')
            continue;

        /* a block-list item for the current list key */
        if (t[0] == '-' && (t[1] == ' ' || t[1] == '\0')) {
            if (in_argv_list) {
                char *item = unquote(trim(t + 1));
                if (*item != '\0')
                    argv_add(&m, item);
            }
            continue;
        }

        in_argv_list = 0;

        char *colon = strchr(t, ':');
        if (colon == NULL) {
            WARN("ignoring manifest line without ':' -> '%s'", t);
            continue;
        }
        *colon = '\0';
        char *key = trim(t);
        char *val = trim(colon + 1);
        in_doc = 1;

        if (strcmp(key, "argv") == 0 && val[0] == '[') {
            char *end = strrchr(val, ']');
            if (end != NULL)
                *end = '\0';
            parse_inline_list(&m, val + 1);
        } else if (strcmp(key, "argv") == 0 && val[0] == '\0') {
            in_argv_list = 1;      /* block list items follow */
        } else {
            set_scalar(&m, key, val);
        }
    }

    if (in_doc && manifest_valid(&m))
        rc = emit(&m, ctx);
    else if (in_doc)
        WARN("skipping manifest document with no valid kind/name");

done:
    buf_free(&m.argv);
    free(copy);
    return rc;
}

/* ----------------------------------------------------------- directory scan */

static int read_file(const char *path, struct buf *out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    buf_reset(out);
    char tmp[8192];
    size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_append(out, tmp, r);
    fclose(f);
    return 0;
}

static int has_suffix(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

int manifest_scan_dir(const char *dir,
                      int (*emit)(const struct manifest *m, void *ctx), void *ctx)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        if (errno != ENOENT)
            PERR("opendir %s", dir);
        return -1;
    }
    struct buf content;
    buf_init(&content);
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!has_suffix(de->d_name, ".yaml") && !has_suffix(de->d_name, ".yml"))
            continue;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (read_file(path, &content) != 0) {
            PERR("read %s", path);
            continue;
        }
        rc = manifest_parse((const char *)content.data, content.len, emit, ctx);
        if (rc != 0)
            break;
    }
    buf_free(&content);
    closedir(d);
    return rc;
}

/* ----------------------------------------------------------- yaml dump */

void manifest_dump_res(struct buf *out, res_t *r)
{
    char line[1200];

    buf_append_str(out, "---\n");

    int kind = (int)ResGetInt(r, NOUN_KIND);
    snprintf(line, sizeof line, "kind: %s\n", KindName(kind));
    buf_append_str(out, line);

    size_t nlen;
    const char *name = ResGetBytes(r, NOUN_NAME, &nlen);
    if (name != NULL) {
        snprintf(line, sizeof line, "name: %.*s\n", (int)nlen, name);
        buf_append_str(out, line);
    }

    if (ResHas(r, NOUN_REPLICAS)) {
        snprintf(line, sizeof line, "replicas: %ld\n",
                 (long)ResGetInt(r, NOUN_REPLICAS));
        buf_append_str(out, line);
    }

    size_t ilen;
    const unsigned char *img = ResGetBytes(r, NOUN_IMAGE, &ilen);
    if (img != NULL && ilen > 0) {
        char hex[2 * SHA256_LEN + 1];
        if (ilen == SHA256_LEN) {
            hex_encode(hex, img, ilen);
            snprintf(line, sizeof line, "image: %s\n", hex);
            buf_append_str(out, line);
        }
    }

    size_t alen;
    const char *argv = ResGetBytes(r, NOUN_ARGV, &alen);
    if (argv != NULL && alen > 0) {
        buf_append_str(out, "argv: [");
        size_t off = 0;
        int first = 1;
        while (off < alen) {
            const char *item = argv + off;
            size_t ilen2 = strnlen(item, alen - off);
            if (!first)
                buf_append_str(out, ", ");
            first = 0;
            /* quote if it contains a space or comma */
            if (memchr(item, ' ', ilen2) != NULL ||
                memchr(item, ',', ilen2) != NULL) {
                buf_append_byte(out, '"');
                buf_append(out, item, ilen2);
                buf_append_byte(out, '"');
            } else {
                buf_append(out, item, ilen2);
            }
            off += ilen2 + 1;
        }
        buf_append_str(out, "]\n");
    }
}

/* ----------------------------------------------------------- inotify */

int manifest_watch_init(const char *dir)
{
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        PERR("inotify_init1");
        return -1;
    }
    int wd = inotify_add_watch(fd, dir,
                               IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM |
                               IN_CREATE | IN_DELETE);
    if (wd < 0) {
        PERR("inotify_add_watch %s", dir);
        close(fd);
        return -1;
    }
    return fd;
}

int manifest_watch_drain(int fd)
{
    char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    int changed = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;      /* EAGAIN: drained */
        }
        changed = 1;    /* some change happened; caller will do a full rescan */
    }
    return changed;
}
