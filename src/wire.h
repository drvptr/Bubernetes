#ifndef WIRE_H
#define WIRE_H

#include "apiserver.h"
#include "util.h"

/*
 * The wire protocol: the same verb/noun vocabulary as the local API, but with
 * an id where the local API has a pointer.
 *
 *   local  : ResGet(res_t *r, noun)        - r is memory, so this is a load
 *   remote : {verb=GET, id, noun}          - travels, and the far node does
 *            StoreGet(id) -> ResGet(r,noun) and sends the value back
 *
 * A pointer cannot cross a socket; an id, a verb and a noun can. That is the
 * whole trick, and it is why the scheduler/reconciler can be written once
 * against the local verbs and still operate on resources that live elsewhere.
 *
 * Everything rides a tiny length-framed envelope:  [u32 len][u8 type][payload]
 */

/* message types (the envelope's type byte) */
enum {
    MSG_REQ = 1,        /* one verb, one noun, one id (+ optional data) */
    MSG_RESP,           /* its answer                                   */
    MSG_LIST_REQ,       /* "dump what you know"  (bubectl get/dump)     */
    MSG_LIST_RESP,      /* a run of resources                           */
    MSG_APPLY,          /* "make this spec exist" (bubectl apply)       */
    MSG_APPLY_RESP,
    MSG_DELETE,         /* "this name should no longer exist"           */
    MSG_DELETE_RESP,
    MSG_GOSSIP_SYN,     /* anti-entropy: my specs + versions            */
    MSG_GOSSIP_ACK,     /* anti-entropy: specs you were behind on       */
    MSG_BLOB_REQ,       /* "send me the image with this hash"           */
    MSG_BLOB_RESP
};

/* verbs (inside MSG_REQ) */
enum { V_GET = 1, V_SET, V_WATCH, V_LIST, V_CREATE, V_DELETE };

/* response status codes */
enum { W_OK = 0, W_NOTFOUND = 1, W_ERR = 2, W_AMBIGUOUS = 3 };

/* ---- generic resource codec -------------------------------------------
 * One codec for every message that carries a resource. It walks nouns, not
 * fields: [u16 count] then per noun [u16 noun][u8 is_int][value]. spec_only
 * restricts it to the replicated nouns, which is exactly what gossip wants and
 * exactly what a manifest produces. The reader returns a detached resource
 * (not inserted in the store) holding whatever nouns were present.
 */
void   wire_put_res(struct buf *b, res_t *r, int spec_only);
res_t *wire_get_res(struct rdr *rd);

/* ---- single verb request/response codec ------------------------------- */
void wire_put_req(struct buf *b, int verb, int noun,
                  const void *id, size_t idlen,
                  const void *data, size_t datalen);
int  wire_get_req(struct rdr *rd, int *verb, int *noun,
                  const void **id, size_t *idlen,
                  const void **data, size_t *datalen);

void wire_put_resp(struct buf *b, int status, const resp_t *v);
/* reader: status returned; *v is a detached resp_t or NULL (caller frees) */
int  wire_get_resp(struct rdr *rd, resp_t **v);

/* ---- transport --------------------------------------------------------
 * A connection is a socket plus an optional TLS session. In the plaintext
 * build ssl stays NULL and the reads/writes go straight to the fd; built with
 * TLS, the same calls run through OpenSSL. Nothing above this struct has to
 * know which.
 */
struct conn {
    int fd;
    void *ssl;      /* SSL* when built with BUBE_TLS, else NULL */
};

int  wire_listen(const char *ip, int port);          /* -> listen fd, or -1 */
/* accept a connection (no TLS yet, so this never blocks on the peer) ... */
int  wire_accept(int lfd, struct conn *out);          /* 0 ok, -1 err/EAGAIN */
/* ... then complete the server side of the handshake, off the event loop */
int  wire_accept_handshake(struct conn *c);           /* 0 ok, -1 err */
int  wire_connect(const char *ip, int port, const char *server_name,
                  struct conn *out);                  /* 0 ok, -1 err */
int  wire_send(struct conn *c, int type, const void *payload, size_t len);
/* reads one frame; refuses a payload larger than wire_max_payload(type) so a
 * hostile header cannot make us allocate for a message that type never needs */
int  wire_recv(struct conn *c, int *type, struct buf *payload);
size_t wire_max_payload(int type);
void wire_close(struct conn *c);

/* one request, one response, over a fresh connection. Blocking; used by the
 * periodic tasks (gossip, blob fetch) and by bubectl. Returns 0 on success and
 * fills reply with the response payload (its type in *rtype). */
int wire_call(const char *ip, int port, const char *server_name,
              int type, const void *payload, size_t len,
              int *rtype, struct buf *reply);

#endif
