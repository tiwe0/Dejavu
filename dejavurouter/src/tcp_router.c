#define _GNU_SOURCE
#include <arpa/inet.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <netdb.h>
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

struct router {
    struct event_base *base;
    struct evconnlistener *listener;
    struct bufferevent *hub_bev;
    struct event *reconnect_timer;
    bool hub_connected;
    bool stopping;
    int reconnect_seconds;
    char *hub_host;
    int hub_port;
    char *device_id;
    char *device_token;
    char *listen_host;
    int listen_port;
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
    router->stopping = true;
    logmsg("INFO", "stopping");
    event_base_loopexit(router->base, NULL);
}

/* ------------------------------------------------------------------------- */
/* Plugin registry                                                           */

static plugin_t *find_plugin(router_t *router, const char *route) {
    for (plugin_t *p = router->plugins; p; p = p->next) {
        if (!p->registered) continue;
        for (size_t i = 0; i < p->route_count; i++) {
            if (p->routes[i] && !strcmp(p->routes[i], route)) return p;
        }
    }
    return NULL;
}

static int plugin_send_json(plugin_t *plugin, cJSON *root) {
    if (!plugin || !plugin->bev || !root) return -1;
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return -1;
    int rc = bufferevent_write(plugin->bev, text, strlen(text));
    if (rc == 0) rc = bufferevent_write(plugin->bev, "\n", 1);
    free(text);
    return rc;
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
/* Hub TCP connection                                                        */

static void hub_connect(router_t *router);
static void dispatch_job(router_t *router, cJSON *job_json);

static void reconnect_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    hub_connect(arg);
}

static void schedule_hub_reconnect(router_t *router, int seconds) {
    if (router->reconnect_timer) return;
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    router->reconnect_timer = evtimer_new(router->base, reconnect_cb, router);
    if (router->reconnect_timer) evtimer_add(router->reconnect_timer, &tv);
}

static int hub_send_json(router_t *router, cJSON *root) {
    if (!router->hub_connected || !router->hub_bev || !root) return -1;
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return -1;
    int rc = bufferevent_write(router->hub_bev, text, strlen(text));
    if (rc == 0) rc = bufferevent_write(router->hub_bev, "\n", 1);
    free(text);
    return rc;
}

static void hub_route_event(router_t *router, const char *action, const char *route,
                            const char *app) {
    if (!router->hub_connected || !route) return;
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "route");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "route", route);
    cJSON_AddStringToObject(root, "device_id", router->device_id);
    if (app) cJSON_AddStringToObject(root, "app", app);
    hub_send_json(router, root);
    cJSON_Delete(root);
}

static void send_hub_response(router_t *router, const char *job_id, const char *route,
                              bool ok, cJSON *result_or_error) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddStringToObject(root, "id", job_id ? job_id : "");
    if (route) cJSON_AddStringToObject(root, "route", route);
    cJSON_AddBoolToObject(root, "ok", ok);
    if (ok) {
        if (result_or_error) cJSON_AddItemReferenceToObject(root, "result", result_or_error);
        else cJSON_AddObjectToObject(root, "result");
    } else {
        if (result_or_error) cJSON_AddItemReferenceToObject(root, "error", result_or_error);
        else {
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "code", "router_error");
            cJSON_AddStringToObject(error, "message", "router error");
            cJSON_AddBoolToObject(error, "retryable", 1);
            cJSON_AddItemToObject(root, "error", error);
        }
    }
    hub_send_json(router, root);
    cJSON_Delete(root);
}

static void send_hub_error(router_t *router, const char *job_id, const char *route,
                           const char *code, const char *message) {
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddBoolToObject(error, "retryable", 1);
    send_hub_response(router, job_id, route, false, error);
    cJSON_Delete(error);
}

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

static void job_timeout_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    job_t *job = arg;
    send_hub_error(job->router, job->id, job->route, "plugin_timeout", "plugin response timed out");
    job_remove(job->router, job);
    job_free(job);
}

