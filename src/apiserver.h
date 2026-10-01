#ifndef APISERVER_H
#define APISERVER_H

#include <stddef.h>
#include <stdint.h>

/*
 * The local API.
 *
 * This is the one abstraction the rest of Bubernetes is built on, so it is
 * worth stating what it is for. The scheduler, the reconciler and the gossip
 * layer must be able to work with a resource without knowing what is inside it.
 * If any of them could call GetReplicas(), GetPid(), GetName(), then the layout
 * of a resource would be smeared across the whole program and could never be
 * changed again. So a resource is an opaque handle, and everything about it is
 * reached through two tiny vocabularies:
 *
 *   verbs  - what you want to do   : Get, Set, Watch, Create, Delete (and, over
 *                                     the store, List)
 *   nouns  - which property        : STATUS, REPLICAS, IMAGE, ...
 *
 *   for (r in store) value = ResGet(r, noun);
 *
 * The same loop runs over every kind of resource. What a given noun *means* is
 * the caller's business, decided by which resource it asked: REPLICAS on a
 * DEPLOY is a desired count; on a NODE it is simply absent. The apiserver never
 * interprets; it only stores and returns.
 *
 * Concretely a resource happens to be a flat array of fields indexed by noun.
 * That is a secret of apiserver.c. The opaque struct here is just the hook that
 * lets us keep the definition in the .c file.
 */

/* opaque handles - nobody outside apiserver.c sees the fields */
typedef struct res res_t;
typedef struct resp resp_t;

/*
 * Nouns: the universal property set. Every resource answers to the same set;
 * most resources leave most of them empty. Add a noun here and every verb,
 * every loop and the wire protocol can carry it with no further changes.
 *
 * A noun is either "spec" (part of the desired state: replicated across the
 * cluster, authored by a manifest) or "runtime" (observed locally, never
 * replicated). NounIsSpec() draws that line; it is the only place the line is
 * drawn.
 */
enum {
    NOUN_ID,        /* bytes : stable cluster-wide id (sha of name+kind)     spec    */
    NOUN_NAME,      /* bytes : name from the manifest                        spec    */
    NOUN_KIND,      /* int   : KIND_*                                        spec    */
    NOUN_REPLICAS,  /* int   : -1 == every node, N == N copies               spec    */
    NOUN_IMAGE,     /* bytes : sha256 of the executable image                spec    */
    NOUN_ARGV,      /* bytes : NUL-separated argv for the workload           spec    */
    NOUN_VERSION,   /* int   : lamport version, for convergence              spec    */
    NOUN_ORIGIN,    /* bytes : name of the node that authored this spec      spec    */

    NOUN_STATUS,    /* int   : ST_*                                          runtime */
    NOUN_ADDRESS,   /* bytes : "ip:port" (NODE)                              runtime */
    NOUN_PID,       /* int   : one running pid (first replica)               runtime */
    NOUN_RUNNING,   /* int   : replicas actually running here                runtime */
    NOUN_MEM,       /* int   : rss bytes                                     runtime */
    NOUN_CPU,       /* int   : cpu permille                                  runtime */

    NOUN__COUNT
};

/*
 * Kinds. Deliberately few. Behaviour that Kubernetes splits into Pod /
 * Deployment / DaemonSet is here expressed through NOUN_REPLICAS on a single
 * DEPLOY kind, so there is no manifest to rewrite when a single process grows
 * into a fleet of them. A kind is spent only on something genuinely different:
 *
 *   KIND_NODE   - a cluster member. A resource like any other, so the same
 *                 verbs/nouns inspect a node and a workload.
 *   KIND_STATIC - a process pinned to the node whose manifest declared it.
 *                 Never scheduled elsewhere (the analogue of a static pod).
 *   KIND_DEPLOY - a scheduled workload. replicas == 1 is a single instance,
 *                 replicas == N is N instances, replicas == -1 is "one on every
 *                 node" (a daemonset). One primitive, three behaviours.
 */
enum { KIND_NODE, KIND_STATIC, KIND_DEPLOY };

/* runtime status values */
enum { ST_PENDING, ST_READY, ST_UNHEALTHY, ST_UNREACHABLE, ST_GONE };

