#ifndef MANIFEST_H
#define MANIFEST_H

#include "apiserver.h"
#include "util.h"

/*
 * Manifests. The manifest directory is the only thing bubelet reads from disk
 * and the only place desired state is kept - there is no database. A file in
 * the directory means "this resource should exist"; removing the file means "it
 * should not". A node persists everything it knows here as <name>.yaml, so a
 * restart rebuilds desired state from the directory and gossip then fills any
 * gaps. That is what makes `bubectl dump > all.yaml` and dropping the file onto
 * a fresh node's directory a complete, if blunt, backup and restore.
 *
 * Two kinds of document live in the directory and the loader tells them apart
 * by one key:
 *
 *   - an EDIT: a document without `version:`. Written by a human or dropped in
 *     by bubectl. When its file's content changes the loader authors a new
 *     version of the resource.
 *   - STATE: a document with `version:` and `origin:`. Written by bubelet to
 *     persist what the cluster agreed on (and by `bubectl dump`). Loaded with
 *     exactly that version, under the same last-writer-wins rules as gossip, so
 *     a restart restores the state as it was instead of re-authoring it.
 *
 * The parser is intentionally a small subset: `---` document separators,
 * `key: value` scalars, `# comments`, and lists in either `[a, b]` or block
 * `- a` form, with "double" or 'single' quotes around an item that holds a
 * space, comma, `#` or bracket. (libyaml would drop in here if the schema ever
 * outgrew this, but the whole point is that it should not.)
 *
 *     kind: Deploy          # Node | Static | Deploy
 *     name: web
 *     replicas: 3           # -1 every node, 1 single, N copies
 *     image: /path/to/bin   # a path to ingest, or a 64-hex image hash
 *     argv: [--port, "80"]
 */

struct manifest {
    char kind[32];
    char name[128];
    int  has_replicas;
    long replicas;
    char image[1024];        /* raw: a path to ingest, or a hex hash */
    struct buf argv;         /* NUL-separated argv entries (no program name) */
    int  argc;
    int  has_version;        /* present => this document is persisted STATE */
    unsigned long long version;
    char origin[128];
};

/* parse a buffer; emit() is called once per document. emit returns 0 to
 * continue, non-zero to stop (that value is returned). Malformed documents are
 * logged and skipped. Overall return: 0 ok, or emit's non-zero stop value. */
int manifest_parse(const char *text, size_t len,
                   int (*emit)(const struct manifest *m, void *ctx), void *ctx);

/* list the *.yaml / *.yml files of a directory, handing each one's name and
 * full content to cb. Returns 0 if the directory was read completely, -1 if it
 * could not be opened - callers must not treat a failed listing as "the files
 * are gone". A file that cannot be read is reported with data == NULL. */
int manifest_list_dir(const char *dir,
                      void (*cb)(const char *fname, const unsigned char *data,
                                 size_t len, void *ctx),
                      void *ctx);

/* serialise one resource's spec as a single YAML document (leading `---`).
 * Includes version/origin when the resource has them, i.e. produces STATE. */
void manifest_dump_res(struct buf *out, res_t *r);

/* inotify on the directory. Returns a watch fd for the event loop, or -1. */
int manifest_watch_init(const char *dir);
/* consume pending inotify events; returns 1 if the directory changed, else 0. */
int manifest_watch_drain(int fd);

#endif