static void job_complete(router_t *router, job_t *job, cJSON *response) {
    cJSON *response_route = cJSON_GetObjectItemCaseSensitive(response, "route");
    if (!cJSON_IsString(response_route) || strcmp(response_route->valuestring, job->route)) {
        send_hub_error(router, job->id, job->route, "route_mismatch",
                       "plugin response route does not match request route");
        job_remove(router, job);
        job_free(job);
        return;
    }
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
    bool success = cJSON_IsTrue(ok);
    if (success) {
        send_hub_response(router, job->id, job->route, true,
                          cJSON_GetObjectItemCaseSensitive(response, "result"));
    } else {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(response, "error");
        if (!error) {
            error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "code", "plugin_error");
            cJSON_AddStringToObject(error, "message", "plugin returned an error");
            cJSON_AddBoolToObject(error, "retryable", 1);
            send_hub_response(router, job->id, job->route, false, error);
            cJSON_Delete(error);
        } else {
            send_hub_response(router, job->id, job->route, false, error);
        }
    }
    job_remove(router, job);
    job_free(job);
}

static job_t *find_job(router_t *router, const char *id) {
    for (job_t *job = router->jobs; job; job = job->next) {
        if (job->id && !strcmp(job->id, id)) return job;
    }
    return NULL;
}

static void drop_all_jobs(router_t *router) {
    job_t *job = router->jobs;
    while (job) {
        job_t *next = job->next;
        job_free(job);
        job = next;
    }
    router->jobs = NULL;
    router->job_count = 0;
}

static void hub_disconnect(router_t *router) {
    router->hub_connected = false;
    if (router->hub_bev) {
        bufferevent_free(router->hub_bev);
        router->hub_bev = NULL;
    }
    drop_all_jobs(router);
}

static void hub_event_cb(struct bufferevent *bev, short events, void *arg) {
    (void)bev;
    router_t *router = arg;
    if (events & BEV_EVENT_CONNECTED) {
        router->hub_connected = true;
        router->reconnect_seconds = 1;
        cJSON *hello = cJSON_CreateObject();
        cJSON_AddStringToObject(hello, "type", "hello");
        cJSON_AddStringToObject(hello, "device_id", router->device_id);
        cJSON_AddNumberToObject(hello, "version", 1);
        if (router->device_token && *router->device_token) {
            cJSON_AddStringToObject(hello, "token", router->device_token);
        }
        cJSON *caps = cJSON_AddArrayToObject(hello, "capabilities");
        cJSON_AddItemToArray(caps, cJSON_CreateString("router.plugin"));
        hub_send_json(router, hello);
        cJSON_Delete(hello);
        logmsg("INFO", "hub connected %s:%d", router->hub_host, router->hub_port);
        /* Let the server learn which local routes are currently online. */
        for (plugin_t *p = router->plugins; p; p = p->next) {
            if (!p->registered) continue;
            for (size_t i = 0; i < p->route_count; i++) {
                hub_route_event(router, "register", p->routes[i], p->app);
            }
        }
        return;
    }
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        logmsg("WARN", "hub disconnected; reconnecting");
        hub_disconnect(router);
        schedule_hub_reconnect(router, router->reconnect_seconds);
        if (router->reconnect_seconds < 30) router->reconnect_seconds *= 2;
    }
}

static void dispatch_job(router_t *router, cJSON *job_json);

static void hub_read_cb(struct bufferevent *bev, void *arg) {
    router_t *router = arg;
    struct evbuffer *input = bufferevent_get_input(bev);
    if (evbuffer_get_length(input) > MAX_LINE * 2) {
        logmsg("WARN", "hub input exceeds limit; reconnecting");
        hub_disconnect(router);
        return;
    }
    while (1) {
        size_t length = 0;
        char *line = evbuffer_readln(input, &length, EVBUFFER_EOL_LF);
        if (!line) break;
        if (length > MAX_LINE) { free(line); continue; }
        cJSON *root = cJSON_ParseWithLength(line, length);
        free(line);
        if (!root) continue;
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        if (cJSON_IsString(type) && !strcmp(type->valuestring, "request")) {
            dispatch_job(router, root);
        } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "ping")) {
            cJSON *pong = cJSON_CreateObject();
            cJSON_AddStringToObject(pong, "type", "pong");
            hub_send_json(router, pong);
            cJSON_Delete(pong);
        }
        cJSON_Delete(root);
    }
}

