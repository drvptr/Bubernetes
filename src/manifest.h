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
 * The parser is intentionally a small subset: `---` document separators,
 * `key: value` scalars, `# comments`, and lists in either `[a, b]` or block
 * `- a` form. Enough for the schema below, nothing more. (libyaml would drop in
 * here if the schema ever outgrew this, but the whole point is that it should
 * not.)
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
};

/* parse a buffer; emit() is called once per document. emit returns 0 to
 * continue, non-zero to stop (that value is returned). Malformed documents are
 * logged and skipped. Overall return: 0 ok, or emit's non-zero stop value. */
int manifest_parse(const char *text, size_t len,
                   int (*emit)(const struct manifest *m, void *ctx), void *ctx);

/* scan a directory for *.yaml / *.yml and parse each. Same emit contract. */
int manifest_scan_dir(const char *dir,
                      int (*emit)(const struct manifest *m, void *ctx), void *ctx);

/* serialise one resource's spec as a single YAML document (leading `---`). */
void manifest_dump_res(struct buf *out, res_t *r);

/* inotify on the directory. Returns a watch fd for the event loop, or -1. */
int manifest_watch_init(const char *dir);
/* consume pending inotify events; returns 1 if the directory changed, else 0. */
int manifest_watch_drain(int fd);

#endif
