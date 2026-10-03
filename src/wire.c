#define _GNU_SOURCE
#include "wire.h"
#include "tls.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* a static binary image can be several megabytes; anything past this is
 * treated as a corrupt or hostile frame */
#define MAX_MSG (64u * 1024u * 1024u)

/* how long one peer may stall a read or write before we give up on it */
#define IO_TIMEOUT_MS 2000

/* ===================================================== resource codec */

static void put_value(struct buf *b, const resp_t *v)
{
    if (RespIsInt(v)) {
        buf_append_byte(b, 1);
        buf_append_u64(b, (uint64_t)RespInt(v));
    } else {
        size_t len;
        const void *p = RespBytes(v, &len);
        buf_append_byte(b, 0);
        buf_append_u32(b, (uint32_t)len);
        buf_append(b, p, len);
    }
}

void wire_put_res(struct buf *b, res_t *r, int spec_only)
{
    /* first pass: count what we will write, so the count goes up front */
    uint16_t count = 0;
    for (int n = 0; n < NOUN__COUNT; n++) {
        if (!ResHas(r, n))
            continue;
        if (spec_only && !NounIsSpec(n))
            continue;
        count++;
    }
    buf_append_u16(b, count);

    for (int n = 0; n < NOUN__COUNT; n++) {
        if (!ResHas(r, n))
            continue;
        if (spec_only && !NounIsSpec(n))
            continue;
        resp_t *v = ResGet(r, n);
        buf_append_u16(b, (uint16_t)n);
        put_value(b, v);
        RespFree(v);
    }
}

res_t *wire_get_res(struct rdr *rd)
{
    uint16_t count = rd_u16(rd);
    if (rd->err)
        return NULL;

    res_t *r = ResCreate();
    for (uint16_t i = 0; i < count; i++) {
        uint16_t noun = rd_u16(rd);
        unsigned char is_int = 0;
        const unsigned char *tag = rd_bytes(rd, 1);
        if (tag != NULL)
            is_int = tag[0];
        if (rd->err || noun >= NOUN__COUNT) {
            ResDelete(r);
            return NULL;
        }
        if (is_int) {
            int64_t x = (int64_t)rd_u64(rd);
            if (rd->err)
                break;
            ResSetInt(r, noun, x);
        } else {
            uint32_t len = rd_u32(rd);
            const void *p = rd_bytes(rd, len);
            if (rd->err)
                break;
            ResSetBytes(r, noun, p, len);
        }
    }
    if (rd->err) {
        ResDelete(r);
        return NULL;
    }
    return r;
}

/* ===================================================== request codec */

void wire_put_req(struct buf *b, int verb, int noun,
                  const void *id, size_t idlen,
                  const void *data, size_t datalen)
{
    buf_append_byte(b, (unsigned char)verb);
    buf_append_u16(b, (uint16_t)noun);
    buf_append_u32(b, (uint32_t)idlen);
    buf_append(b, id, idlen);
    buf_append_u32(b, (uint32_t)datalen);
    if (datalen > 0)
        buf_append(b, data, datalen);
}

int wire_get_req(struct rdr *rd, int *verb, int *noun,
                 const void **id, size_t *idlen,
                 const void **data, size_t *datalen)
{
    const unsigned char *v = rd_bytes(rd, 1);
    if (v == NULL)
        return -1;
    *verb = v[0];
    *noun = rd_u16(rd);
    uint32_t il = rd_u32(rd);
    *id = rd_bytes(rd, il);
    *idlen = il;
    uint32_t dl = rd_u32(rd);
    *data = rd_bytes(rd, dl);
    *datalen = dl;
    if (rd->err)
        return -1;
    return 0;
}

void wire_put_resp(struct buf *b, int status, const resp_t *v)
{
    buf_append_byte(b, (unsigned char)status);
    if (v == NULL) {
        buf_append_byte(b, 0);      /* no value */
        return;
    }
    buf_append_byte(b, 1);          /* has value */
    put_value(b, v);
}

int wire_get_resp(struct rdr *rd, resp_t **v)
{
    *v = NULL;
    const unsigned char *st = rd_bytes(rd, 1);
    const unsigned char *has = rd_bytes(rd, 1);
    if (st == NULL || has == NULL)
        return W_ERR;
    if (has[0] == 0)
        return st[0];

    const unsigned char *tag = rd_bytes(rd, 1);
    if (tag == NULL)
        return W_ERR;
    if (tag[0]) {
        int64_t x = (int64_t)rd_u64(rd);
        if (!rd->err)
            *v = RespFromInt(x);
    } else {
        uint32_t len = rd_u32(rd);
        const void *p = rd_bytes(rd, len);
        if (!rd->err)
            *v = RespFromBytes(p, len);
    }
    if (rd->err)
        return W_ERR;
    return st[0];
}

