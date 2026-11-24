#ifndef DEJAVU_ROUTER_TLS_TRANSPORT_H
#define DEJAVU_ROUTER_TLS_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>
#include <event2/buffer.h>
#include <event2/event.h>

typedef struct tls_transport tls_transport_t;

typedef struct {
    const char *host;
    const char *sni;
    const char *ca_file;
    bool insecure_skip_verify;
    int connect_timeout_secs;
    void (*on_connected)(void *ctx);
    void (*on_read)(struct evbuffer *input, void *ctx);
    void (*on_error)(const char *reason, void *ctx);
    void *ctx;
} tls_transport_config_t;

bool tls_transport_global_init(void);

tls_transport_t *tls_transport_connect(struct event_base *base,
                                       const struct sockaddr *addr,
                                       socklen_t addrlen,
                                       const tls_transport_config_t *cfg);

int tls_transport_write(tls_transport_t *tls, const void *data, size_t len);
size_t tls_transport_output_len(const tls_transport_t *tls);
void tls_transport_set_timeout(tls_transport_t *tls, int timeout_secs);
int tls_transport_get_fd(const tls_transport_t *tls);
const char *tls_transport_version_str(const tls_transport_t *tls);
void tls_transport_free(tls_transport_t *tls);

#endif /* DEJAVU_ROUTER_TLS_TRANSPORT_H */