static void hub_connect(router_t *router) {
    if (router->stopping || router->hub_bev) return;
    if (router->reconnect_timer) {
        event_free(router->reconnect_timer);
        router->reconnect_timer = NULL;
    }
    struct addrinfo hints;
    struct addrinfo *resolved = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char service[16];
    snprintf(service, sizeof(service), "%d", router->hub_port);
    if (getaddrinfo(router->hub_host, service, &hints, &resolved) != 0 || !resolved) {
        logmsg("WARN", "cannot resolve hub %s", router->hub_host);
        schedule_hub_reconnect(router, router->reconnect_seconds);
        if (router->reconnect_seconds < 30) router->reconnect_seconds *= 2;
        if (resolved) freeaddrinfo(resolved);
        return;
    }
    struct bufferevent *bev = bufferevent_socket_new(router->base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) { freeaddrinfo(resolved); schedule_hub_reconnect(router, 2); return; }
    bufferevent_setcb(bev, hub_read_cb, NULL, hub_event_cb, router);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
    router->hub_bev = bev;
    if (bufferevent_socket_connect(bev, resolved->ai_addr, (int)resolved->ai_addrlen) != 0) {
        logmsg("WARN", "hub connect failed");
        hub_disconnect(router);
        schedule_hub_reconnect(router, router->reconnect_seconds);
        if (router->reconnect_seconds < 30) router->reconnect_seconds *= 2;
    }
    freeaddrinfo(resolved);
}

/* ------------------------------------------------------------------------- */
/* Jobs and plugin forwarding                                                */