/* ===================================================== transport */

static int conn_read(struct conn *c, void *buf, size_t n)
{
    if (c->ssl != NULL)
        return tls_read(c, buf, n);
    return read_full(c->fd, buf, n);
}

static int conn_write(struct conn *c, const void *buf, size_t n)
{
    if (c->ssl != NULL)
        return tls_write(c, buf, n);
    return write_full(c->fd, buf, n);
}

static void set_timeouts(int fd)
{
    struct timeval tv;
    tv.tv_sec = IO_TIMEOUT_MS / 1000;
    tv.tv_usec = (IO_TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static int fill_addr(struct sockaddr_in *sa, const char *ip, int port)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa->sin_addr) != 1)
        return -1;
    return 0;
}

int wire_listen(const char *ip, int port)
{
    struct sockaddr_in sa;
    if (fill_addr(&sa, ip, port) != 0) {
        ERR("bad listen address %s", ip);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        PERR("socket");
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        PERR("bind %s:%d", ip, port);
        close(fd);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        PERR("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int wire_accept(int lfd, struct conn *out)
{
    out->fd = -1;
    out->ssl = NULL;
    int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0)
        return -1;
    set_timeouts(fd);
    out->fd = fd;
    return 0;
}

int wire_accept_handshake(struct conn *c)
{
    return tls_server_handshake(c);
}

size_t wire_max_payload(int type)
{
    switch (type) {
    case MSG_APPLY:         /* carries an executable image */
    case MSG_BLOB_RESP:
        return MAX_MSG;
    case MSG_GOSSIP_SYN:    /* whole desired state: hashes, not images */
    case MSG_GOSSIP_ACK:
    case MSG_LIST_RESP:
        return 16u * 1024u * 1024u;
    default:                /* a request, a name, a hash, a status byte */
        return 1u * 1024u * 1024u;
    }
}

int wire_connect(const char *ip, int port, const char *server_name,
                 struct conn *out)
{
    out->fd = -1;
    out->ssl = NULL;
    struct sockaddr_in sa;
    if (fill_addr(&sa, ip, port) != 0)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    set_timeouts(fd);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    out->fd = fd;
    if (tls_client_handshake(out, server_name) != 0) {
        wire_close(out);
        return -1;
    }
    return 0;
}

int wire_send(struct conn *c, int type, const void *payload, size_t len)
{
    if (len + 1 > MAX_MSG)
        return -1;
    unsigned char hdr[5];
    uint32_t total = (uint32_t)(len + 1);
    hdr[0] = (unsigned char)(total >> 24);
    hdr[1] = (unsigned char)(total >> 16);
    hdr[2] = (unsigned char)(total >> 8);
    hdr[3] = (unsigned char)(total);
    hdr[4] = (unsigned char)type;
    if (conn_write(c, hdr, sizeof hdr) != 0)
        return -1;
    if (len > 0 && conn_write(c, payload, len) != 0)
        return -1;
    return 0;
}

int wire_recv(struct conn *c, int *type, struct buf *payload)
{
    unsigned char hdr[5];
    if (conn_read(c, hdr, sizeof hdr) != 0)
        return -1;
    uint32_t total = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                     ((uint32_t)hdr[2] << 8) | hdr[3];
    if (total < 1 || total > MAX_MSG)
        return -1;
    *type = hdr[4];
    size_t len = total - 1;
    if (len > wire_max_payload(*type))
        return -1;      /* this message type never needs that much */
    buf_reset(payload);
    buf_reserve(payload, len);
    if (len > 0 && conn_read(c, payload->data, len) != 0)
        return -1;
    payload->len = len;
    return 0;
}

void wire_close(struct conn *c)
{
    if (c->ssl != NULL)
        tls_free(c);
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->ssl = NULL;
}

int wire_call(const char *ip, int port, const char *server_name,
              int type, const void *payload, size_t len,
              int *rtype, struct buf *reply)
{
    struct conn c;
    if (wire_connect(ip, port, server_name, &c) != 0)
        return -1;
    int rc = -1;
    if (wire_send(&c, type, payload, len) == 0 &&
        wire_recv(&c, rtype, reply) == 0)
        rc = 0;
    wire_close(&c);
    return rc;
}
