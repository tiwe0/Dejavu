#define _GNU_SOURCE
#include <arpa/inet.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/listener.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"

#define MAX_LINE (1024 * 1024)
#define MAX_PLUGINS 512
#define MAX_JOBS 8192

typedef struct router router_t;
typedef struct plugin plugin_t;
typedef struct job job_t;

struct plugin {
    router_t *router;
    struct bufferevent *bev;
    char *app;
    char **routes;
    size_t route_count;
    bool registered;
    plugin_t *next;
};

struct job {
    router_t *router;
    char *id;
    char *route;
    plugin_t *plugin;
    struct event *timer;
    job_t *next;
};

struct http_ctx {
    router_t *router;
    struct evhttp_connection *conn;
    char *tag;
};

struct router {
    struct event_base *base;
    struct evdns_base *dns;
    struct evconnlistener *listener;
    char *hub_scheme;
    char *hub_host;
    int hub_port;
    char *hub_prefix;
    char *device_id;
    char *device_token;
    int pollers;
    int poll_wait;
    int pollers_running;
    plugin_t *plugins;
    size_t plugin_count;
    job_t *jobs;
    size_t job_count;
};

static void logmsg(const char *level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "dejavurouter: %s ", level);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    fflush(stderr);
}

static void signal_cb(evutil_socket_t sig, short events, void *arg) {
    (void)sig;
    (void)events;
    router_t *router = arg;
    logmsg("INFO", "stopping");
    event_base_loopexit(router->base, NULL);
}

static void free_http_ctx(struct http_ctx *ctx) {
    if (!ctx) return;
    if (ctx->conn) evhttp_connection_free(ctx->conn);
    free(ctx->tag);
    free(ctx);
}

/* ------------------------------------------------------------------------- */
/* Plugin registry                                                           */

static plugin_t *find_plugin(router_t *router, const char *route) {
    for (plugin_t *p = router->plugins; p; p = p->next) {
        if (!p->registered) continue;
        for (size_t i = 0; i < p->route_count; i++) {
            if (!strcmp(p->routes[i], route)) return p;
        }
    }
    return NULL;
}


static int plugin_send_json(plugin_t *plugin, cJSON *root) {
    if (!plugin || !plugin->bev || !root) return -1;
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return -1;
    size_t length = strlen(text);
    int rc = bufferevent_write(plugin->bev, text, length);
    if (rc == 0) rc = bufferevent_write(plugin->bev, "\n", 1);
    free(text);
    return rc;
}

static void plugin_unregister(plugin_t *plugin) {
    plugin->registered = false;
}

static void free_plugin(plugin_t *plugin) {
    if (!plugin) return;
    if (plugin->bev) bufferevent_free(plugin->bev);
    free(plugin->app);
    for (size_t i = 0; i < plugin->route_count; i++) free(plugin->routes[i]);
    free(plugin->routes);
    free(plugin);
}

static void detach_plugin(router_t *router, plugin_t *plugin) {
    plugin_t **slot = &router->plugins;
    while (*slot && *slot != plugin) slot = &(*slot)->next;
    if (*slot == plugin) *slot = plugin->next;
    if (router->plugin_count > 0) router->plugin_count--;
}

/* ------------------------------------------------------------------------- */
/* Hub HTTP client                                                           */

static char *join_path(const router_t *router, const char *path) {
    const char *prefix = router->hub_prefix && *router->hub_prefix ? router->hub_prefix : "";
    size_t plen = strlen(prefix);
    size_t slen = strlen(path);
    char *out = malloc(plen + slen + 2);
    if (!out) return NULL;
    if (plen && prefix[plen - 1] == '/') {
        snprintf(out, plen + slen + 2, "%s%s", prefix, path[0] == '/' ? path + 1 : path);
    } else {
        snprintf(out, plen + slen + 2, "%s%s", prefix, path[0] == '/' ? path : "/");
    }
    return out;
}

static struct evhttp_connection *new_http_connection(router_t *router) {
    struct evhttp_connection *conn = evhttp_connection_base_bufferevent_new(
        router->base, router->dns, NULL, router->hub_host, (ev_uint16_t)router->hub_port);
    return conn;
}

static void poller_launch(router_t *router);
static void hello_launch(router_t *router);

