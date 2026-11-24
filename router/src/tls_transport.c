#define _GNU_SOURCE
#include "tls_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#ifdef MSG_NOSIGNAL
#define TLS_SEND_FLAGS MSG_NOSIGNAL
#else
#define TLS_SEND_FLAGS 0
#endif

enum tls_state {
    TLS_STATE_TCP_CONNECTING = 0,
    TLS_STATE_HANDSHAKING = 1,
    TLS_STATE_OPEN = 2,
    TLS_STATE_FAILED = 3
};

struct tls_transport {
    struct event_base *base;
    int fd;
    enum tls_state state;
    struct event *read_ev;
    struct event *write_ev;
    struct event *timer_ev;
    int timeout_secs;
    struct evbuffer *in_buf;
    struct evbuffer *out_buf;

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool ssl_initialized;
    bool want_write;
    bool want_read_for_write;

    void (*on_connected)(void *ctx);
    void (*on_read)(struct evbuffer *input, void *ctx);
    void (*on_error)(const char *reason, void *ctx);
    void *ctx;

    bool in_callback;
    bool pending_free;
};

static bool g_psa_ready = false;

bool tls_transport_global_init(void) {
    if (g_psa_ready) return true;
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    g_psa_ready = true;
    return true;
}

static bool is_ip_literal(const char *host) {
    if (!host || !host[0]) return false;
    struct in_addr v4;
    struct in6_addr v6;
    return inet_pton(AF_INET, host, &v4) == 1 || inet_pton(AF_INET6, host, &v6) == 1;
}

static int tls_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    tls_transport_t *tls = (tls_transport_t *)ctx;
    if (tls->fd < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    ssize_t n;
    do {
        n = send(tls->fd, buf, len, TLS_SEND_FLAGS);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)n;
}

