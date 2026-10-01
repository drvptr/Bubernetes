#ifndef TLS_H
#define TLS_H

#include "wire.h"
#include <stddef.h>

/*
 * The TLS seam. Built plain (the default), every function here is a no-op and a
 * connection's ssl stays NULL, so the transport is cleartext TCP and the binary
 * links with nothing but libc. Built with BUBE_TLS, these wrap OpenSSL and the
 * node-to-node channel becomes mutual-auth TLS: each node is both a client and
 * a server, presenting node.pem/node.key and verifying peers against ca.pem in
 * the cert directory. Nothing above wire.c changes either way.
 */
int  tls_setup(const char *certdir);                 /* 0 ok (no-op if plain) */
int  tls_server_handshake(struct conn *c);
int  tls_client_handshake(struct conn *c, const char *server_name);
void tls_free(struct conn *c);
int  tls_read(struct conn *c, void *buf, size_t n);  /* 0 ok, 1 eof, -1 err */
int  tls_write(struct conn *c, const void *buf, size_t n);  /* 0 ok, -1 err */

#endif