static void poller_launch_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    poller_launch(arg);
}

static void hello_launch_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    hello_launch(arg);
}

static void result_done(struct evhttp_request *req, void *arg) {
    struct http_ctx *ctx = arg;
    (void)req;
    free_http_ctx(ctx);
}

static void post_result(router_t *router, cJSON *root) {
    char *body = cJSON_PrintUnformatted(root);
    if (!body) return;
    struct http_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { free(body); return; }
    ctx->router = router;
    ctx->conn = new_http_connection(router);
    if (!ctx->conn) { free_http_ctx(ctx); free(body); return; }
    evhttp_connection_set_timeout(ctx->conn, 20);
    struct evhttp_request *req = evhttp_request_new(result_done, ctx);
    if (!req) { free_http_ctx(ctx); free(body); return; }
    struct evkeyvalq *headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(headers, "Content-Type", "application/json");
    if (router->device_token && *router->device_token) {
        char auth[1024];
        snprintf(auth, sizeof(auth), "Bearer %s", router->device_token);
        evhttp_add_header(headers, "Authorization", auth);
    }
    struct evbuffer *buffer = evhttp_request_get_output_buffer(req);
    evbuffer_add(buffer, body, strlen(body));
    char *path = join_path(router, "/device/result");
    free(body);
    if (!path) { evhttp_request_free(req); free_http_ctx(ctx); return; }
    if (evhttp_make_request(ctx->conn, req, EVHTTP_REQ_POST, path) != 0) {
        free(path);
        evhttp_request_free(req);
        free_http_ctx(ctx);
        return;
    }
    free(path);
}

static void send_error(router_t *router, const char *job_id, const char *code, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", router->device_id);
    cJSON_AddStringToObject(root, "id", job_id ? job_id : "");
    cJSON_AddBoolToObject(root, "ok", 0);
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddBoolToObject(error, "retryable", 1);
    cJSON_AddItemToObject(root, "error", error);
    post_result(router, root);
    cJSON_Delete(root);
}

static void send_plugin_result(router_t *router, const char *job_id, cJSON *response) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", router->device_id);
    cJSON_AddStringToObject(root, "id", job_id ? job_id : "");
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
    bool success = cJSON_IsTrue(ok);
    cJSON_AddBoolToObject(root, "ok", success);
    if (success) {
        cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
        if (result) cJSON_AddItemReferenceToObject(root, "result", result);
        else cJSON_AddObjectToObject(root, "result");
    } else {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
        if (error) cJSON_AddItemReferenceToObject(root, "error", error);
        else {
            cJSON *created = cJSON_CreateObject();
            cJSON_AddStringToObject(created, "code", "plugin_error");
            cJSON_AddStringToObject(created, "message", "plugin returned an error");
            cJSON_AddBoolToObject(created, "retryable", 1);
            cJSON_AddItemToObject(root, "error", created);
        }
    }
    post_result(router, root);
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------------- */
/* Job tracking                                                              */

static void job_remove(router_t *router, job_t *job) {
    job_t **slot = &router->jobs;
    while (*slot && *slot != job) slot = &(*slot)->next;
    if (*slot == job) *slot = job->next;
    if (router->job_count > 0) router->job_count--;
}

static void job_free(job_t *job) {
    if (!job) return;
    if (job->timer) event_free(job->timer);
    free(job->id);
    free(job->route);
    free(job);
}

static job_t *find_job(router_t *router, const char *id) {
    for (job_t *job = router->jobs; job; job = job->next) {
        if (!strcmp(job->id, id)) return job;
    }
    return NULL;
}

static void job_timeout_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    job_t *job = arg;
    router_t *router = job->router;
    send_error(router, job->id, "plugin_timeout", "plugin response timed out");
    job_remove(router, job);
    job_free(job);
}

static void job_complete(router_t *router, job_t *job, cJSON *response) {
    send_plugin_result(router, job->id, response);
    job_remove(router, job);
    job_free(job);
}

/* ------------------------------------------------------------------------- */
/* Plugin connection callbacks                                               */

static void plugin_event_cb(struct bufferevent *bev, short events, void *arg);