static int tls_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    tls_transport_t *tls = (tls_transport_t *)ctx;
    if (tls->fd < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    ssize_t n;
    do {
        n = recv(tls->fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (n == 0) return 0;
    return (int)n;
}

static void tls_destroy_now(tls_transport_t *tls) {
    if (!tls) return;
    if (tls->read_ev) event_free(tls->read_ev);
    if (tls->write_ev) event_free(tls->write_ev);
    if (tls->timer_ev) event_free(tls->timer_ev);
    if (tls->ssl_initialized) {
        mbedtls_ssl_free(&tls->ssl);
        mbedtls_ssl_config_free(&tls->conf);
        mbedtls_x509_crt_free(&tls->cacert);
        mbedtls_ctr_drbg_free(&tls->ctr_drbg);
        mbedtls_entropy_free(&tls->entropy);
    }
    if (tls->fd >= 0) close(tls->fd);
    if (tls->in_buf) evbuffer_free(tls->in_buf);
    if (tls->out_buf) evbuffer_free(tls->out_buf);
    free(tls);
}

static void tls_reset_timer(tls_transport_t *tls) {
    if (!tls || !tls->timer_ev || tls->pending_free) return;
    if (tls->timeout_secs > 0) {
        struct timeval tv = { .tv_sec = tls->timeout_secs, .tv_usec = 0 };
        evtimer_add(tls->timer_ev, &tv);
    } else {
        evtimer_del(tls->timer_ev);
    }
}

static void tls_update_events(tls_transport_t *tls) {
    if (!tls || tls->pending_free || tls->state == TLS_STATE_FAILED) return;
    if (tls->state == TLS_STATE_TCP_CONNECTING) {
        if (tls->read_ev) event_del(tls->read_ev);
        if (tls->write_ev) event_add(tls->write_ev, NULL);
        return;
    }
    if (tls->read_ev) event_add(tls->read_ev, NULL);
    bool need_write = tls->want_write ||
                      (!tls->want_read_for_write && tls->state == TLS_STATE_OPEN &&
                       tls->out_buf && evbuffer_get_length(tls->out_buf) > 0);
    if (tls->write_ev) {
        if (need_write) event_add(tls->write_ev, NULL);
        else event_del(tls->write_ev);
    }
}

static void apply_tcp_liveness(int fd) {
    int keepalive = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
#ifdef __linux__
    int idle = 30;
    int intvl = 10;
    int cnt = 3;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#ifdef TCP_USER_TIMEOUT
    unsigned int user_timeout_ms = 20000u;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof(user_timeout_ms));
#endif
#endif
}

static void tls_fail(tls_transport_t *tls, const char *reason) {
    if (!tls || tls->state == TLS_STATE_FAILED || tls->pending_free) return;
    tls->state = TLS_STATE_FAILED;
    if (tls->read_ev) event_del(tls->read_ev);
    if (tls->write_ev) event_del(tls->write_ev);
    if (tls->timer_ev) evtimer_del(tls->timer_ev);
    if (tls->on_error) tls->on_error(reason ? reason : "tls_error", tls->ctx);
}

static void tls_drive(tls_transport_t *tls, short trigger_events) {
    if (!tls || tls->pending_free || tls->state == TLS_STATE_FAILED) return;
    tls->in_callback = true;
    tls->want_write = false;
    if (trigger_events & EV_READ) {
        tls->want_read_for_write = false;
    }

    if (tls->state == TLS_STATE_TCP_CONNECTING) {
        int err = 0;
        socklen_t errlen = sizeof(err);
        if (getsockopt(tls->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 || err != 0) {
            tls_fail(tls, "tcp_connect_failed");
            goto done;
        }
        tls->state = TLS_STATE_HANDSHAKING;
    }

    if (tls->state == TLS_STATE_HANDSHAKING) {
        int rc = mbedtls_ssl_handshake(&tls->ssl);
        if (rc == 0) {
            tls->state = TLS_STATE_OPEN;
            tls_reset_timer(tls);
            if (tls->on_connected) tls->on_connected(tls->ctx);
            if (tls->pending_free || tls->state == TLS_STATE_FAILED) goto done;
        } else if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
            tls_update_events(tls);
            goto done;
        } else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            tls->want_write = true;
            tls_update_events(tls);
            goto done;
        } else {
            char errbuf[128];
            mbedtls_strerror(rc, errbuf, sizeof(errbuf));
            tls_fail(tls, errbuf);
            goto done;
        }
    }

    if (tls->state == TLS_STATE_OPEN) {
        /* 1) Flush queued outbound plaintext through TLS (never resets read-liveness timer). */
        while (!tls->want_read_for_write && tls->out_buf && evbuffer_get_length(tls->out_buf) > 0) {
            size_t avail = evbuffer_get_length(tls->out_buf);
            size_t chunk = avail > 16384 ? 16384 : avail;
            unsigned char *ptr = evbuffer_pullup(tls->out_buf, (ev_ssize_t)chunk);
            if (!ptr) break;
            int rc = mbedtls_ssl_write(&tls->ssl, ptr, chunk);
            if (rc > 0) {
                evbuffer_drain(tls->out_buf, (size_t)rc);
                tls->want_read_for_write = false;
            } else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                tls->want_write = true;
                break;
            } else if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
                tls->want_read_for_write = true;
                break;
            } else {
                char errbuf[128];
                mbedtls_strerror(rc, errbuf, sizeof(errbuf));
                tls_fail(tls, errbuf);
                goto done;
            }
        }

        /* 2) Drain decrypted inbound records (only peer reads reset liveness timeout). */
        bool got_data = false;
        for (;;) {
            unsigned char tmp[16384];
            int rc = mbedtls_ssl_read(&tls->ssl, tmp, sizeof(tmp));
            if (rc > 0) {
                if (evbuffer_add(tls->in_buf, tmp, (size_t)rc) != 0) {
                    tls_fail(tls, "tls_oom");
                    goto done;
                }
                got_data = true;
                tls->want_read_for_write = false;
                tls_reset_timer(tls);
            } else if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
                break;
            } else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                tls->want_write = true;
                break;
            } else if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                if (got_data && tls->on_read && evbuffer_get_length(tls->in_buf) > 0) {
                    tls->on_read(tls->in_buf, tls->ctx);
                    if (tls->pending_free || tls->state == TLS_STATE_FAILED) goto done;
                }
                tls_fail(tls, "tls_eof");
                goto done;
            } else {
                char errbuf[128];
                mbedtls_strerror(rc, errbuf, sizeof(errbuf));
                tls_fail(tls, errbuf);
                goto done;
            }
        }

        if (got_data && tls->on_read && evbuffer_get_length(tls->in_buf) > 0) {
            tls->on_read(tls->in_buf, tls->ctx);
            if (tls->pending_free || tls->state == TLS_STATE_FAILED) goto done;
        }

        /* Flush any response bytes queued by on_read immediately */
        while (!tls->want_read_for_write && tls->out_buf && evbuffer_get_length(tls->out_buf) > 0) {
            size_t avail = evbuffer_get_length(tls->out_buf);
            size_t chunk = avail > 16384 ? 16384 : avail;
            unsigned char *ptr = evbuffer_pullup(tls->out_buf, (ev_ssize_t)chunk);
            if (!ptr) break;
            int rc = mbedtls_ssl_write(&tls->ssl, ptr, chunk);
            if (rc > 0) {
                evbuffer_drain(tls->out_buf, (size_t)rc);
                tls->want_read_for_write = false;
            } else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                tls->want_write = true;
                break;
            } else if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
                tls->want_read_for_write = true;
                break;
            } else {
                char errbuf[128];
                mbedtls_strerror(rc, errbuf, sizeof(errbuf));
                tls_fail(tls, errbuf);
                goto done;
            }
        }

        tls_update_events(tls);
    }

