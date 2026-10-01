#define _GNU_SOURCE
#include "membership.h"
#include "util.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* SWIM message types */
enum { SW_PING = 1, SW_ACK, SW_PINGREQ };

/* timings (ms) */
#define PERIOD_MS      1000     /* one fresh probe per second            */
#define PING_TIMEOUT   400      /* wait this long for a direct ack       */
#define INDIRECT_TMO   600      /* then this long for an indirect ack    */
#define SUSPECT_MS     4000     /* suspect -> dead                       */
#define DEAD_REAP_MS   20000    /* forget a dead member after this       */
#define INDIRECT_K     3        /* how many relays for an indirect probe */
#define PIGGYBACK_MAX  6        /* member updates glued onto each packet  */

#define MAXN 128

struct member {
    char name[MAXN];
    char ip[64];
    int udp_port;
    int tcp_port;
    int state;
    uint64_t incarnation;
    int64_t state_ms;
    int used;
};

static char g_self_name[MAXN];
static char g_self_ip[64];
static int g_self_udp;
static int g_self_tcp;
static uint64_t g_self_inc;

static struct member *g_mem;
static size_t g_cap;
static size_t g_len;

struct seed { char ip[64]; int udp; };
static struct seed g_seeds[16];
static int g_nseeds;
static int g_seed_cursor;

/* the single outstanding probe */
static int g_probe_active;
static int g_probe_phase;        /* 0 direct, 1 indirect */
static int64_t g_probe_sent;
static uint32_t g_probe_seq;
static char g_probe_ip[64];
static int g_probe_udp;
static char g_probe_name[MAXN];  /* "" if probing a bare seed address */
static int64_t g_last_period;

static uint32_t g_seq_next = 1;
static int g_diss_cursor;        /* rotates piggyback coverage */

/* forward table for relayed (indirect) acks */
struct fwd {
    uint32_t relay_seq;
    char oip[64];
    int oport;
    uint32_t oseq;
    int64_t ts;
    int used;
};
#define FWD_MAX 64
static struct fwd g_fwd[FWD_MAX];

/* ------------------------------------------------------------- members */

static struct member *mem_find(const char *name)
{
    for (size_t i = 0; i < g_len; i++)
        if (g_mem[i].used && strcmp(g_mem[i].name, name) == 0)
            return &g_mem[i];
    return NULL;
}

static struct member *mem_alloc(void)
{
    for (size_t i = 0; i < g_len; i++)
        if (!g_mem[i].used)
            return &g_mem[i];
    if (g_len == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 16;
        g_mem = xrealloc(g_mem, g_cap * sizeof *g_mem);
    }
    return &g_mem[g_len++];
}

/* apply one membership fact using SWIM precedence. Returns 1 if it changed us. */
static int mem_merge(const char *name, const char *ip, int udp, int tcp,
                     int state, uint64_t inc)
{
    if (strcmp(name, g_self_name) == 0) {
        /* someone has an opinion about us. If they think us gone, refute by
         * speaking with a strictly higher incarnation. */
        if (state != M_ALIVE && inc >= g_self_inc) {
            g_self_inc = inc + 1;
            INFO("refuting %s rumour, incarnation now %llu",
                 state == M_SUSPECT ? "suspect" : "dead",
                 (unsigned long long)g_self_inc);
            return 1;
        }
        return 0;
    }

    struct member *m = mem_find(name);
    if (m == NULL) {
        if (state == M_DEAD)
            return 0;               /* don't resurrect a stranger as dead */
        m = mem_alloc();
        memset(m, 0, sizeof *m);
        m->used = 1;
        snprintf(m->name, sizeof m->name, "%s", name);
        snprintf(m->ip, sizeof m->ip, "%s", ip);
        m->udp_port = udp;
        m->tcp_port = tcp;
        m->state = state;
        m->incarnation = inc;
        m->state_ms = monotime_ms();
        INFO("member %s@%s:%d joined (%s)", name, ip, udp,
             state == M_ALIVE ? "alive" : "suspect");
        return 1;
    }

    /* keep the freshest address we have seen */
    if (ip[0] != '\0') {
        snprintf(m->ip, sizeof m->ip, "%s", ip);
        if (udp > 0) m->udp_port = udp;
        if (tcp > 0) m->tcp_port = tcp;
    }

    /* precedence: higher incarnation always wins; at equal incarnation a worse
     * state wins (alive < suspect < dead), which lets suspicion escalate. */
    int changed = 0;
    if (inc > m->incarnation ||
        (inc == m->incarnation && state > m->state)) {
        if (m->state != state) {
            INFO("member %s %s -> %s", name,
                 m->state == M_ALIVE ? "alive" :
                 m->state == M_SUSPECT ? "suspect" : "dead",
                 state == M_ALIVE ? "alive" :
                 state == M_SUSPECT ? "suspect" : "dead");
            m->state = state;
            m->state_ms = monotime_ms();
            changed = 1;
        }
        m->incarnation = inc;
    }
    return changed;
}