static void plugin_handle_message(plugin_t *plugin, const char *line, size_t length) {
    cJSON *root = cJSON_ParseWithLength(line, length);
    if (!root) return;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) { cJSON_Delete(root); return; }
    if (!strcmp(type->valuestring, "hello")) {
        if (plugin->registered) { cJSON_Delete(root); return; }
        cJSON *app = cJSON_GetObjectItemCaseSensitive(root, "app");
        cJSON *routes = cJSON_GetObjectItemCaseSensitive(root, "routes");
        plugin->app = strdup(cJSON_IsString(app) ? app->valuestring : "");
        if (!plugin->app) { cJSON_Delete(root); return; }
        size_t count = cJSON_IsArray(routes) ? (size_t)cJSON_GetArraySize(routes) : 0;
        if (count > 64) count = 64;
        plugin->routes = calloc(count ? count : 1, sizeof(char *));
        if (!plugin->routes) { cJSON_Delete(root); return; }
        for (size_t i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(routes, (int)i);
            if (cJSON_IsString(item)) {
                char *route = strdup(item->valuestring);
                if (route) plugin->routes[plugin->route_count++] = route;
            }
        }
        plugin->registered = true;
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "hello_ok");
        cJSON_AddNumberToObject(reply, "version", 1);
        plugin_send_json(plugin, reply);
        cJSON_Delete(reply);
        logmsg("INFO", "plugin connected app=%s routes=%zu", plugin->app, plugin->route_count);
    } else if (!strcmp(type->valuestring, "response")) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (!cJSON_IsString(id)) { cJSON_Delete(root); return; }
        job_t *job = find_job(plugin->router, id->valuestring);
        if (job && job->plugin == plugin) job_complete(plugin->router, job, root);
    } else if (!strcmp(type->valuestring, "ping")) {
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "pong");
        plugin_send_json(plugin, reply);
        cJSON_Delete(reply);
    }
    cJSON_Delete(root);
}

static void plugin_read_cb(struct bufferevent *bev, void *arg) {
    plugin_t *plugin = arg;
    struct evbuffer *input = bufferevent_get_input(bev);
    if (evbuffer_get_length(input) > MAX_LINE * 2) {
        logmsg("WARN", "plugin input exceeds limit");
        bufferevent_free(bev);
        return;
    }
    while (1) {
        size_t length = 0;
        char *line = evbuffer_readln(input, &length, EVBUFFER_EOL_LF);
        if (!line) break;
        if (length > MAX_LINE) { free(line); continue; }
        plugin_handle_message(plugin, line, length);
        free(line);
    }
}

static void plugin_event_cb(struct bufferevent *bev, short events, void *arg) {
    plugin_t *plugin = arg;
    router_t *router = plugin->router;
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        plugin_unregister(plugin);
        for (job_t *job = router->jobs; job; ) {
            job_t *next = job->next;
            if (job->plugin == plugin) {
                send_error(router, job->id, "plugin_disconnected", "plugin disconnected");
                job_remove(router, job);
                job_free(job);
            }
            job = next;
        }
        detach_plugin(router, plugin);
        logmsg("INFO", "plugin disconnected app=%s", plugin->app ? plugin->app : "");
        free_plugin(plugin);
        (void)bev;
    }
}

