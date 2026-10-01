#define _GNU_SOURCE
#include "tls.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#ifndef BUBE_TLS

/* ------------------------------------------------------------------------
 * Plain build. The transport is cleartext TCP; these keep the call sites in
 * wire.c unconditional. A connection's ssl is never set, so tls_read/tls_write
 * are never reached - they exist only so the program links.
 * ---------------------------------------------------------------------- */

int tls_setup(const char *certdir)
{
    (void)certdir;
    return 0;
}

int tls_server_handshake(struct conn *c)
{
    (void)c;
    return 0;
}

int tls_client_handshake(struct conn *c, const char *server_name)
{
    (void)c;
    (void)server_name;
    return 0;
}

void tls_free(struct conn *c)
{
    (void)c;
}

int tls_read(struct conn *c, void *buf, size_t n)
{
    return read_full(c->fd, buf, n);
}

int tls_write(struct conn *c, const void *buf, size_t n)
{
    return write_full(c->fd, buf, n);
}

#else /* BUBE_TLS */

/* ------------------------------------------------------------------------
 * OpenSSL build: mutual-auth TLS between peers.
 *
 * There is no server node and no client node, so every bubelet holds both
 * roles with one context: it presents node.pem/node.key and demands a cert from
 * the other side, verified against ca.pem. Membership in the cluster is
 * therefore "holds a cert signed by the cluster CA" - which is also what lets a
 * replacement machine step into an existing node's identity.
 * ---------------------------------------------------------------------- */

#include <openssl/ssl.h>
#include <openssl/err.h>

static SSL_CTX *g_ctx;

static void log_ssl_error(const char *what)
{
    unsigned long e = ERR_get_error();
    char msg[256];
    ERR_error_string_n(e, msg, sizeof msg);
    ERR("%s: %s", what, msg);
}

int tls_setup(const char *certdir)
{
    char ca[512], crt[512], key[512];
    snprintf(ca, sizeof ca, "%s/ca.pem", certdir);
    snprintf(crt, sizeof crt, "%s/node.pem", certdir);
    snprintf(key, sizeof key, "%s/node.key", certdir);

    g_ctx = SSL_CTX_new(TLS_method());
    if (g_ctx == NULL) {
        log_ssl_error("SSL_CTX_new");
        return -1;
    }
    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_file(g_ctx, crt, SSL_FILETYPE_PEM) != 1) {
        log_ssl_error(crt);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(g_ctx, key, SSL_FILETYPE_PEM) != 1) {
        log_ssl_error(key);
        return -1;
    }
    if (SSL_CTX_load_verify_locations(g_ctx, ca, NULL) != 1) {
        log_ssl_error(ca);
        return -1;
    }
    /* mutual: we always verify the peer, and refuse peers with no cert */
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       NULL);
    INFO("mutual TLS enabled (certs in %s)", certdir);
    return 0;
}

int tls_server_handshake(struct conn *c)
{
    if (g_ctx == NULL)
        return 0;           /* TLS compiled in but not configured: plain */
    SSL *ssl = SSL_new(g_ctx);
    SSL_set_fd(ssl, c->fd);
    if (SSL_accept(ssl) != 1) {
        log_ssl_error("SSL_accept");
        SSL_free(ssl);
        return -1;
    }
    c->ssl = ssl;
    return 0;
}

int tls_client_handshake(struct conn *c, const char *server_name)
{
    if (g_ctx == NULL)
        return 0;
    SSL *ssl = SSL_new(g_ctx);
    SSL_set_fd(ssl, c->fd);
    if (server_name != NULL && server_name[0] != '\0') {
        SSL_set_tlsext_host_name(ssl, server_name);
        SSL_set1_host(ssl, server_name);    /* cert must name this node */
    }
    if (SSL_connect(ssl) != 1) {
        log_ssl_error("SSL_connect");
        SSL_free(ssl);
        return -1;
    }
    c->ssl = ssl;
    return 0;
}

void tls_free(struct conn *c)
{
    if (c->ssl == NULL)
        return;
    SSL_shutdown(c->ssl);
    SSL_free(c->ssl);
    c->ssl = NULL;
}

int tls_read(struct conn *c, void *buf, size_t n)
{
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        int r = SSL_read(c->ssl, p + got, (int)(n - got));
        if (r <= 0) {
            int e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_ZERO_RETURN)
                return 1;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

int tls_write(struct conn *c, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t sent = 0;
    while (sent < n) {
        int w = SSL_write(c->ssl, p + sent, (int)(n - sent));
        if (w <= 0)
            return -1;
        sent += (size_t)w;
    }
    return 0;
}

#endif /* BUBE_TLS */