/* ------------------------------------------------------------- packets */

static void put_header(struct buf *b, int type, uint32_t seq)
{
    buf_append_byte(b, (unsigned char)type);
    buf_append_u32(b, seq);
    buf_append_u16(b, (uint16_t)strlen(g_self_name));
    buf_append_str(b, g_self_name);
    buf_append_u16(b, (uint16_t)strlen(g_self_ip));
    buf_append_str(b, g_self_ip);
    buf_append_u16(b, (uint16_t)g_self_udp);
    buf_append_u16(b, (uint16_t)g_self_tcp);
    buf_append_u64(b, g_self_inc);
}

static void put_one_update(struct buf *b, const char *name, const char *ip,
                           int udp, int tcp, int state, uint64_t inc)
{
    buf_append_byte(b, (unsigned char)state);
    buf_append_u64(b, inc);
    buf_append_u16(b, (uint16_t)strlen(name));
    buf_append_str(b, name);
    buf_append_u16(b, (uint16_t)strlen(ip));
    buf_append_str(b, ip);
    buf_append_u16(b, (uint16_t)udp);
    buf_append_u16(b, (uint16_t)tcp);
}

/* glue a rotating sample of what we know onto the packet, so facts spread */
static void put_updates(struct buf *b)
{
    uint16_t n = 0;
    size_t slots = g_len + 1;        /* +1 for self */
    size_t budget = PIGGYBACK_MAX;
    if (budget > slots)
        budget = slots;

    /* reserve the count, patch it after */
    size_t count_at = b->len;
    buf_append_u16(b, 0);

    /* always include self: our own alive+incarnation is how we refute */
    put_one_update(b, g_self_name, g_self_ip, g_self_udp, g_self_tcp,
                   M_ALIVE, g_self_inc);
    n++;

    for (size_t i = 0; i + 1 < budget && g_len > 0; i++) {
        g_diss_cursor = (g_diss_cursor + 1) % (int)g_len;
        struct member *m = &g_mem[g_diss_cursor];
        if (!m->used)
            continue;
        put_one_update(b, m->name, m->ip, m->udp_port, m->tcp_port,
                       m->state, m->incarnation);
        n++;
    }

    b->data[count_at] = (unsigned char)(n >> 8);
    b->data[count_at + 1] = (unsigned char)(n);
}

static void send_to(int fd, const char *ip, int udp, struct buf *b)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)udp);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1)
        return;
    sendto(fd, b->data, b->len, 0, (struct sockaddr *)&sa, sizeof sa);
}

static void send_ping(int fd, const char *ip, int udp, uint32_t seq, int type)
{
    struct buf b;
    buf_init(&b);
    put_header(&b, type, seq);
    put_updates(&b);
    send_to(fd, ip, udp, &b);
    buf_free(&b);
}

static void send_pingreq(int fd, const char *relay_ip, int relay_udp,
                         const char *tname, const char *tip, int tudp,
                         uint32_t oseq)
{
    struct buf b;
    buf_init(&b);
    put_header(&b, SW_PINGREQ, oseq);
    buf_append_u16(&b, (uint16_t)strlen(tname));
    buf_append_str(&b, tname);
    buf_append_u16(&b, (uint16_t)strlen(tip));
    buf_append_str(&b, tip);
    buf_append_u16(&b, (uint16_t)tudp);
    put_updates(&b);
    send_to(fd, relay_ip, relay_udp, &b);
    buf_free(&b);
}

/* ------------------------------------------------------------- forward tbl */

