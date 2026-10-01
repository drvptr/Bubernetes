#ifndef MEMBERSHIP_H
#define MEMBERSHIP_H

#include <stddef.h>

/*
 * Membership, by SWIM.
 *
 * Every node keeps a list of the others and a guess at whether each is alive.
 * There is no coordinator doing the watching: once per period a node pings one
 * random peer; if that peer is quiet, the node asks a few others to ping it too
 * (an indirect probe, which rules out a one-off dropped packet or a link that is
 * bad only between those two); only if everyone fails to reach it does the peer
 * become suspected, and after a grace period, dead. News of these changes rides
 * piggybacked on the same ping/ack traffic, so the whole cluster converges on
 * one view without a broadcast storm.
 *
 * Identity is the name, not the address. A node ruled dead can come back under
 * the same name from a new machine; an incarnation number lets a node refute a
 * false rumour of its own death by speaking with a higher number.
 *
 * This module is deliberately independent of the resource store. The daemon
 * mirrors the alive set into NODE resources so the rest of the system can treat
 * a node like any other resource, but membership itself knows nothing about that.
 */

enum { M_ALIVE, M_SUSPECT, M_DEAD };

struct member_view {
    const char *name;
    const char *ip;
    int udp_port;
    int tcp_port;
    int state;
};

/* set up self identity. self_ip must be a numeric IPv4. */
void membership_init(const char *self_name, const char *self_ip,
                     int udp_port, int tcp_port);

/* create and bind the SWIM UDP socket (non-blocking). Returns fd or -1. */
int  membership_udp_socket(void);

/* remember a seed peer to contact so this node can join an existing cluster. */
void membership_seed(const char *ip, int udp_port);

/* drain and process all pending datagrams on fd. */
void membership_handle(int fd);

/* advance the protocol state machine; call it often (e.g. every 200ms). */
void membership_tick(int fd);

/* snapshot of the alive set (including self), sorted by name for determinism.
 * Returns the count written (<= max). This is what the scheduler hashes over. */
int  membership_snapshot(struct member_view *out, int max);

/* up to max random alive peers, excluding self. For gossip and blob fetch. */
int  membership_random_peers(struct member_view *out, int max);

int  membership_alive_count(void);
const char *membership_self_name(void);

/* visit every member (any state); for `bubectl get nodes` mirroring. */
void membership_foreach(void (*fn)(const struct member_view *m, void *ctx),
                        void *ctx);

#endif