/* classify a noun. 1 == spec (replicated desired state), 0 == runtime (local) */
int NounIsSpec(int noun);
/* human name of a noun / kind / status, for logs, yaml and bubectl */
const char *NounName(int noun);
const char *KindName(int kind);
const char *StatusName(int status);
int         KindFromName(const char *s);   /* -1 if unknown */

/* ------------------------------------------------------------------ verbs
 *
 * Local verbs. The target is a pointer in this address space, so the call is a
 * direct memory access. The networked form of exactly these verbs lives in
 * wire.h: there the target is an id, the receiving node resolves it to its own
 * res_t * with StoreGet(), and runs the very same local verb. A pointer cannot
 * travel over a socket; an id, a verb and a noun can.
 */
resp_t *ResGet(res_t *r, int noun);
int     ResSet(res_t *r, int noun, const resp_t *v);
int     ResWatch(res_t *r, int noun);          /* register interest; see below */
res_t  *ResCreate(void);
void    ResDelete(res_t *r);

/* WATCH, completed: ResWatch marks a noun as watched on a resource; a single
 * observer registered here is called whenever a watched noun is Set. The
 * reconciler uses it to wake on spec changes rather than only on its timer. */
void    ApiOnChange(void (*fn)(res_t *r, int noun, void *ctx), void *ctx);

/* small typed conveniences over ResGet/ResSet - still generic, still about the
 * value and never about a particular resource */
int64_t ResGetInt(res_t *r, int noun);                 /* 0 if unset        */
void    ResSetInt(res_t *r, int noun, int64_t x);
const void *ResGetBytes(res_t *r, int noun, size_t *len);  /* NULL if unset */
void    ResSetBytes(res_t *r, int noun, const void *p, size_t n);
int     ResHas(res_t *r, int noun);                    /* is the noun set?  */

/* ------------------------------------------------------- response values
 *
 * A resp_t wraps one value: either an integer or a byte string. The accessors
 * are about the value's own type, never about which resource produced it, so
 * they do not reintroduce the coupling the opaque handle removes.
 */
int           RespIsInt(const resp_t *v);
int64_t       RespInt(const resp_t *v);
const void   *RespBytes(const resp_t *v, size_t *len);
const void   *RespGetValuePtr(const resp_t *v);   /* raw pointer, legacy form */
resp_t       *RespFromInt(int64_t x);
resp_t       *RespFromBytes(const void *p, size_t n);
void          RespFree(resp_t *v);

/* ------------------------------------------------------------------ store
 *
 * The set of all local resources, keyed by id. This is the layer the wire
 * protocol talks to: a remote request names an id, and the node turns it into a
 * res_t * here. StoreForEach is also how "desired state" is examined without
 * ever being a single object - you iterate and apply a function (see below).
 */
res_t *StoreCreate(const void *id, size_t idlen);   /* create + insert; or existing */
res_t *StoreGet(const void *id, size_t idlen);
res_t *StoreGetName(const char *name);              /* convenience for bubectl */
void   StoreRemove(const void *id, size_t idlen);
void   StoreForEach(void (*fn)(res_t *r, void *ctx), void *ctx);
size_t StoreCount(void);

/* ------------------------------------------------------ desired state
 *
 * There is no desired_state_t and there is not going to be one. The desired
 * state is whatever you compute by applying a function over the resources that
 * carry a spec. Tie the program to a struct and tomorrow's change to the shape
 * of a manifest is a rewrite; tie it to a function over opaque resources and
 * the same code keeps working. These helpers are those functions, nothing more.
 */

/* visit every resource that is part of the desired state (has a spec, not a
 * pure runtime/NODE record). The callback receives the opaque handle only. */
void DesiredForEach(void (*fn)(res_t *r, void *ctx), void *ctx);

/* fold the desired state into a single value without materialising it: the
 * caller supplies the accumulator. Used e.g. to hash the whole desired state
 * for a quick "are we converged?" check, or to count instances. */
void DesiredFold(void (*fn)(res_t *r, void *acc), void *acc);

/* true if the resource carries a spec that this node should act on */
int ResIsDesired(res_t *r);

/* copy only the spec nouns (the replicated desired state) from src to dst,
 * leaving dst's runtime nouns untouched. Used wherever a learned or applied
 * spec is merged onto a resource we already hold. */
void ResCopySpec(res_t *dst, res_t *src);

#endif