done:
    tls->in_callback = false;
    if (tls->pending_free) tls_destroy_now(tls);
}

static void tls_io_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    tls_drive((tls_transport_t *)arg, events);
}

static void tls_timeout_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    tls_transport_t *tls = (tls_transport_t *)arg;
    tls->in_callback = true;
    tls_fail(tls, "tls_timeout");
    tls->in_callback = false;
    if (tls->pending_free) tls_destroy_now(tls);
}

static bool load_ca_certs(mbedtls_x509_crt *cacert, const char *ca_file) {
    if (ca_file && ca_file[0]) {
        struct stat st;
        if (stat(ca_file, &st) != 0) return false;
        if (S_ISDIR(st.st_mode)) {
            return mbedtls_x509_crt_parse_path(cacert, ca_file) >= 0 && cacert->raw.p != NULL;
        }
        return mbedtls_x509_crt_parse_file(cacert, ca_file) == 0 && cacert->raw.p != NULL;
    }
#ifdef __ANDROID__
    /* Android 14+ stores system root CAs under APEX Conscrypt; older versions use /system.
     * parse_path returns >= 0 even on an empty directory, so verify at least one CA loaded. */
    (void)mbedtls_x509_crt_parse_path(cacert, "/apex/com.android.conscrypt/cacerts");
    (void)mbedtls_x509_crt_parse_path(cacert, "/system/etc/security/cacerts");
    return cacert->raw.p != NULL;
#else
    static const char *const candidates[] = {
        "/etc/ssl/cert.pem",
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(*candidates); i++) {
        if (access(candidates[i], R_OK) == 0 &&
            mbedtls_x509_crt_parse_file(cacert, candidates[i]) == 0 &&
            cacert->raw.p != NULL) {
            return true;
        }
    }
    return false;
#endif
}