static struct fwd *fwd_alloc(void)
{
    int64_t now = monotime_ms();
    for (int i = 0; i < FWD_MAX; i++)
        if (!g_fwd[i].used || now - g_fwd[i].ts > 2000)
            return &g_fwd[i];
    return &g_fwd[0];
}

/* ------------------------------------------------------------- receive */

static void read_updates(struct rdr *rd)
{
    uint16_t n = rd_u16(rd);
    for (uint16_t i = 0; i < n && !rd->err; i++) {
        int state = 0;
        const unsigned char *st = rd_bytes(rd, 1);
        if (st != NULL) state = st[0];
        uint64_t inc = rd_u64(rd);
        uint16_t nl = rd_u16(rd);
        const char *nm = rd_bytes(rd, nl);
        uint16_t il = rd_u16(rd);
        const char *ip = rd_bytes(rd, il);
        int udp = rd_u16(rd);
        int tcp = rd_u16(rd);
        if (rd->err || nm == NULL || ip == NULL)
            break;
        char name[MAXN], ipbuf[64];
        snprintf(name, sizeof name, "%.*s", (int)nl, nm);
        snprintf(ipbuf, sizeof ipbuf, "%.*s", (int)il, ip);
        mem_merge(name, ipbuf, udp, tcp, state, inc);
    }
}

static void handle_one(int fd, const unsigned char *pkt, size_t len,
                       const char *src_ip)
{
    struct rdr rd;
    rdr_init(&rd, pkt, len);

    const unsigned char *tp = rd_bytes(&rd, 1);
    if (tp == NULL)
        return;
    int type = tp[0];
    uint32_t seq = rd_u32(&rd);

    uint16_t nl = rd_u16(&rd);
    const char *nm = rd_bytes(&rd, nl);
    uint16_t il = rd_u16(&rd);
    const char *ip = rd_bytes(&rd, il);
    int fudp = rd_u16(&rd);
    int ftcp = rd_u16(&rd);
    uint64_t finc = rd_u64(&rd);
    if (rd.err || nm == NULL || ip == NULL)
        return;

    char from[MAXN], fip[64];
    snprintf(from, sizeof from, "%.*s", (int)nl, nm);
    snprintf(fip, sizeof fip, "%.*s", (int)il, ip);
    if (fip[0] == '\0')
        snprintf(fip, sizeof fip, "%s", src_ip);

    /* the sender is, evidently, alive */
    mem_merge(from, fip, fudp, ftcp, M_ALIVE, finc);

    if (type == SW_PING) {
        read_updates(&rd);
        send_ping(fd, fip, fudp, seq, SW_ACK);
        return;
    }

    if (type == SW_ACK) {
        /* is it for our own probe? */
        if (g_probe_active && seq == g_probe_seq) {
            g_probe_active = 0;
            if (g_probe_name[0] != '\0')
                mem_merge(g_probe_name, "", 0, 0, M_ALIVE, 0);
        }
        /* or for someone we relayed on behalf of? */
        for (int i = 0; i < FWD_MAX; i++) {
            if (g_fwd[i].used && g_fwd[i].relay_seq == seq) {
                struct buf b;
                buf_init(&b);
                put_header(&b, SW_ACK, g_fwd[i].oseq);
                put_updates(&b);
                send_to(fd, g_fwd[i].oip, g_fwd[i].oport, &b);
                buf_free(&b);
                g_fwd[i].used = 0;
                break;
            }
        }
        read_updates(&rd);
        return;
    }

    if (type == SW_PINGREQ) {
        uint16_t tnl = rd_u16(&rd);
        const char *tnm = rd_bytes(&rd, tnl);
        uint16_t tilen = rd_u16(&rd);
        const char *tip = rd_bytes(&rd, tilen);
        int tudp = rd_u16(&rd);
        if (rd.err || tnm == NULL || tip == NULL)
            return;
        char tname[MAXN], tipbuf[64];
        snprintf(tname, sizeof tname, "%.*s", (int)tnl, tnm);
        snprintf(tipbuf, sizeof tipbuf, "%.*s", (int)tilen, tip);
        read_updates(&rd);

        /* relay: ping the target on the requester's behalf, remember to
         * forward the target's ack back to the requester */
        struct fwd *f = fwd_alloc();
        f->used = 1;
        f->relay_seq = g_seq_next++;
        snprintf(f->oip, sizeof f->oip, "%s", fip);
        f->oport = fudp;
        f->oseq = seq;
        f->ts = monotime_ms();
        send_ping(fd, tipbuf, tudp, f->relay_seq, SW_PING);
        (void)tname;
        return;
    }
}