static void listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
                        struct sockaddr *address, int socklen, void *arg) {
    (void)listener;
    (void)address;
    (void)socklen;
    router_t *router = arg;
    if (router->plugin_count >= MAX_PLUGINS) {
        close(fd);
        return;
    }
    plugin_t *plugin = calloc(1, sizeof(*plugin));
    if (!plugin) { close(fd); return; }
    plugin->router = router;
    struct bufferevent *bev = bufferevent_socket_new(router->base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) { free(plugin); close(fd); return; }
    plugin->bev = bev;
    plugin->next = router->plugins;
    router->plugins = plugin;
    router->plugin_count++;
    bufferevent_setcb(bev, plugin_read_cb, NULL, plugin_event_cb, plugin);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

/* ------------------------------------------------------------------------- */
/* Hub jobs                                                                  */

static void dispatch_job(router_t *router, cJSON *job_json) {
    cJSON *id = cJSON_GetObjectItemCaseSensitive(job_json, "id");
    cJSON *route = cJSON_GetObjectItemCaseSensitive(job_json, "route");
    if (!cJSON_IsString(id) || !cJSON_IsString(route)) {
        send_error(router, cJSON_IsString(id) ? id->valuestring : "", "invalid_job", "job needs id and route");
        return;
    }
    if (router->job_count >= MAX_JOBS) {
        send_error(router, id->valuestring, "router_busy", "too many in-flight jobs");
        return;
    }
    plugin_t *plugin = find_plugin(router, route->valuestring);
    if (!plugin) {
        send_error(router, id->valuestring, "route_unavailable", "no plugin registered for route");
        return;
    }
    job_t *job = calloc(1, sizeof(*job));
    if (!job) { send_error(router, id->valuestring, "router_oom", "out of memory"); return; }
    job->router = router;
    job->id = strdup(id->valuestring);
    job->route = strdup(route->valuestring);
    job->plugin = plugin;
    if (!job->id || !job->route) { job_free(job); send_error(router, id->valuestring, "router_oom", "out of memory"); return; }
    job->next = router->jobs;
    router->jobs = job;
    router->job_count++;
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "type", "request");
    cJSON_AddStringToObject(request, "id", job->id);
    cJSON_AddStringToObject(request, "route", job->route);
    cJSON *method = cJSON_GetObjectItemCaseSensitive(job_json, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(job_json, "params");
    if (method) cJSON_AddItemReferenceToObject(request, "method", method);
    if (params) cJSON_AddItemReferenceToObject(request, "params", params);
    if (plugin_send_json(plugin, request) != 0) {
        cJSON_Delete(request);
        send_error(router, job->id, "plugin_send_failed", "could not write to plugin");
        job_remove(router, job);
        job_free(job);
        return;
    }
    cJSON_Delete(request);
    cJSON *deadline = cJSON_GetObjectItemCaseSensitive(job_json, "deadline_ms");
    double seconds = 60.0;
    if (cJSON_IsNumber(deadline) && deadline->valuedouble > 0) seconds = deadline->valuedouble / 1000.0;
    job->timer = evtimer_new(router->base, job_timeout_cb, job);
    if (job->timer) {
        struct timeval tv = { .tv_sec = (time_t)seconds,
                              .tv_usec = (suseconds_t)((seconds - (time_t)seconds) * 1000000) };
        evtimer_add(job->timer, &tv);
    }
}

/* ------------------------------------------------------------------------- */
/* Hub polling                                                               */

static void poller_callback(struct evhttp_request *req, void *arg);

static void schedule_poller(router_t *router) {
    struct timeval tv = {0, 0};
    event_base_once(router->base, -1, EV_TIMEOUT, poller_launch_cb, router, &tv);
}

static void poller_callback(struct evhttp_request *req, void *arg) {
    struct http_ctx *ctx = arg;
    router_t *router = ctx->router;
    int code = req ? evhttp_request_get_response_code(req) : 0;
    if (code == 200) {
        struct evbuffer *input = evhttp_request_get_input_buffer(req);
        size_t length = evbuffer_get_length(input);
        if (length > 0 && length <= MAX_LINE * 4) {
            char *body = malloc(length + 1);
            if (body) {
                evbuffer_copyout(input, body, length);
                body[length] = 0;
                cJSON *root = cJSON_ParseWithLength(body, length);
                if (root) {
                    cJSON *job = cJSON_GetObjectItemCaseSensitive(root, "job");
                    if (cJSON_IsObject(job)) dispatch_job(router, job);
                    cJSON_Delete(root);
                }
                free(body);
            }
        }
    } else if (code != 204 && code != 0) {
        logmsg("WARN", "poll HTTP %d", code);
    }
    free_http_ctx(ctx);
    router->pollers_running--;
    schedule_poller(router);
}

static void poller_launch(router_t *router) {
    if (router->pollers_running >= router->pollers) return;
    char path[2048];
    snprintf(path, sizeof(path), "/device/poll?device_id=%s&wait=%d",
             router->device_id, router->poll_wait);
    struct http_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return;
    ctx->router = router;
    ctx->conn = new_http_connection(router);
    if (!ctx->conn) { free_http_ctx(ctx); return; }
    evhttp_connection_set_timeout(ctx->conn, router->poll_wait + 10);
    struct evhttp_request *req = evhttp_request_new(poller_callback, ctx);
    if (!req) { free_http_ctx(ctx); return; }
    if (router->device_token && *router->device_token) {
        char auth[1024];
        snprintf(auth, sizeof(auth), "Bearer %s", router->device_token);
        evhttp_add_header(evhttp_request_get_output_headers(req), "Authorization", auth);
    }
    char *full_path = join_path(router, path);
    if (!full_path) { evhttp_request_free(req); free_http_ctx(ctx); return; }
    router->pollers_running++;
    if (evhttp_make_request(ctx->conn, req, EVHTTP_REQ_GET, full_path) != 0) {
        router->pollers_running--;
        evhttp_request_free(req);
        free_http_ctx(ctx);
    }
    free(full_path);
}

static void start_pollers_cb(struct evhttp_request *req, void *arg) {
    struct http_ctx *ctx = arg;
    router_t *router = ctx->router;
    int code = req ? evhttp_request_get_response_code(req) : 0;
    free_http_ctx(ctx);
    if (code / 100 != 2) {
        logmsg("WARN", "hello failed HTTP %d", code);
        struct timeval tv = {2, 0};
        event_base_once(router->base, -1, EV_TIMEOUT, hello_launch_cb, router, &tv);
        return;
    }
    logmsg("INFO", "registered device=%s", router->device_id);
    for (int i = 0; i < router->pollers; i++) poller_launch(router);
    /* Launch one additional poller whenever one finishes. */
}

static void hello_launch(router_t *router) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", router->device_id);
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddArrayToObject(root, "accounts");
    cJSON *caps = cJSON_AddArrayToObject(root, "capabilities");
    cJSON_AddItemToArray(caps, cJSON_CreateString("router.plugin"));
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return;
    struct http_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { free(body); return; }
    ctx->router = router;
    ctx->conn = new_http_connection(router);
    if (!ctx->conn) { free(body); free_http_ctx(ctx); return; }
    evhttp_connection_set_timeout(ctx->conn, 20);
    struct evhttp_request *req = evhttp_request_new(start_pollers_cb, ctx);
    if (!req) { free(body); free_http_ctx(ctx); return; }
    struct evkeyvalq *headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(headers, "Content-Type", "application/json");
    if (router->device_token && *router->device_token) {
        char auth[1024];
        snprintf(auth, sizeof(auth), "Bearer %s", router->device_token);
        evhttp_add_header(headers, "Authorization", auth);
    }
    evbuffer_add(evhttp_request_get_output_buffer(req), body, strlen(body));
    char *path = join_path(router, "/device/hello");
    free(body);
    if (!path) { evhttp_request_free(req); free_http_ctx(ctx); return; }
    if (evhttp_make_request(ctx->conn, req, EVHTTP_REQ_POST, path) != 0) {
        evhttp_request_free(req);
        free_http_ctx(ctx);
    }
    free(path);
}