tls_transport_t *tls_transport_connect(struct event_base *base,
                                       const struct sockaddr *addr,
                                       socklen_t addrlen,
                                       const tls_transport_config_t *cfg) {
    if (!base || !addr || !cfg || !tls_transport_global_init()) return NULL;

    tls_transport_t *tls = calloc(1, sizeof(*tls));
    if (!tls) return NULL;
    tls->base = base;
    tls->fd = -1;
    tls->timeout_secs = cfg->connect_timeout_secs > 0 ? cfg->connect_timeout_secs : 10;
    tls->on_connected = cfg->on_connected;
    tls->on_read = cfg->on_read;
    tls->on_error = cfg->on_error;
    tls->ctx = cfg->ctx;

    tls->in_buf = evbuffer_new();
    tls->out_buf = evbuffer_new();
    if (!tls->in_buf || !tls->out_buf) { tls_destroy_now(tls); return NULL; }

    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->conf);
    mbedtls_x509_crt_init(&tls->cacert);
    mbedtls_entropy_init(&tls->entropy);
    mbedtls_ctr_drbg_init(&tls->ctr_drbg);
    tls->ssl_initialized = true;

    const char *pers = "dejavuerouter-tls";
    if (mbedtls_ctr_drbg_seed(&tls->ctr_drbg, mbedtls_entropy_func, &tls->entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        tls_destroy_now(tls);
        return NULL;
    }

    if (mbedtls_ssl_config_defaults(&tls->conf,
                                    MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        tls_destroy_now(tls);
        return NULL;
    }
    mbedtls_ssl_conf_min_tls_version(&tls->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&tls->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->ctr_drbg);

    if (cfg->insecure_skip_verify) {
        mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
    } else {
        if (!load_ca_certs(&tls->cacert, cfg->ca_file)) {
            tls_destroy_now(tls);
            return NULL;
        }
        mbedtls_ssl_conf_ca_chain(&tls->conf, &tls->cacert, NULL);
        mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    }

    if (mbedtls_ssl_setup(&tls->ssl, &tls->conf) != 0) {
        tls_destroy_now(tls);
        return NULL;
    }

    const char *sni = (cfg->sni && cfg->sni[0]) ? cfg->sni :
                      (cfg->host && cfg->host[0] && !is_ip_literal(cfg->host) ? cfg->host : NULL);
    if (sni && mbedtls_ssl_set_hostname(&tls->ssl, sni) != 0) {
        tls_destroy_now(tls);
        return NULL;
    }

    int fd = socket(addr->sa_family, SOCK_STREAM, 0);
    if (fd < 0) { tls_destroy_now(tls); return NULL; }
    evutil_make_socket_closeonexec(fd);
    evutil_make_socket_nonblocking(fd);
    apply_tcp_liveness(fd);
    tls->fd = fd;

    mbedtls_ssl_set_bio(&tls->ssl, tls, tls_bio_send, tls_bio_recv, NULL);

    int rc = connect(fd, addr, addrlen);
    if (rc == 0) {
        tls->state = TLS_STATE_HANDSHAKING;
    } else if (errno == EINPROGRESS) {
        tls->state = TLS_STATE_TCP_CONNECTING;
    } else {
        tls_destroy_now(tls);
        return NULL;
    }

    tls->read_ev = event_new(base, fd, EV_READ | EV_PERSIST, tls_io_cb, tls);
    tls->write_ev = event_new(base, fd, EV_WRITE | EV_PERSIST, tls_io_cb, tls);
    tls->timer_ev = evtimer_new(base, tls_timeout_cb, tls);
    if (!tls->read_ev || !tls->write_ev || !tls->timer_ev) {
        tls_destroy_now(tls);
        return NULL;
    }

    tls_reset_timer(tls);
    if (tls->state == TLS_STATE_HANDSHAKING) {
        /* Kick off TLS ClientHello on next loop tick to avoid synchronous reentrancy. */
        tls->want_write = true;
    }
    tls_update_events(tls);
    return tls;
}

int tls_transport_write(tls_transport_t *tls, const void *data, size_t len) {
    if (!tls || tls->pending_free || tls->state != TLS_STATE_OPEN || !tls->out_buf) return -1;
    if (len == 0) return 0;
    if (evbuffer_add(tls->out_buf, data, len) != 0) return -1;
    if (!tls->in_callback) {
        tls_drive(tls, 0);
    } else {
        tls_update_events(tls);
    }
    return 0;
}

size_t tls_transport_output_len(const tls_transport_t *tls) {
    return (tls && tls->out_buf) ? evbuffer_get_length(tls->out_buf) : 0;
}

void tls_transport_set_timeout(tls_transport_t *tls, int timeout_secs) {
    if (!tls || tls->pending_free) return;
    tls->timeout_secs = timeout_secs;
    tls_reset_timer(tls);
}

int tls_transport_get_fd(const tls_transport_t *tls) {
    return tls ? tls->fd : -1;
}

const char *tls_transport_version_str(const tls_transport_t *tls) {
    if (!tls || !tls->ssl_initialized || tls->state != TLS_STATE_OPEN) return "none";
    return mbedtls_ssl_get_version(&tls->ssl);
}

void tls_transport_free(tls_transport_t *tls) {
    if (!tls) return;
    if (tls->in_callback) {
        tls->pending_free = true;
        tls->state = TLS_STATE_FAILED;
        if (tls->read_ev) event_del(tls->read_ev);
        if (tls->write_ev) event_del(tls->write_ev);
        if (tls->timer_ev) evtimer_del(tls->timer_ev);
        return;
    }
    tls_destroy_now(tls);
}