void membership_handle(int fd)
{
    unsigned char pkt[65536];
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        ssize_t n = recvfrom(fd, pkt, sizeof pkt, 0,
                             (struct sockaddr *)&sa, &sl);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;      /* EAGAIN: drained */
        }
        char src[64];
        inet_ntop(AF_INET, &sa.sin_addr, src, sizeof src);
        handle_one(fd, pkt, (size_t)n, src);
    }
}

/* ------------------------------------------------------------- tick */

static int pick_alive_peer(struct member **out)
{
    int alive_idx[256];
    int k = 0;
    for (size_t i = 0; i < g_len && k < 256; i++)
        if (g_mem[i].used && g_mem[i].state != M_DEAD)
            alive_idx[k++] = (int)i;
    if (k == 0)
        return 0;
    *out = &g_mem[alive_idx[rand() % k]];
    return 1;
}

static void start_probe(int fd)
{
    struct member *t = NULL;
    if (pick_alive_peer(&t)) {
        snprintf(g_probe_name, sizeof g_probe_name, "%s", t->name);
        snprintf(g_probe_ip, sizeof g_probe_ip, "%s", t->ip);
        g_probe_udp = t->udp_port;
    } else if (g_nseeds > 0) {
        /* nobody known yet: knock on a seed's door to join */
        g_seed_cursor = (g_seed_cursor + 1) % g_nseeds;
        g_probe_name[0] = '\0';
        snprintf(g_probe_ip, sizeof g_probe_ip, "%s", g_seeds[g_seed_cursor].ip);
        g_probe_udp = g_seeds[g_seed_cursor].udp;
    } else {
        return;     /* alone, no seeds: nothing to probe */
    }
    g_probe_seq = g_seq_next++;
    g_probe_phase = 0;
    g_probe_sent = monotime_ms();
    g_probe_active = 1;
    send_ping(fd, g_probe_ip, g_probe_udp, g_probe_seq, SW_PING);
}

static void begin_indirect(int fd)
{
    int sent = 0;
    for (size_t i = 0; i < g_len && sent < INDIRECT_K; i++) {
        struct member *m = &g_mem[i];
        if (!m->used || m->state == M_DEAD)
            continue;
        if (g_probe_name[0] != '\0' && strcmp(m->name, g_probe_name) == 0)
            continue;
        send_pingreq(fd, m->ip, m->udp_port,
                     g_probe_name[0] ? g_probe_name : "?",
                     g_probe_ip, g_probe_udp, g_probe_seq);
        sent++;
    }
    g_probe_phase = 1;
    g_probe_sent = monotime_ms();
    if (sent == 0) {
        /* no relays available: conclude directly */
        if (g_probe_name[0] != '\0') {
            struct member *m = mem_find(g_probe_name);
            if (m != NULL && m->state == M_ALIVE) {
                m->state = M_SUSPECT;
                m->state_ms = monotime_ms();
                INFO("member %s alive -> suspect (no ack)", m->name);
            }
        }
        g_probe_active = 0;
    }
}

void membership_tick(int fd)
{
    int64_t now = monotime_ms();

    /* escalate suspects to dead, and forget long-dead members */
    for (size_t i = 0; i < g_len; i++) {
        struct member *m = &g_mem[i];
        if (!m->used)
            continue;
        if (m->state == M_SUSPECT && now - m->state_ms > SUSPECT_MS) {
            m->state = M_DEAD;
            m->state_ms = now;
            INFO("member %s suspect -> dead", m->name);
        } else if (m->state == M_DEAD && now - m->state_ms > DEAD_REAP_MS) {
            INFO("forgetting dead member %s", m->name);
            m->used = 0;
        }
    }

    if (g_probe_active) {
        if (g_probe_phase == 0 && now - g_probe_sent > PING_TIMEOUT) {
            begin_indirect(fd);
        } else if (g_probe_phase == 1 && now - g_probe_sent > INDIRECT_TMO) {
            if (g_probe_name[0] != '\0') {
                struct member *m = mem_find(g_probe_name);
                if (m != NULL && m->state == M_ALIVE) {
                    m->state = M_SUSPECT;
                    m->state_ms = now;
                    INFO("member %s alive -> suspect (no indirect ack)", m->name);
                }
            }
            g_probe_active = 0;
        }
    }

    if (!g_probe_active && now - g_last_period > PERIOD_MS) {
        g_last_period = now;
        start_probe(fd);
    }
}