static void dispatch_job(router_t *router, cJSON *job_json) {
    cJSON *id = cJSON_GetObjectItemCaseSensitive(job_json, "id");
    cJSON *route = cJSON_GetObjectItemCaseSensitive(job_json, "route");
    if (!cJSON_IsString(id) || !cJSON_IsString(route)) {
        send_hub_error(router, cJSON_IsString(id) ? id->valuestring : "",
                       cJSON_IsString(route) ? route->valuestring : "", "invalid_job",
                       "job needs id and route");
        return;
    }
    if (router->job_count >= MAX_JOBS) {
        send_hub_error(router, id->valuestring, route->valuestring, "router_busy",
                       "too many in-flight jobs");
        return;
    }
    plugin_t *plugin = find_plugin(router, route->valuestring);
    if (!plugin) {
        send_hub_error(router, id->valuestring, route->valuestring, "route_unavailable",
                       "no plugin registered for route");
        return;
    }
    job_t *job = calloc(1, sizeof(*job));
    if (!job) { send_hub_error(router, id->valuestring, route->valuestring, "router_oom",
                              "out of memory"); return; }
    job->router = router;
    job->id = strdup(id->valuestring);
    job->route = strdup(route->valuestring);
    job->plugin = plugin;
    if (!job->id || !job->route) {
        job_free(job);
        send_hub_error(router, id->valuestring, route->valuestring, "router_oom",
                       "out of memory");
        return;
    }
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
        send_hub_error(router, job->id, job->route, "plugin_send_failed",
                       "could not write to plugin");
        job_remove(router, job);
        job_free(job);
        return;
    }
    cJSON_Delete(request);
    cJSON *deadline = cJSON_GetObjectItemCaseSensitive(job_json, "deadline_ms");
    double seconds = 60.0;
    if (cJSON_IsNumber(deadline) && deadline->valuedouble > 0) seconds = deadline->valuedouble / 1000.0;
    if (seconds > 86400.0) seconds = 86400.0;
    job->timer = evtimer_new(router->base, job_timeout_cb, job);
    if (job->timer) {
        struct timeval tv = { .tv_sec = (time_t)seconds,
                              .tv_usec = (suseconds_t)((seconds - (time_t)seconds) * 1000000) };
        evtimer_add(job->timer, &tv);
    }
}

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
        for (size_t i = 0; i < plugin->route_count; i++) {
            hub_route_event(plugin->router, "register", plugin->routes[i], plugin->app);
        }
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
        plugin->registered = false;
        for (job_t *job = router->jobs; job; ) {
            job_t *next = job->next;
            if (job->plugin == plugin) {
                send_hub_error(router, job->id, job->route, "plugin_disconnected",
                               "plugin disconnected");
                job_remove(router, job);
                job_free(job);
            }
            job = next;
        }
        for (size_t i = 0; i < plugin->route_count; i++) {
            hub_route_event(router, "unregister", plugin->routes[i], plugin->app);
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
    if (router->plugin_count >= MAX_PLUGINS) { close(fd); return; }
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
/* Config and main                                                           */

typedef struct {
    char *hub_host;
    int hub_port;
    char *device_id;
    char *device_token;
    char *listen_host;
    int listen_port;
} config_t;

static void config_free(config_t *cfg) {
    free(cfg->hub_host);
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
    cfg->listen_host = strdup("127.0.0.1");
    cfg->device_id = strdup("dejavu-device");
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "hub_host");
    if (!cJSON_IsString(item)) { cJSON_Delete(root); snprintf(err, errcap, "hub_host is required"); return -1; }
    cfg->hub_host = strdup(item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(root, "hub_port");
    if (!cJSON_IsNumber(item)) { cJSON_Delete(root); snprintf(err, errcap, "hub_port is required"); return -1; }
    cfg->hub_port = item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    if (cJSON_IsString(item)) { free(cfg->device_id); cfg->device_id = strdup(item->valuestring); }
    item = cJSON_GetObjectItemCaseSensitive(root, "device_token");
    if (cJSON_IsString(item)) cfg->device_token = strdup(item->valuestring);
    item = cJSON_GetObjectItemCaseSensitive(root, "listen_host");
    if (cJSON_IsString(item)) { free(cfg->listen_host); cfg->listen_host = strdup(item->valuestring); }
    item = cJSON_GetObjectItemCaseSensitive(root, "listen_port");
    if (cJSON_IsNumber(item)) cfg->listen_port = item->valueint;
    cJSON_Delete(root);
    if (!cfg->hub_host || !cfg->device_id || !cfg->listen_host) {
        config_free(cfg);
        snprintf(err, errcap, "out of memory"); return -1;
    }
    if (cfg->hub_port <= 0 || cfg->hub_port > 65535) { config_free(cfg); snprintf(err, errcap, "invalid hub_port"); return -1; }
    if (cfg->listen_port <= 0 || cfg->listen_port > 65535) { config_free(cfg); snprintf(err, errcap, "invalid listen_port"); return -1; }
    return 0;
}

int main(int argc, char **argv) {
    const char *config_path = argc > 1 ? argv[1] : "config.json";
    config_t cfg;
    char err[512] = {0};
    if (config_load(config_path, &cfg, err, sizeof(err)) != 0) {
        logmsg("ERROR", "%s", err);
        return 2;
    }
    router_t router;
    memset(&router, 0, sizeof(router));
    router.base = event_base_new();
    if (!router.base) { logmsg("ERROR", "cannot initialize libevent"); return 1; }
    router.hub_host = cfg.hub_host;
    router.hub_port = cfg.hub_port;
    router.device_id = cfg.device_id;
    router.device_token = cfg.device_token;
    router.listen_host = cfg.listen_host;
    router.listen_port = cfg.listen_port;
    router.reconnect_seconds = 1;
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
    hub_connect(&router);
    event_base_dispatch(router.base);
    router.stopping = true;
    drop_all_jobs(&router);
    if (router.hub_bev) bufferevent_free(router.hub_bev);
    if (router.reconnect_timer) event_free(router.reconnect_timer);
    evconnlistener_free(router.listener);
    event_free(sigint);
    event_free(sigterm);
    event_base_free(router.base);
    config_free(&cfg);
    return 0;
}