/* ------------------------------------------------------------------------- */
/* Config and main                                                           */

typedef struct {
    char *hub_url;
    char *device_id;
    char *device_token;
    char *listen_host;
    int listen_port;
    int pollers;
    int poll_wait;
} config_t;

static void config_free(config_t *cfg) {
    free(cfg->hub_url);
    free(cfg->device_id);
    free(cfg->device_token);
    free(cfg->listen_host);
    memset(cfg, 0, sizeof(*cfg));
}

static int config_load(const char *path, config_t *cfg, char *err, size_t errcap) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(err, errcap, "cannot open %s", path); return -1; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size < 0 || size > 4 * 1024 * 1024) { fclose(fp); snprintf(err, errcap, "config too large"); return -1; }
    rewind(fp);
    char *text = malloc((size_t)size + 1);
    if (!text) { fclose(fp); snprintf(err, errcap, "out of memory"); return -1; }
    size_t got = fread(text, 1, (size_t)size, fp);
    fclose(fp);
    text[got] = 0;
    cJSON *root = cJSON_ParseWithLength(text, got);
    free(text);
    if (!root || !cJSON_IsObject(root)) { cJSON_Delete(root); snprintf(err, errcap, "config must be a JSON object"); return -1; }
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_port = 39111;
    cfg->pollers = 8;
    cfg->poll_wait = 25;
    cfg->listen_host = strdup("127.0.0.1");
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "hub_url");
    if (!cJSON_IsString(item)) { cJSON_Delete(root); snprintf(err, errcap, "hub_url is required"); return -1; }
    cfg->hub_url = strdup(item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    cfg->device_id = strdup(cJSON_IsString(item) ? item->valuestring : "dejavu-device");
    item = cJSON_GetObjectItemCaseSensitive(root, "device_token");
    if (cJSON_IsString(item)) cfg->device_token = strdup(item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(root, "listen_host");
    if (cJSON_IsString(item)) { free(cfg->listen_host); cfg->listen_host = strdup(item->valuestring); }
    item = cJSON_GetObjectItemCaseSensitive(root, "listen_port");
    if (cJSON_IsNumber(item)) cfg->listen_port = item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(root, "pollers");
    if (cJSON_IsNumber(item)) cfg->pollers = item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(root, "poll_wait_seconds");
    if (cJSON_IsNumber(item)) cfg->poll_wait = item->valueint;
    cJSON_Delete(root);
    if (!cfg->hub_url || !cfg->device_id || !cfg->listen_host) {
        free(cfg->hub_url); free(cfg->device_id); free(cfg->listen_host);
        snprintf(err, errcap, "out of memory"); return -1;
    }
    if (cfg->pollers < 1) cfg->pollers = 1;
    if (cfg->pollers > 64) cfg->pollers = 64;
    if (cfg->poll_wait < 1) cfg->poll_wait = 1;
    if (cfg->poll_wait > 60) cfg->poll_wait = 60;
    return 0;
}


int main(int argc, char **argv) {
    const char *config_path = argc > 1 ? argv[1] : "config.json";
    config_t cfg;
    char err[512] = {0};
    if (config_load(config_path, &cfg, err, sizeof(err)) != 0) { logmsg("ERROR", "%s", err); return 2; }
    if (strncmp(cfg.hub_url, "http://", 7) != 0) {
        logmsg("ERROR", "only http:// hub URLs are supported (network security is out of scope)");
        config_free(&cfg);
        return 2;
    }
    router_t router;
    memset(&router, 0, sizeof(router));
    router.base = event_base_new();
    router.dns = evdns_base_new(router.base, 1);
    if (!router.base || !router.dns) { logmsg("ERROR", "cannot initialize libevent"); return 1; }
    struct evhttp_uri *uri = evhttp_uri_parse(cfg.hub_url);
    if (!uri || !evhttp_uri_get_host(uri)) { logmsg("ERROR", "invalid hub_url"); return 1; }
    router.hub_host = strdup(evhttp_uri_get_host(uri));
    router.hub_port = evhttp_uri_get_port(uri);
    if (router.hub_port <= 0) router.hub_port = 80;
    const char *path = evhttp_uri_get_path(uri);
    router.hub_prefix = strdup(path ? path : "");
    evhttp_uri_free(uri);
    router.device_id = cfg.device_id;
    router.device_token = cfg.device_token;
    router.pollers = cfg.pollers;
    router.poll_wait = cfg.poll_wait;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg.listen_port);
    if (inet_pton(AF_INET, cfg.listen_host, &addr.sin_addr) != 1) {
        logmsg("ERROR", "invalid listen_host");
        return 1;
    }
    router.listener = evconnlistener_new_bind(
        router.base, listener_cb, &router,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, 128,
        (struct sockaddr *)&addr, sizeof(addr));
    if (!router.listener) { logmsg("ERROR", "cannot bind plugin listener"); return 1; }
    logmsg("INFO", "plugin listener %s:%d", cfg.listen_host, cfg.listen_port);
    struct event *sigint = evsignal_new(router.base, SIGINT, signal_cb, &router);
    struct event *sigterm = evsignal_new(router.base, SIGTERM, signal_cb, &router);
    event_add(sigint, NULL);
    event_add(sigterm, NULL);
    hello_launch(&router);
    event_base_dispatch(router.base);
    evconnlistener_free(router.listener);
    event_free(sigint);
    event_free(sigterm);
    evdns_base_free(router.dns, 0);
    event_base_free(router.base);
    free(router.hub_host);
    free(router.hub_prefix);
    config_free(&cfg);
    return 0;
}