/* ------------------------------------------------------------- setup */

void membership_init(const char *self_name, const char *self_ip,
                     int udp_port, int tcp_port)
{
    snprintf(g_self_name, sizeof g_self_name, "%s", self_name);
    snprintf(g_self_ip, sizeof g_self_ip, "%s", self_ip);
    g_self_udp = udp_port;
    g_self_tcp = tcp_port;
    g_self_inc = 1;
    srand((unsigned)(getpid() ^ (int)time(NULL)));
}

int membership_udp_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        PERR("socket udp");
        return -1;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_self_udp);
    if (inet_pton(AF_INET, g_self_ip, &sa.sin_addr) != 1) {
        ERR("bad self ip %s", g_self_ip);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        PERR("bind udp %s:%d", g_self_ip, g_self_udp);
        close(fd);
        return -1;
    }
    return fd;
}

void membership_seed(const char *ip, int udp_port)
{
    if (g_nseeds >= (int)(sizeof g_seeds / sizeof g_seeds[0]))
        return;
    snprintf(g_seeds[g_nseeds].ip, sizeof g_seeds[g_nseeds].ip, "%s", ip);
    g_seeds[g_nseeds].udp = udp_port;
    g_nseeds++;
}

/* ------------------------------------------------------------- views */

static int by_name(const void *a, const void *b)
{
    const struct member_view *x = a;
    const struct member_view *y = b;
    return strcmp(x->name, y->name);
}

int membership_snapshot(struct member_view *out, int max)
{
    int n = 0;
    if (n < max) {
        out[n].name = g_self_name;
        out[n].ip = g_self_ip;
        out[n].udp_port = g_self_udp;
        out[n].tcp_port = g_self_tcp;
        out[n].state = M_ALIVE;
        n++;
    }
    for (size_t i = 0; i < g_len && n < max; i++) {
        struct member *m = &g_mem[i];
        if (!m->used || m->state == M_DEAD)
            continue;
        out[n].name = m->name;
        out[n].ip = m->ip;
        out[n].udp_port = m->udp_port;
        out[n].tcp_port = m->tcp_port;
        out[n].state = m->state;
        n++;
    }
    qsort(out, n, sizeof *out, by_name);
    return n;
}

int membership_random_peers(struct member_view *out, int max)
{
    int idx[256];
    int k = 0;
    for (size_t i = 0; i < g_len && k < 256; i++)
        if (g_mem[i].used && g_mem[i].state != M_DEAD)
            idx[k++] = (int)i;
    /* Fisher-Yates partial shuffle */
    for (int i = 0; i < k; i++) {
        int j = i + rand() % (k - i);
        int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    int n = 0;
    for (int i = 0; i < k && n < max; i++) {
        struct member *m = &g_mem[idx[i]];
        out[n].name = m->name;
        out[n].ip = m->ip;
        out[n].udp_port = m->udp_port;
        out[n].tcp_port = m->tcp_port;
        out[n].state = m->state;
        n++;
    }
    return n;
}

int membership_alive_count(void)
{
    int n = 1;      /* self */
    for (size_t i = 0; i < g_len; i++)
        if (g_mem[i].used && g_mem[i].state != M_DEAD)
            n++;
    return n;
}

const char *membership_self_name(void)
{
    return g_self_name;
}

void membership_foreach(void (*fn)(const struct member_view *m, void *ctx),
                        void *ctx)
{
    struct member_view v;
    v.name = g_self_name;
    v.ip = g_self_ip;
    v.udp_port = g_self_udp;
    v.tcp_port = g_self_tcp;
    v.state = M_ALIVE;
    fn(&v, ctx);
    for (size_t i = 0; i < g_len; i++) {
        struct member *m = &g_mem[i];
        if (!m->used)
            continue;
        v.name = m->name;
        v.ip = m->ip;
        v.udp_port = m->udp_port;
        v.tcp_port = m->tcp_port;
        v.state = m->state;
        fn(&v, ctx);
    }
}
