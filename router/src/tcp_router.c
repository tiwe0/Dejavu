#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <netdb.h>
#include <math.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/un.h>
#include <stddef.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#include "cJSON.h"

#define MAX_LINE (1024 * 1024)
#define MAX_PLUGINS 64
#define PLUGIN_HELLO_SECONDS 5
#define MAX_ROUTES 512
#define MAX_TARGETS 256
#define MAX_CONTROL_CLIENTS 16
#define MAX_CONTROL_LINE (32 * 1024)
#define MAX_OUTPUT (4 * MAX_LINE)
#define MAX_JOBS 8192
#define MAX_WAKES 64
#define WAKE_DEFAULT_MS 20000
#define WAKE_MAX_MS 120000

typedef struct router router_t;
typedef struct plugin plugin_t;
typedef struct job job_t;
typedef struct wake wake_t;
typedef struct wake_target wake_target_t;
typedef struct control_client control_client_t;
#define MAX_DNS_ADDRESSES 8
struct resolution {
    size_t count;
    socklen_t lengths[MAX_DNS_ADDRESSES];
    struct sockaddr_storage addresses[MAX_DNS_ADDRESSES];
};

struct wake_target {
    char *route;
    int user_id;
    char *package;
    char *activity;
    char *plugin_path;
    wake_target_t *next;
};

struct wake {
    router_t *router;
    char *id;
    char *route;
    struct event *timer;
    wake_t *next;
};

struct plugin {
    router_t *router;
    struct bufferevent *bev;
    char *app;
    char **routes;
    size_t route_count;
    struct event *hello_deadline;
    bool registered;
    bool closing;
    bool close_write_failed;
    plugin_t *next;
};

struct job {
    router_t *router;
    char wire_id[32];
    char *id;
    char *route;
    plugin_t *plugin;
    struct event *timer;
    job_t *next;
};

struct router {
    struct event_base *base;
    struct event *resolver_event;
    struct event *resolver_timeout;
    int resolver_fd;
    pid_t resolver_pid;
    struct resolution resolution;
    size_t resolver_bytes;
    size_t next_address;
    struct evconnlistener *control_listener;
    control_client_t *control_clients;
    size_t control_count;
    struct evconnlistener *plugin_unix_listener;
    char *control_socket;
    char *plugin_socket;
    char *control_name;
    char *plugin_name;
    bool listen_tcp;
    char control_token[65];
    char *config_path;
    char *state_dir;
    char *wake_targets_path;
    int lock_fd;
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
    uint64_t wire_sequence;
    wake_t *wakes;
    size_t wake_count;
    wake_target_t *wake_targets;
    size_t wake_target_count;
};

struct control_client {
    router_t *router;
    struct bufferevent *bev;
    control_client_t *next;
    bool responded;
    struct event *deadline;
};

static void logmsg(const char *level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "dejavuerouter: %s ", level);
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

static bool pending_id(router_t *router, const char *id) {
    for (job_t *job = router->jobs; job; job = job->next) if (!strcmp(job->id, id)) return true;
    for (wake_t *wake = router->wakes; wake; wake = wake->next) if (!strcmp(wake->id, id)) return true;
    return false;
}

static int plugin_send_json(plugin_t *plugin, cJSON *root) {
    if (!plugin || !plugin->bev || !root ||
        evbuffer_get_length(bufferevent_get_output(plugin->bev)) > MAX_OUTPUT) return -1;
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return -1;
    int rc = bufferevent_write(plugin->bev, text, strlen(text));
    if (rc == 0) rc = bufferevent_write(plugin->bev, "\n", 1);
    free(text);
    return rc;
}

static void free_plugin(plugin_t *plugin) {
    if (!plugin) return;
    if (plugin->hello_deadline) event_free(plugin->hello_deadline);
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
static void resolver_cancel(router_t *router);
static cJSON *parse_message(const char *text, size_t length);
static bool text_field(cJSON *item, size_t maximum, bool empty);
static bool hub_try_next_address(router_t *router);
static void dispatch_job(router_t *router, cJSON *job_json);

static void reconnect_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    router_t *router = arg;
    if (router->reconnect_timer) {
        event_free(router->reconnect_timer);
        router->reconnect_timer = NULL;
    }
    hub_connect(router);
}

static void schedule_hub_reconnect(router_t *router, int seconds) {
    if (router->stopping || !router->hub_host[0] || router->reconnect_timer) return;
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    router->reconnect_timer = evtimer_new(router->base, reconnect_cb, router);
    if (router->reconnect_timer) evtimer_add(router->reconnect_timer, &tv);
}

enum { HUB_SEND_TOO_LARGE = -2 };

static int hub_send_json(router_t *router, cJSON *root) {
    if (!router->hub_connected || !router->hub_bev || !root ||
        evbuffer_get_length(bufferevent_get_output(router->hub_bev)) > MAX_OUTPUT) return -1;
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return -1;
    if (strlen(text) > MAX_LINE) {
        free(text);
        return HUB_SEND_TOO_LARGE;
    }
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
    if (hub_send_json(router, root) == HUB_SEND_TOO_LARGE) {
        /* Restoring the external ID can grow an otherwise valid plugin frame.
         * Replace its payload once; the shared sender also bounds the fallback. */
        cJSON_DeleteItemFromObjectCaseSensitive(root, "result");
        cJSON_DeleteItemFromObjectCaseSensitive(root, "error");
        bool encoded = cJSON_ReplaceItemInObjectCaseSensitive(root, "ok", cJSON_CreateFalse());
        cJSON *error = cJSON_AddObjectToObject(root, "error");
        if (encoded && error && cJSON_AddStringToObject(error, "code", "response_too_large") &&
            cJSON_AddStringToObject(error, "message", "response exceeds Hub frame limit") &&
            cJSON_AddBoolToObject(error, "retryable", false))
            hub_send_json(router, root);
    }
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

/* ------------------------------------------------------------------------- */
/* Wake targets and wake operation                                           */

static wake_target_t *find_wake_target(router_t *router, const char *route) {
    for (wake_target_t *target = router->wake_targets; target; target = target->next) {
        if (target->route && !strcmp(target->route, route)) return target;
    }
    return NULL;
}

static void wake_target_free(wake_target_t *target) {
    if (!target) return;
    free(target->route);
    free(target->package);
    free(target->activity);
    free(target->plugin_path);
    free(target);
}

static void wake_targets_free(router_t *router) {
    wake_target_t *target = router->wake_targets;
    while (target) {
        wake_target_t *next = target->next;
        wake_target_free(target);
        target = next;
    }
    router->wake_targets = NULL;
    router->wake_target_count = 0;
}

static void child_close_fd(int fd, int keep) {
    if (fd >= 0 && fd != keep) close(fd);
}

/* A DNS/launch child must not retain the daemon lock or any listening/client
 * sockets if the supervisor restarts a parent killed with SIGKILL. */
static void child_close_router_fds(router_t *router, int keep) {
    child_close_fd(router->lock_fd, keep);
    if (router->listener) child_close_fd(evconnlistener_get_fd(router->listener), keep);
    if (router->plugin_unix_listener) child_close_fd(evconnlistener_get_fd(router->plugin_unix_listener), keep);
    if (router->control_listener) child_close_fd(evconnlistener_get_fd(router->control_listener), keep);
    if (router->hub_bev) child_close_fd(bufferevent_getfd(router->hub_bev), keep);
    for (plugin_t *p = router->plugins; p; p = p->next) child_close_fd(bufferevent_getfd(p->bev), keep);
    for (control_client_t *p = router->control_clients; p; p = p->next) child_close_fd(bufferevent_getfd(p->bev), keep);
}

/* Execute Android launch commands in a child so a slow ActivityManager cannot
 * block routing, deadlines, or the control endpoint. SIGCHLD reaps children. */
static int launch_wake_target(router_t *router, wake_target_t *target) {
#ifdef __ANDROID__
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        child_close_router_fds(router, -1);
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        /* Injection follows the plugin's configured scope. A router wake only
         * launches the explicit component, never changes global hook properties. */
        char user[16], component[544];
        snprintf(user, sizeof(user), "%d", target->user_id);
        snprintf(component, sizeof(component), "%s/%s", target->package, target->activity);
        execl("/system/bin/am", "am", "start", "--user", user, "-n", component, (char *)NULL);
        _exit(127);
    }
    return 0;
#else
    (void)router; (void)target;
    return -1;
#endif
}



static void wake_remove(router_t *router, wake_t *wake) {
    wake_t **slot = &router->wakes;
    while (*slot && *slot != wake) slot = &(*slot)->next;
    if (*slot == wake) *slot = wake->next;
    if (router->wake_count > 0) router->wake_count--;
}

static void wake_free(wake_t *wake) {
    if (!wake) return;
    if (wake->timer) event_free(wake->timer);
    free(wake->id);
    free(wake->route);
    free(wake);
}

static void wake_drop(wake_t *wake) {
    wake_remove(wake->router, wake);
    wake_free(wake);
}

static void wake_timeout_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    wake_t *wake = arg;
    send_hub_error(wake->router, wake->id, wake->route, "wake_timeout",
                   "app did not register the route before the wake deadline");
    wake_drop(wake);
}

static void wake_finish_ok(router_t *router, wake_t *wake, bool woken) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "online", 1);
    cJSON_AddBoolToObject(result, "woken", woken);
    wake_target_t *target = find_wake_target(router, wake->route);
    if (target) cJSON_AddNumberToObject(result, "user_id", target->user_id);
    send_hub_response(router, wake->id, wake->route, true, result);
    cJSON_Delete(result);
    wake_drop(wake);
}

static void wake_route_online(router_t *router, const char *route) {
    for (wake_t *wake = router->wakes; wake; ) {
        wake_t *next = wake->next;
        if (wake->route && route && !strcmp(wake->route, route)) {
            wake_finish_ok(router, wake, true);
        }
        wake = next;
    }
}

static void drop_all_wakes(router_t *router) {
    wake_t *wake = router->wakes;
    while (wake) {
        wake_t *next = wake->next;
        wake_free(wake);
        wake = next;
    }
    router->wakes = NULL;
    router->wake_count = 0;
}

static void handle_wake(router_t *router, cJSON *root) {
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *route = cJSON_GetObjectItemCaseSensitive(root, "route");
    if (!cJSON_IsString(id) || !cJSON_IsString(route) || !id->valuestring[0] || !route->valuestring[0] ||
        strlen(id->valuestring) > 128 || strlen(route->valuestring) > 128) {
        send_hub_error(router, cJSON_IsString(id) ? id->valuestring : "",
                       cJSON_IsString(route) ? route->valuestring : "",
                       "invalid_wake", "wake needs id and route");
        return;
    }
    if (pending_id(router, id->valuestring)) {
        send_hub_error(router, id->valuestring, route->valuestring, "duplicate_id", "request id already pending");
        return;
    }
    if (find_plugin(router, route->valuestring)) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "online", 1);
        cJSON_AddBoolToObject(result, "woken", 0);
        send_hub_response(router, id->valuestring, route->valuestring, true, result);
        cJSON_Delete(result);
        return;
    }
    wake_target_t *target = find_wake_target(router, route->valuestring);
    if (!target) {
        send_hub_error(router, id->valuestring, route->valuestring, "wake_target_unavailable",
                       "no local wake target for route");
        return;
    }
    for (wake_t *wake = router->wakes; wake; wake = wake->next) {
        if (wake->route && !strcmp(wake->route, route->valuestring)) {
            send_hub_error(router, id->valuestring, route->valuestring, "wake_in_progress",
                           "a wake is already in progress for this route");
            return;
        }
    }
    if (router->wake_count >= MAX_WAKES) {
        send_hub_error(router, id->valuestring, route->valuestring, "router_busy",
                       "too many wake operations");
        return;
    }
    wake_t *wake = calloc(1, sizeof(*wake));
    if (!wake) {
        send_hub_error(router, id->valuestring, route->valuestring, "router_oom", "out of memory");
        return;
    }
    wake->router = router;
    wake->id = strdup(id->valuestring);
    wake->route = strdup(route->valuestring);
    if (!wake->id || !wake->route) { wake_free(wake); send_hub_error(router, id->valuestring,
                                                                     route->valuestring, "router_oom",
                                                                     "out of memory"); return; }
    int timeout_ms = WAKE_DEFAULT_MS;
    cJSON *timeout = cJSON_GetObjectItemCaseSensitive(root, "timeout_ms");
    if (cJSON_IsNumber(timeout) && timeout->valueint > 0) timeout_ms = timeout->valueint;
    if (timeout_ms < 1000) timeout_ms = 1000;
    if (timeout_ms > WAKE_MAX_MS) timeout_ms = WAKE_MAX_MS;
    wake->timer = evtimer_new(router->base, wake_timeout_cb, wake);
    if (!wake->timer) { wake_free(wake); send_hub_error(router, id->valuestring,
                                                        route->valuestring, "router_oom",
                                                        "out of memory"); return; }
    struct timeval tv = { .tv_sec = timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };
    evtimer_add(wake->timer, &tv);
    wake->next = router->wakes;
    router->wakes = wake;
    router->wake_count++;
    logmsg("INFO", "wake route=%s user=%d package=%s", route->valuestring, target->user_id,
           target->package);
    if (launch_wake_target(router, target) != 0) {
        send_hub_error(router, wake->id, wake->route, "wake_failed",
                       "could not launch the target app");
            wake_drop(wake);
    }
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

static void job_cancel_plugin(job_t *job) {
    cJSON *cancel = cJSON_CreateObject();
    if (!cancel) return;
    if (cJSON_AddStringToObject(cancel, "type", "cancel") &&
        cJSON_AddStringToObject(cancel, "id", job->wire_id) &&
        cJSON_AddStringToObject(cancel, "route", job->route))
        plugin_send_json(job->plugin, cancel);
    cJSON_Delete(cancel);
}

static void job_timeout_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    job_t *job = arg;
    job_cancel_plugin(job);
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

static job_t *find_hub_job(router_t *router, const char *id) {
    for (job_t *job = router->jobs; job; job = job->next) {
        if (job->id && !strcmp(job->id, id)) return job;
    }
    return NULL;
}

/* Wire IDs never repeat during the daemon lifetime, including Hub reconnects. */
static job_t *find_wire_job(router_t *router, const char *id) {
    for (job_t *job = router->jobs; job; job = job->next)
        if (!strcmp(job->wire_id, id)) return job;
    return NULL;
}

static void drop_all_jobs(router_t *router) {
    job_t *job = router->jobs;
    while (job) {
        job_t *next = job->next;
        job_cancel_plugin(job);
        job_free(job);
        job = next;
    }
    router->jobs = NULL;
    router->job_count = 0;
}

static void hub_disconnect(router_t *router) {
    resolver_cancel(router);
    router->hub_connected = false;
    if (router->hub_bev) {
        bufferevent_free(router->hub_bev);
        router->hub_bev = NULL;
    }
    drop_all_jobs(router);
    drop_all_wakes(router);
}

static void hub_event_cb(struct bufferevent *bev, short events, void *arg) {
    (void)bev;
    router_t *router = arg;
    if (events & BEV_EVENT_CONNECTED) {
        struct timeval timeout = {60, 0};
        bufferevent_set_timeouts(bev, &timeout, &timeout);
        router->next_address = router->resolution.count;
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
            if (!p->registered || p->closing) continue;
            for (size_t i = 0; i < p->route_count; i++) {
                hub_route_event(router, "register", p->routes[i], p->app);
            }
        }
        return;
    }
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        logmsg("WARN", "hub disconnected; reconnecting");
        hub_disconnect(router);
        if (!hub_try_next_address(router)) {
            schedule_hub_reconnect(router, router->reconnect_seconds);
            if (router->reconnect_seconds < 30) router->reconnect_seconds = router->reconnect_seconds < 15 ? router->reconnect_seconds * 2 : 30;
        }
    }
}

static void dispatch_job(router_t *router, cJSON *job_json);

static void hub_read_cb(struct bufferevent *bev, void *arg) {
    router_t *router = arg;
    struct evbuffer *input = bufferevent_get_input(bev);
    if (evbuffer_get_length(input) > MAX_LINE * 2) {
        logmsg("WARN", "hub input exceeds limit; reconnecting");
        hub_disconnect(router);
        schedule_hub_reconnect(router, 1);
        return;
    }
    while (1) {
        size_t length = 0;
        char *line = evbuffer_readln(input, &length, EVBUFFER_EOL_LF);
        if (!line) break;
        if (length > MAX_LINE) { free(line); continue; }
        cJSON *root = parse_message(line, length);
        free(line);
        if (!root) {
            send_hub_error(router, "", "", "invalid_job", "invalid Hub message");
            continue;
        }
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        if (cJSON_IsString(type) && !strcmp(type->valuestring, "request")) {
            dispatch_job(router, root);
        } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "cancel")) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
            cJSON *route = cJSON_GetObjectItemCaseSensitive(root, "route");
            job_t *job = cJSON_IsString(id) ? find_hub_job(router, id->valuestring) : NULL;
            if (job && cJSON_IsString(route) && !strcmp(job->route, route->valuestring)) {
                job_cancel_plugin(job);
                job_remove(router, job);
                job_free(job);
            }
        } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "wake")) {
            handle_wake(router, root);
        } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "ping")) {
            cJSON *pong = cJSON_CreateObject();
            cJSON_AddStringToObject(pong, "type", "pong");
            hub_send_json(router, pong);
            cJSON_Delete(pong);
        }
        cJSON_Delete(root);
    }
}

static void resolver_cancel(router_t *router) {
    if (router->resolver_event) { event_free(router->resolver_event); router->resolver_event = NULL; }
    if (router->resolver_timeout) { event_free(router->resolver_timeout); router->resolver_timeout = NULL; }
    if (router->resolver_fd >= 0) { close(router->resolver_fd); router->resolver_fd = -1; }
    if (router->resolver_pid > 0) {
        kill(router->resolver_pid, SIGKILL);
        while (waitpid(router->resolver_pid, NULL, 0) < 0 && errno == EINTR) { }
        router->resolver_pid = 0;
    }
}

static void hub_connect_address(router_t *router, struct sockaddr *address, socklen_t length) {
    struct bufferevent *bev = bufferevent_socket_new(router->base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) { schedule_hub_reconnect(router, 2); return; }
    bufferevent_setcb(bev, hub_read_cb, NULL, hub_event_cb, router);
    struct timeval timeout = {10, 0};
    bufferevent_set_timeouts(bev, &timeout, &timeout);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
    router->hub_bev = bev;
    int connected = bufferevent_socket_connect(bev, address, (int)length);
    int socket_fd = bufferevent_getfd(bev);
    if (socket_fd >= 0) evutil_make_socket_closeonexec(socket_fd);
    if (connected != 0) {
        hub_disconnect(router);
        if (!hub_try_next_address(router)) schedule_hub_reconnect(router, router->reconnect_seconds);
    }
}

static bool hub_try_next_address(router_t *router) {
    if (router->next_address >= router->resolution.count) return false;
    size_t index = router->next_address++;
    hub_connect_address(router, (struct sockaddr *)&router->resolution.addresses[index],
                        router->resolution.lengths[index]);
    return true;
}

static void resolver_done_cb(evutil_socket_t fd, short events, void *arg) {
    (void)events;
    router_t *router = arg;
    ssize_t got;
    do {
        got = read(fd, (char *)&router->resolution + router->resolver_bytes,
                   sizeof(router->resolution) - router->resolver_bytes);
    } while (got < 0 && errno == EINTR);
    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (got > 0) router->resolver_bytes += (size_t)got;
    if (got > 0 && router->resolver_bytes < sizeof(router->resolution)) return;
    resolver_cancel(router);
    bool valid = router->resolver_bytes == sizeof(router->resolution) &&
                 router->resolution.count > 0 && router->resolution.count <= MAX_DNS_ADDRESSES;
    for (size_t i = 0; valid && i < router->resolution.count; i++) {
        int family = router->resolution.addresses[i].ss_family;
        valid = (family == AF_INET && router->resolution.lengths[i] == sizeof(struct sockaddr_in)) ||
                (family == AF_INET6 && router->resolution.lengths[i] == sizeof(struct sockaddr_in6));
    }
    router->next_address = 0;
    if (!valid) router->resolution.count = 0;
    if (!hub_try_next_address(router)) {
        schedule_hub_reconnect(router, router->reconnect_seconds);
        if (router->reconnect_seconds < 30) router->reconnect_seconds = router->reconnect_seconds < 15 ? router->reconnect_seconds * 2 : 30;
    }
}

static void resolver_timeout_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd; (void)events;
    router_t *router = arg;
    resolver_cancel(router);
    schedule_hub_reconnect(router, router->reconnect_seconds);
    if (router->reconnect_seconds < 30) router->reconnect_seconds = router->reconnect_seconds < 15 ? router->reconnect_seconds * 2 : 30;
}

static void hub_connect(router_t *router) {
    if (router->stopping || router->hub_bev || router->resolver_event || !router->hub_host[0]) return;
    if (router->reconnect_timer) {
        event_free(router->reconnect_timer);
        router->reconnect_timer = NULL;
    }
    memset(&router->resolution, 0, sizeof(router->resolution));
    router->next_address = 0; router->resolver_bytes = 0;
    /* Numeric endpoints do not need DNS or a child. */
    struct sockaddr_in v4 = {0};
    v4.sin_family = AF_INET; v4.sin_port = htons((uint16_t)router->hub_port);
    if (inet_pton(AF_INET, router->hub_host, &v4.sin_addr) == 1) {
        hub_connect_address(router, (struct sockaddr *)&v4, sizeof(v4)); return;
    }
    struct sockaddr_in6 v6 = {0};
    v6.sin6_family = AF_INET6; v6.sin6_port = htons((uint16_t)router->hub_port);
    if (inet_pton(AF_INET6, router->hub_host, &v6.sin6_addr) == 1) {
        hub_connect_address(router, (struct sockaddr *)&v6, sizeof(v6)); return;
    }
    /* Use the platform resolver (including Android Private DNS/VPN policies),
     * isolated in one bounded child so DNS can never stall the event loop. */
    int descriptors[2];
    if (pipe(descriptors) != 0) { schedule_hub_reconnect(router, 2); return; }
    fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    pid_t child = fork();
    if (child < 0) { close(descriptors[0]); close(descriptors[1]); schedule_hub_reconnect(router, 2); return; }
    if (child == 0) {
        close(descriptors[0]);
        child_close_router_fds(router, descriptors[1]);
        struct addrinfo hints = {0}, *resolved = NULL;
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        char service[16]; snprintf(service, sizeof(service), "%d", router->hub_port);
        struct resolution result = {0};
        if (getaddrinfo(router->hub_host, service, &hints, &resolved) == 0) {
            for (struct addrinfo *p = resolved; p && result.count < MAX_DNS_ADDRESSES; p = p->ai_next) {
                if ((p->ai_family != AF_INET && p->ai_family != AF_INET6) || p->ai_addrlen > sizeof(result.addresses[0])) continue;
                bool duplicate = false;
                for (size_t i = 0; i < result.count; i++)
                    if (result.lengths[i] == p->ai_addrlen && !memcmp(&result.addresses[i], p->ai_addr, p->ai_addrlen)) duplicate = true;
                if (duplicate) continue;
                result.lengths[result.count] = p->ai_addrlen;
                memcpy(&result.addresses[result.count++], p->ai_addr, p->ai_addrlen);
            }
        }
        if (resolved) freeaddrinfo(resolved);
        size_t total = 0;
        while (total < sizeof(result)) {
            ssize_t written = write(descriptors[1], (char *)&result + total, sizeof(result) - total);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            total += (size_t)written;
        }
        close(descriptors[1]);
        _exit(total == sizeof(result) ? 0 : 1);
    }
    close(descriptors[1]);
    router->resolver_fd = descriptors[0]; router->resolver_pid = child;
    evutil_make_socket_nonblocking(descriptors[0]);
    router->resolver_event = event_new(router->base, descriptors[0], EV_READ | EV_PERSIST, resolver_done_cb, router);
    router->resolver_timeout = evtimer_new(router->base, resolver_timeout_cb, router);
    if (!router->resolver_event || !router->resolver_timeout) { resolver_cancel(router); schedule_hub_reconnect(router, 2); return; }
    event_add(router->resolver_event, NULL);
    struct timeval deadline = {10, 0};
    evtimer_add(router->resolver_timeout, &deadline);
}

/* ------------------------------------------------------------------------- */
/* Jobs and plugin forwarding                                                */

static bool method_field(cJSON *item) {
    if (!text_field(item, 128, false)) return false;
    for (const unsigned char *p = (const unsigned char *)item->valuestring; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-')) return false;
    return true;
}

static void dispatch_job(router_t *router, cJSON *job_json) {
    cJSON *id = cJSON_GetObjectItemCaseSensitive(job_json, "id");
    cJSON *route = cJSON_GetObjectItemCaseSensitive(job_json, "route");
    cJSON *method = cJSON_GetObjectItemCaseSensitive(job_json, "method");
    if (!text_field(id, 128, false) || !text_field(route, 128, false) || !method_field(method)) {
        send_hub_error(router, cJSON_IsString(id) ? id->valuestring : "",
                       cJSON_IsString(route) ? route->valuestring : "", "invalid_job",
                       "job needs valid id, route and method");
        return;
    }
    if (pending_id(router, id->valuestring)) {
        send_hub_error(router, id->valuestring, route->valuestring, "duplicate_id", "request id already pending");
        return;
    }
    if (router->job_count >= MAX_JOBS) {
        send_hub_error(router, id->valuestring, route->valuestring, "router_busy",
                       "too many in-flight jobs");
        return;
    }
    plugin_t *plugin = find_plugin(router, route->valuestring);
    if (!plugin || plugin->closing) {
        send_hub_error(router, id->valuestring, route->valuestring, "route_unavailable",
                       "no plugin registered for route");
        return;
    }
    if (router->wire_sequence == UINT64_MAX) {
        send_hub_error(router, id->valuestring, route->valuestring, "router_busy", "request sequence exhausted");
        return;
    }
    job_t *job = calloc(1, sizeof(*job));
    if (!job) { send_hub_error(router, id->valuestring, route->valuestring, "router_oom",
                              "out of memory"); return; }
    job->router = router;
    snprintf(job->wire_id, sizeof(job->wire_id), "rpc-%llu", (unsigned long long)++router->wire_sequence);
    job->id = strdup(id->valuestring);
    job->route = strdup(route->valuestring);
    job->plugin = plugin;
    if (!job->id || !job->route) {
        job_free(job);
        send_hub_error(router, id->valuestring, route->valuestring, "router_oom",
                       "out of memory");
        return;
    }
    cJSON *deadline = cJSON_GetObjectItemCaseSensitive(job_json, "deadline_ms");
    double timeout_ms = 60000.0;
    if (cJSON_IsNumber(deadline) && isfinite(deadline->valuedouble) && deadline->valuedouble > 0)
        timeout_ms = fmin(deadline->valuedouble, 86400000.0);
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        send_hub_error(router, job->id, job->route, "router_clock", "cannot determine request deadline");
        job_free(job); return;
    }
    uint64_t expires_ms = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u +
                          (uint64_t)ceil(timeout_ms);
    cJSON *request = cJSON_CreateObject();
    cJSON *params = cJSON_GetObjectItemCaseSensitive(job_json, "params");
    bool encoded = request && cJSON_AddStringToObject(request, "type", "request") &&
                   cJSON_AddStringToObject(request, "id", job->wire_id) &&
                   cJSON_AddStringToObject(request, "route", job->route) &&
                   cJSON_AddNumberToObject(request, "deadline_ms", timeout_ms) &&
                   cJSON_AddNumberToObject(request, "deadline_monotonic_ms", (double)expires_ms) &&
                   cJSON_AddItemReferenceToObject(request, "method", method) &&
                   (!params || cJSON_AddItemReferenceToObject(request, "params", params));
    if (!encoded) {
        cJSON_Delete(request);
        send_hub_error(router, job->id, job->route, "router_oom", "cannot encode request");
        job_free(job); return;
    }
    char *wire = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!wire || strlen(wire) > MAX_LINE) {
        send_hub_error(router, job->id, job->route, wire ? "invalid_job" : "router_oom",
                       "cannot encode request within plugin frame limit");
        free(wire);
        job_free(job); return;
    }
    /* Arm the deadline before dispatch: an allocation/timer failure must not
     * leave a plugin executing a request the router cannot time out. */
    job->timer = evtimer_new(router->base, job_timeout_cb, job);
    struct timeval tv = { .tv_sec = (time_t)(timeout_ms / 1000.0),
                          .tv_usec = (suseconds_t)(fmod(timeout_ms, 1000.0) * 1000.0) };
    if (!job->timer || evtimer_add(job->timer, &tv) != 0) {
        free(wire);
        send_hub_error(router, job->id, job->route, "router_oom", "cannot create deadline");
        job_free(job); return;
    }
    job->next = router->jobs;
    router->jobs = job;
    router->job_count++;
    if (evbuffer_get_length(bufferevent_get_output(plugin->bev)) > MAX_OUTPUT ||
        bufferevent_write(plugin->bev, wire, strlen(wire)) != 0 ||
        bufferevent_write(plugin->bev, "\n", 1) != 0) {
        free(wire);
        send_hub_error(router, job->id, job->route, "plugin_send_failed",
                       "could not write to plugin");
        job_remove(router, job);
        job_free(job);
        return;
    }
    free(wire);
}

static void plugin_event_cb(struct bufferevent *bev, short events, void *arg);

static void plugin_write_cb(struct bufferevent *bev, void *arg) {
    plugin_t *plugin = arg;
    if (plugin->closing && evbuffer_get_length(bufferevent_get_output(bev)) == 0)
        plugin_event_cb(bev, BEV_EVENT_EOF, plugin);
}

static void plugin_reject_hello(plugin_t *plugin, const char *code, const char *message) {
    plugin->closing = true;
    bufferevent_disable(plugin->bev, EV_READ);
    struct timeval timeout = {2, 0};
    bufferevent_set_timeouts(plugin->bev, NULL, &timeout);
    cJSON *reply = cJSON_CreateObject();
    cJSON *error = reply ? cJSON_AddObjectToObject(reply, "error") : NULL;
    bool encoded = reply && error && cJSON_AddStringToObject(reply, "type", "hello_error") &&
                   cJSON_AddStringToObject(error, "code", code) &&
                   cJSON_AddStringToObject(error, "message", message);
    plugin->close_write_failed = !encoded || plugin_send_json(plugin, reply) != 0;
    cJSON_Delete(reply);
}

static void plugin_handle_message(plugin_t *plugin, const char *line, size_t length) {
    cJSON *root = parse_message(line, length);
    if (!root) {
        if (!plugin->registered) plugin_reject_hello(plugin, "invalid_hello", "expected a valid hello object");
        return;
    }
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || (!plugin->registered && strcmp(type->valuestring, "hello"))) {
        if (!plugin->registered) plugin_reject_hello(plugin, "invalid_hello", "hello must be the first message");
        cJSON_Delete(root); return;
    }
    if (!strcmp(type->valuestring, "hello")) {
        if (plugin->registered) {
            plugin_reject_hello(plugin, "already_registered", "route changes require a new connection");
            cJSON_Delete(root); return;
        }
        cJSON *app = cJSON_GetObjectItemCaseSensitive(root, "app");
        cJSON *routes = cJSON_GetObjectItemCaseSensitive(root, "routes");
        size_t count = cJSON_IsArray(routes) ? (size_t)cJSON_GetArraySize(routes) : 0;
        size_t total = 0;
        for (plugin_t *p = plugin->router->plugins; p; p = p->next) total += p->route_count;
        const char *registration_error = NULL;
        if (!text_field(app, 256, false) || !cJSON_IsArray(routes) || count == 0 || count > 64)
            registration_error = "invalid_hello";
        else if (total + count > MAX_ROUTES) registration_error = "router_busy";
        for (size_t i = 0; !registration_error && i < count; ++i) {
            cJSON *item = cJSON_GetArrayItem(routes, (int)i);
            if (!text_field(item, 128, false)) {
                registration_error = "invalid_hello"; break;
            }
            if (find_plugin(plugin->router, item->valuestring)) {
                registration_error = "route_conflict"; break;
            }
            for (size_t j = 0; !registration_error && j < i; ++j)
                if (!strcmp(item->valuestring, cJSON_GetArrayItem(routes, (int)j)->valuestring))
                    registration_error = "invalid_hello";
        }
        if (registration_error) {
            plugin_reject_hello(plugin, registration_error, "route registration rejected");
            cJSON_Delete(root); return;
        }
        plugin->app = strdup(app->valuestring);
        plugin->routes = calloc(count, sizeof(char *));
        if (!plugin->app || !plugin->routes) {
            free(plugin->app); plugin->app = NULL;
            free(plugin->routes); plugin->routes = NULL;
            plugin_reject_hello(plugin, "router_oom", "cannot allocate route registration");
            cJSON_Delete(root); return;
        }
        for (size_t i = 0; i < count; i++) {
            char *route = strdup(cJSON_GetArrayItem(routes, (int)i)->valuestring);
            if (!route) {
                for (size_t j = 0; j < plugin->route_count; j++) free(plugin->routes[j]);
                free(plugin->routes); plugin->routes = NULL; plugin->route_count = 0;
                free(plugin->app); plugin->app = NULL;
                plugin_reject_hello(plugin, "router_oom", "cannot allocate route registration");
                cJSON_Delete(root); return;
            }
            plugin->routes[plugin->route_count++] = route;
        }
        cJSON *reply = cJSON_CreateObject();
        bool acknowledged = reply && cJSON_AddStringToObject(reply, "type", "hello_ok") &&
                            cJSON_AddNumberToObject(reply, "version", 1) &&
                            plugin_send_json(plugin, reply) == 0;
        cJSON_Delete(reply);
        if (!acknowledged) {
            plugin_reject_hello(plugin, "router_oom", "cannot acknowledge route registration");
            cJSON_Delete(root); return;
        }
        plugin->registered = true;
        event_free(plugin->hello_deadline);
        plugin->hello_deadline = NULL;
        bufferevent_set_timeouts(plugin->bev, NULL, NULL);
        for (size_t i = 0; i < plugin->route_count; i++) {
            hub_route_event(plugin->router, "register", plugin->routes[i], plugin->app);
            wake_route_online(plugin->router, plugin->routes[i]);
        }
        logmsg("INFO", "plugin connected app=%s routes=%zu", plugin->app, plugin->route_count);
    } else if (!strcmp(type->valuestring, "response")) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (!cJSON_IsString(id)) { cJSON_Delete(root); return; }
        job_t *job = find_wire_job(plugin->router, id->valuestring);
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
        plugin_event_cb(bev, BEV_EVENT_ERROR, plugin);
        return;
    }
    while (1) {
        size_t length = 0;
        char *line = evbuffer_readln(input, &length, EVBUFFER_EOL_LF);
        if (!line) break;
        if (length > MAX_LINE) { free(line); continue; }
        plugin_handle_message(plugin, line, length);
        free(line);
        if (plugin->closing) {
            if (plugin->close_write_failed) plugin_event_cb(bev, BEV_EVENT_ERROR, plugin);
            return;
        }
    }
}

static void plugin_event_cb(struct bufferevent *bev, short events, void *arg) {
    plugin_t *plugin = arg;
    router_t *router = plugin->router;
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        bool registered = plugin->registered;
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
        if (registered) {
            for (size_t i = 0; i < plugin->route_count; i++)
                hub_route_event(router, "unregister", plugin->routes[i], plugin->app);
        }
        detach_plugin(router, plugin);
        logmsg("INFO", "plugin disconnected app=%s", plugin->app ? plugin->app : "");
        free_plugin(plugin);
        (void)bev;
    }
}

static void plugin_hello_deadline_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd;
    (void)events;
    plugin_t *plugin = arg;
    plugin_event_cb(plugin->bev, BEV_EVENT_TIMEOUT, plugin);
}

static void listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
                        struct sockaddr *address, int socklen, void *arg) {
    (void)listener;
    (void)address;
    (void)socklen;
    router_t *router = arg;
    evutil_make_socket_closeonexec(fd);
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
    plugin->hello_deadline = evtimer_new(router->base, plugin_hello_deadline_cb, plugin);
    struct timeval timeout = {PLUGIN_HELLO_SECONDS, 0};
    if (!plugin->hello_deadline || evtimer_add(plugin->hello_deadline, &timeout) != 0) {
        plugin_event_cb(bev, BEV_EVENT_ERROR, plugin);
        return;
    }
    bufferevent_setcb(bev, plugin_read_cb, plugin_write_cb, plugin_event_cb, plugin);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

/* ------------------------------------------------------------------------- */
/* Config and main                                                           */

typedef struct {
    char *hub_host, *device_id, *device_token, *listen_host;
    char *control_socket, *plugin_socket;
    bool listen_tcp;
    int hub_port, listen_port;
} config_t;

static bool text_field(cJSON *item, size_t maximum, bool empty) {
    if (!cJSON_IsString(item) || (!empty && !item->valuestring[0]) || strlen(item->valuestring) > maximum) return false;
    for (const unsigned char *p = (const unsigned char *)item->valuestring; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}

static bool integer_field(cJSON *item, int minimum, int maximum) {
    return cJSON_IsNumber(item) && isfinite(item->valuedouble) &&
           item->valuedouble >= minimum && item->valuedouble <= maximum &&
           item->valuedouble == item->valueint;
}

static bool component_field(cJSON *item, bool activity) {
    if (!text_field(item, 256, false)) return false;
    for (const unsigned char *p = (const unsigned char *)item->valuestring; *p; ++p)
        if (!isalnum(*p) && *p != '.' && *p != '_' && (!activity || *p != '$')) return false;
    return true;
}

static bool nul_escape(const char *text, size_t length) {
    if (memchr(text, 0, length)) return true;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] != '\\') continue;
        if (i + 5 < length && text[i + 1] == 'u' && !memcmp(text + i + 2, "0000", 4)) return true;
        if (i + 1 < length) i++; /* Skip an escaped backslash. */
    }
    return false;
}

static cJSON *parse_message(const char *text, size_t length) {
    if (nul_escape(text, length)) return NULL;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, length + 1, &end, true);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    return root;
}

static bool socket_name(cJSON *item) {
    if (!text_field(item, 90, false)) return false;
    if (!strcmp(item->valuestring, ".") || !strcmp(item->valuestring, "..") ||
        !strcmp(item->valuestring, "control.json") || !strcmp(item->valuestring, "wake_targets.json") ||
        !strcmp(item->valuestring, "daemon.lock")) return false;
    for (const unsigned char *p = (const unsigned char *)item->valuestring; *p; ++p)
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-') return false;
    return true;
}

static cJSON *read_json_file(const char *path, size_t maximum) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size > maximum) {
        close(fd); return NULL;
    }
    size_t size = (size_t)st.st_size, got = 0;
    char *text = calloc(size + 1, 1);
    if (!text) { close(fd); return NULL; }
    while (got < size) {
        ssize_t n = read(fd, text + got, size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    cJSON *root = got == size ? parse_message(text, size) : NULL;
    free(text);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }
    return root;
}

static bool atomic_json_write(const char *path, cJSON *root) {
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return false;
    char temporary[PATH_MAX];
    static unsigned sequence;
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld.%u", path, (long)getpid(), sequence++);
    if (length < 0 || (size_t)length >= sizeof(temporary)) { free(text); return false; }
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { free(text); return false; }
    size_t size = strlen(text), written = 0;
    bool ok = true;
    while (written < size) {
        ssize_t n = write(fd, text + written, size - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        written += (size_t)n;
    }
    free(text);
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(temporary, path) != 0) ok = false;
    if (!ok) unlink(temporary);
    return ok;
}

static char *state_path(router_t *router, const char *name) {
    size_t size = strlen(router->state_dir) + strlen(name) + 2;
    char *path = malloc(size);
    if (path) snprintf(path, size, "%s/%s", router->state_dir, name);
    return path;
}

static void config_free(config_t *cfg) {
    free(cfg->hub_host); free(cfg->device_id); free(cfg->device_token); free(cfg->listen_host);
    free(cfg->control_socket); free(cfg->plugin_socket);
    memset(cfg, 0, sizeof(*cfg));
}

static bool config_parse(cJSON *root, config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "hub_host");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "device_token");
    cJSON *listen = cJSON_GetObjectItemCaseSensitive(root, "listen_host");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "hub_port");
    cJSON *local_port = cJSON_GetObjectItemCaseSensitive(root, "listen_port");
    cJSON *control = cJSON_GetObjectItemCaseSensitive(root, "control_socket");
    cJSON *plugins = cJSON_GetObjectItemCaseSensitive(root, "plugin_socket");
    cJSON *tcp = cJSON_GetObjectItemCaseSensitive(root, "listen_tcp");
    if ((host && !text_field(host, 253, true)) || (id && !text_field(id, 128, false)) ||
        (token && !text_field(token, 4096, true)) ||
        (listen && (!text_field(listen, 15, false) || strcmp(listen->valuestring, "127.0.0.1"))) ||
        (port && !integer_field(port, 1, 65535)) ||
        (local_port && !integer_field(local_port, 1, 65535)) ||
        (control && !socket_name(control)) ||
        (plugins && !socket_name(plugins)) || (tcp && !cJSON_IsBool(tcp))) return false;
#ifdef __ANDROID__
    if ((control && strchr(control->valuestring, '/')) || (plugins && strchr(plugins->valuestring, '/'))) return false;
#endif
    cfg->hub_host = strdup(host ? host->valuestring : "");
    cfg->device_id = strdup(id ? id->valuestring : "dejavu-device");
    cfg->device_token = strdup(token ? token->valuestring : "");
    cfg->listen_host = strdup("127.0.0.1");
    cfg->hub_port = port ? port->valueint : 39010;
    cfg->listen_port = local_port ? local_port->valueint : 39111;
#ifdef __ANDROID__
    cfg->control_socket = strdup(control ? control->valuestring : "dejavu.router.control");
    cfg->plugin_socket = strdup(plugins ? plugins->valuestring : "dejavu.router.plugins");
#else
    cfg->control_socket = strdup(control ? control->valuestring : "control.sock");
    cfg->plugin_socket = strdup(plugins ? plugins->valuestring : "plugins.sock");
#endif
    cfg->listen_tcp = cJSON_IsTrue(tcp);
    if (!cfg->hub_host || !cfg->device_id || !cfg->device_token || !cfg->listen_host ||
        !cfg->control_socket || !cfg->plugin_socket || !strcmp(cfg->control_socket, cfg->plugin_socket)) { config_free(cfg); return false; }
    return true;
}

static cJSON *config_json(router_t *router, const char *host, int port, const char *id, const char *token) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "hub_host", host);
    cJSON_AddNumberToObject(root, "hub_port", port);
    cJSON_AddStringToObject(root, "device_id", id);
    cJSON_AddStringToObject(root, "device_token", token);
    cJSON_AddStringToObject(root, "listen_host", "127.0.0.1");
    cJSON_AddNumberToObject(root, "listen_port", router->listen_port);
    cJSON_AddBoolToObject(root, "listen_tcp", router->listen_tcp);
    cJSON_AddStringToObject(root, "control_socket", router->control_name);
    cJSON_AddStringToObject(root, "plugin_socket", router->plugin_name);
    cJSON_AddStringToObject(root, "wake_targets", router->wake_targets_path);
    return root;
}

static bool target_valid(const char *route, cJSON *item) {
    cJSON *key = cJSON_CreateString(route);
    bool valid = key && text_field(key, 128, false);
    cJSON_Delete(key);
    cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "plugin_path");
    return valid && cJSON_IsObject(item) &&
        integer_field(cJSON_GetObjectItemCaseSensitive(item, "user_id"), 0, 999) &&
        component_field(cJSON_GetObjectItemCaseSensitive(item, "package"), false) &&
        component_field(cJSON_GetObjectItemCaseSensitive(item, "activity"), true) &&
        (!path || (text_field(path, 1024, true) && (!path->valuestring[0] || path->valuestring[0] == '/')));
}

static wake_target_t *target_from_json(const char *route, cJSON *item) {
    wake_target_t *target = calloc(1, sizeof(*target));
    if (!target) return NULL;
    target->route = strdup(route);
    target->user_id = cJSON_GetObjectItemCaseSensitive(item, "user_id")->valueint;
    target->package = strdup(cJSON_GetObjectItemCaseSensitive(item, "package")->valuestring);
    target->activity = strdup(cJSON_GetObjectItemCaseSensitive(item, "activity")->valuestring);
    cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "plugin_path");
    target->plugin_path = strdup(path ? path->valuestring : "");
    if (!target->route || !target->package || !target->activity || !target->plugin_path) {
        wake_target_free(target); return NULL;
    }
    return target;
}

static cJSON *target_json(wake_target_t *target) {
    cJSON *item = cJSON_CreateObject();
    if (!item) return NULL;
    cJSON_AddNumberToObject(item, "user_id", target->user_id);
    cJSON_AddStringToObject(item, "package", target->package);
    cJSON_AddStringToObject(item, "activity", target->activity);
    cJSON_AddStringToObject(item, "plugin_path", target->plugin_path ? target->plugin_path : "");
    return item;
}

static cJSON *targets_json(router_t *router) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    for (wake_target_t *target = router->wake_targets; target; target = target->next)
        cJSON_AddItemToObject(root, target->route, target_json(target));
    return root;
}

static cJSON *status_json(router_t *router) {
    cJSON *status = cJSON_CreateObject();
    if (!status) return NULL;
    cJSON_AddNumberToObject(status, "pid", getpid());
    cJSON_AddBoolToObject(status, "hub_connected", router->hub_connected);
    size_t registered = 0;
    for (plugin_t *p = router->plugins; p; p = p->next) if (p->registered) registered++;
    cJSON_AddNumberToObject(status, "plugin_count", (double)registered);
    cJSON_AddNumberToObject(status, "pending_jobs", (double)router->job_count);
    cJSON_AddNumberToObject(status, "listen_port", router->listen_port);
    cJSON_AddBoolToObject(status, "listen_tcp", router->listen_tcp);
    cJSON_AddStringToObject(status, "plugin_socket", router->plugin_socket);
    cJSON_AddStringToObject(status, "hub_host", router->hub_host);
    cJSON_AddNumberToObject(status, "hub_port", router->hub_port);
    cJSON_AddStringToObject(status, "device_id", router->device_id);
    cJSON_AddBoolToObject(status, "token_configured", router->device_token && router->device_token[0]);
    cJSON *rows = cJSON_AddArrayToObject(status, "routes");
    for (wake_target_t *target = router->wake_targets; target; target = target->next) {
        cJSON *item = target_json(target);
        plugin_t *plugin = find_plugin(router, target->route);
        cJSON_AddStringToObject(item, "route", target->route);
        cJSON_AddStringToObject(item, "app", plugin && plugin->app ? plugin->app : "");
        cJSON_AddBoolToObject(item, "online", plugin != NULL);
        cJSON_AddBoolToObject(item, "configured", true);
        cJSON_AddItemToArray(rows, item);
    }
    for (plugin_t *p = router->plugins; p; p = p->next) {
        if (!p->registered || p->closing) continue;
        for (size_t i = 0; i < p->route_count; ++i) {
            if (find_wake_target(router, p->routes[i])) continue;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "route", p->routes[i]);
            cJSON_AddStringToObject(item, "app", p->app ? p->app : "");
            cJSON_AddBoolToObject(item, "online", true);
            cJSON_AddBoolToObject(item, "configured", false);
            cJSON_AddItemToArray(rows, item);
        }
    }
    return status;
}

static const char *control_configure(router_t *router, cJSON *request) {
    cJSON *host = cJSON_GetObjectItemCaseSensitive(request, "hub_host");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(request, "hub_port");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "device_id");
    cJSON *token = cJSON_GetObjectItemCaseSensitive(request, "device_token");
    if (!text_field(host, 253, true) || !integer_field(port, 1, 65535) ||
        !text_field(id, 128, false) || (token && !text_field(token, 4096, true))) return "invalid_config";
    char *new_host = strdup(host->valuestring), *new_id = strdup(id->valuestring);
    char *new_token = strdup(token ? token->valuestring : router->device_token);
    if (!new_host || !new_id || !new_token) { free(new_host); free(new_id); free(new_token); return "out_of_memory"; }
    cJSON *config = config_json(router, new_host, port->valueint, new_id, new_token);
    bool written = config && atomic_json_write(router->config_path, config);
    cJSON_Delete(config);
    if (!written) { free(new_host); free(new_id); free(new_token); return "persist_failed"; }
    hub_disconnect(router);
    if (router->reconnect_timer) { event_free(router->reconnect_timer); router->reconnect_timer = NULL; }
    free(router->hub_host); router->hub_host = new_host;
    free(router->device_id); router->device_id = new_id;
    free(router->device_token); router->device_token = new_token;
    router->hub_port = port->valueint;
    router->reconnect_seconds = 1;
    hub_connect(router);
    return NULL;
}

static const char *control_route(router_t *router, cJSON *request, bool remove) {
    cJSON *route = cJSON_GetObjectItemCaseSensitive(request, "route");
    if (!text_field(route, 128, false)) return "invalid_route";
    wake_target_t *old = find_wake_target(router, route->valuestring);
    if (remove && !old) return "route_not_found";
    if (!remove && (!target_valid(route->valuestring, request) || (!old && router->wake_target_count >= MAX_TARGETS)))
        return "invalid_route";
    /* The disk write happens before modifying the live registry. */
    cJSON *targets = targets_json(router);
    if (!targets) return "out_of_memory";
    cJSON_DeleteItemFromObjectCaseSensitive(targets, route->valuestring);
    wake_target_t *replacement = remove ? NULL : target_from_json(route->valuestring, request);
    if (!remove && !replacement) { cJSON_Delete(targets); return "out_of_memory"; }
    if (replacement) cJSON_AddItemToObject(targets, replacement->route, target_json(replacement));
    bool written = atomic_json_write(router->wake_targets_path, targets);
    cJSON_Delete(targets);
    if (!written) { wake_target_free(replacement); return "persist_failed"; }
    wake_target_t **slot = &router->wake_targets;
    while (*slot && *slot != old) slot = &(*slot)->next;
    if (old) { *slot = old->next; wake_target_free(old); router->wake_target_count--; }
    if (replacement) { replacement->next = router->wake_targets; router->wake_targets = replacement; router->wake_target_count++; }
    return NULL;
}



static void control_close(control_client_t *client) {
    router_t *router = client->router;
    control_client_t **slot = &router->control_clients;
    while (*slot && *slot != client) slot = &(*slot)->next;
    /* Every live client belongs to exactly this registry. */
    if (!*slot) abort();
    *slot = client->next; router->control_count--;
    if (client->deadline) event_free(client->deadline);
    bufferevent_free(client->bev);
    free(client);
}

static bool token_matches(router_t *router, cJSON *item) {
    if (!cJSON_IsString(item)) return false;
    size_t size = strlen(item->valuestring);
    if (size != 64) return false;
    unsigned char different = 0;
    for (size_t i = 0; i < 64; ++i) different |= (unsigned char)(item->valuestring[i] ^ router->control_token[i]);
    return different == 0;
}

static void control_write_cb(struct bufferevent *bev, void *arg) {
    control_client_t *client = arg;
    if (client->responded && evbuffer_get_length(bufferevent_get_output(bev)) == 0) control_close(client);
}

static void control_read_cb(struct bufferevent *bev, void *arg) {
    control_client_t *client = arg;
    router_t *router = client->router;
    struct evbuffer *input = bufferevent_get_input(bev);
    if (evbuffer_get_length(input) > MAX_CONTROL_LINE) { control_close(client); return; }
    size_t length;
    char *line = evbuffer_readln(input, &length, EVBUFFER_EOL_LF);
    if (!line) return;
    /* One request per connection keeps response attribution and limits simple. */
    bufferevent_disable(bev, EV_READ);
    cJSON *request = parse_message(line, length);
    const char *error = NULL;
    if (!cJSON_IsObject(request)) error = "invalid_request";
    else if (!token_matches(router, cJSON_GetObjectItemCaseSensitive(request, "token"))) error = "unauthorized";
    else {
        cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
        if (!text_field(op, 32, false)) error = "invalid_operation";
        else if (!strcmp(op->valuestring, "status")) { }
        else if (!strcmp(op->valuestring, "reconnect")) {
            hub_disconnect(router);
            if (router->reconnect_timer) { event_free(router->reconnect_timer); router->reconnect_timer = NULL; }
            router->reconnect_seconds = 1;
            hub_connect(router);
        } else if (!strcmp(op->valuestring, "configure")) error = control_configure(router, request);
        else if (!strcmp(op->valuestring, "route_put")) error = control_route(router, request, false);
        else if (!strcmp(op->valuestring, "route_delete")) error = control_route(router, request, true);
        else error = "invalid_operation";
    }
    cJSON_Delete(request); free(line);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", !error);
    if (error) cJSON_AddStringToObject(reply, "error", error);
    else cJSON_AddItemToObject(reply, "status", status_json(router));
    char *encoded = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    if (!encoded) { control_close(client); return; }
    client->responded = true;
    bufferevent_write(bev, encoded, strlen(encoded));
    bufferevent_write(bev, "\n", 1);
    free(encoded);
}

static void control_event_cb(struct bufferevent *bev, short events, void *arg) {
    (void)bev;
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) control_close(arg);
}

static void control_deadline_cb(evutil_socket_t fd, short events, void *arg) {
    (void)fd; (void)events;
    control_close(arg);
}

static void control_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                             struct sockaddr *address, int length, void *arg) {
    (void)listener; (void)address; (void)length;
    router_t *router = arg;
    evutil_make_socket_closeonexec(fd);
    if (router->control_count >= MAX_CONTROL_CLIENTS) { close(fd); return; }
    control_client_t *client = calloc(1, sizeof(*client));
    if (!client) { close(fd); return; }
    client->router = router;
    client->bev = bufferevent_socket_new(router->base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!client->bev) { free(client); close(fd); return; }
    client->next = router->control_clients; router->control_clients = client; router->control_count++;
    struct timeval timeout = {5, 0};
    client->deadline = evtimer_new(router->base, control_deadline_cb, client);
    if (!client->deadline) { control_close(client); return; }
    evtimer_add(client->deadline, &timeout);
    bufferevent_set_timeouts(client->bev, &timeout, &timeout);
    bufferevent_setwatermark(client->bev, EV_READ, 0, MAX_CONTROL_LINE + 1);
    bufferevent_setcb(client->bev, control_read_cb, control_write_cb, control_event_cb, client);
    bufferevent_enable(client->bev, EV_READ | EV_WRITE);
}

static void child_reap_cb(evutil_socket_t signal, short events, void *arg) {
    (void)signal; (void)events;
    router_t *router = arg;
    pid_t child;
    while ((child = waitpid(-1, NULL, WNOHANG)) > 0) {
        if (child == router->resolver_pid) router->resolver_pid = 0;
    }
}

static struct evconnlistener *bind_listener(router_t *router, int port, evconnlistener_cb callback) {
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return evconnlistener_new_bind(router->base, callback, router,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE, 32, (struct sockaddr *)&address, sizeof(address));
}

static struct evconnlistener *bind_unix_listener(router_t *router, const char *name,
                                                 evconnlistener_cb callback) {
    if (!name) return NULL;
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    socklen_t length;
#ifdef __ANDROID__
    if (strlen(name) + 1 > sizeof(address.sun_path)) return NULL;
    memcpy(address.sun_path + 1, name, strlen(name));
    length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(name) + 1);
#else
    if (strlen(name) >= sizeof(address.sun_path)) return NULL;
    strcpy(address.sun_path, name);
    struct stat existing;
    if (lstat(name, &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || unlink(name) != 0) return NULL;
    } else if (errno != ENOENT) return NULL;
    length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(name) + 1);
#endif
    return evconnlistener_new_bind(router->base, callback, router,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC, 32, (struct sockaddr *)&address, length);
}

static bool publish_control(router_t *router) {
    unsigned char random[32];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < sizeof(random)) {
        ssize_t n = read(fd, random + got, sizeof(random) - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != sizeof(random)) return false;
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(router->control_token + i * 2, 3, "%02x", random[i]);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "transport", "unix");
#ifdef __ANDROID__
    cJSON_AddStringToObject(root, "namespace", "abstract");
#else
    cJSON_AddStringToObject(root, "namespace", "filesystem");
#endif
    cJSON_AddStringToObject(root, "name", router->control_socket);
    cJSON_AddStringToObject(root, "token", router->control_token);
    char *path = state_path(router, "control.json");
    bool written = path && atomic_json_write(path, root);
    cJSON_Delete(root); free(path);
    return written;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: dejavuerouter <config.json>\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    int exit_code = 1;
    bool owns_lock = false, config_bound = false, published = false;
    char *metadata = NULL, *lock_path = NULL;
    cJSON *root = NULL;
    config_t cfg = {0};
    struct event *sigint = NULL, *sigterm = NULL, *sigchild = NULL;
    router_t router = {0};
    router.lock_fd = -1;
    router.resolver_fd = -1;
    router.config_path = strdup(argv[1]);
    router.state_dir = strdup(argv[1]);
    if (!router.config_path || !router.state_dir) goto cleanup;
    char *slash = strrchr(router.state_dir, '/');
    if (slash) { if (slash == router.state_dir) slash[1] = 0; else *slash = 0; }
    else { free(router.state_dir); router.state_dir = strdup("."); }
    struct stat directory;
    if (!router.state_dir || stat(router.state_dir, &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != geteuid() || (directory.st_mode & 077) != 0) {
        logmsg("ERROR", "state directory must be private and owned by daemon user"); exit_code = 2; goto cleanup;
    }
    lock_path = state_path(&router, "daemon.lock");
    metadata = state_path(&router, "control.json");
    if (!lock_path || !metadata) goto cleanup;
    router.lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (router.lock_fd < 0 || flock(router.lock_fd, LOCK_EX | LOCK_NB) != 0) {
        logmsg("ERROR", "daemon already running or lock unavailable"); goto cleanup;
    }
    owns_lock = true;
    unlink(metadata);
    root = read_json_file(argv[1], 32768);
    if (!root || !config_parse(root, &cfg)) { logmsg("ERROR", "invalid configuration"); exit_code = 2; goto cleanup; }
    cJSON_Delete(root); root = NULL;
    router.base = event_base_new();
    if (!router.base) { logmsg("ERROR", "cannot initialize event loop"); goto cleanup; }
    router.hub_host = cfg.hub_host; router.hub_port = cfg.hub_port;
    router.device_id = cfg.device_id; router.device_token = cfg.device_token;
    router.listen_host = cfg.listen_host; router.listen_port = cfg.listen_port;
    router.listen_tcp = cfg.listen_tcp; router.reconnect_seconds = 1;
    router.control_name = cfg.control_socket; router.plugin_name = cfg.plugin_socket;
    config_bound = true;
#ifdef __ANDROID__
    router.control_socket = cfg.control_socket; router.plugin_socket = cfg.plugin_socket;
#else
    router.control_socket = state_path(&router, cfg.control_socket);
    router.plugin_socket = state_path(&router, cfg.plugin_socket);
#endif
    router.wake_targets_path = state_path(&router, "wake_targets.json");
    root = router.wake_targets_path ? read_json_file(router.wake_targets_path, MAX_LINE) : NULL;
    if (root) {
        cJSON *item;
        bool valid = cJSON_GetArraySize(root) <= MAX_TARGETS;
        cJSON_ArrayForEach(item, root) {
            if (!item->string || !target_valid(item->string, item)) valid = false;
            for (cJSON *previous = root->child; previous && previous != item; previous = previous->next)
                if (previous->string && item->string && !strcmp(previous->string, item->string)) valid = false;
        }
        if (!valid) { logmsg("ERROR", "invalid wake targets"); exit_code = 2; goto cleanup; }
        cJSON_ArrayForEach(item, root) {
            wake_target_t *target = target_from_json(item->string, item);
            if (!target) goto cleanup;
            target->next = router.wake_targets; router.wake_targets = target; router.wake_target_count++;
        }
        cJSON_Delete(root); root = NULL;
    } else if (!router.wake_targets_path || access(router.wake_targets_path, F_OK) == 0) {
        logmsg("ERROR", "cannot read wake targets"); exit_code = 2; goto cleanup;
    }
    if (router.listen_tcp) router.listener = bind_listener(&router, router.listen_port, listener_cb);
    router.plugin_unix_listener = bind_unix_listener(&router, router.plugin_socket, listener_cb);
    router.control_listener = bind_unix_listener(&router, router.control_socket, control_accept_cb);
    if ((router.listen_tcp && !router.listener) || !router.plugin_unix_listener || !router.control_listener) {
        logmsg("ERROR", "cannot initialize listeners"); goto cleanup;
    }
    sigint = evsignal_new(router.base, SIGINT, signal_cb, &router);
    sigterm = evsignal_new(router.base, SIGTERM, signal_cb, &router);
    sigchild = evsignal_new(router.base, SIGCHLD, child_reap_cb, &router);
    if (!sigint || !sigterm || !sigchild || event_add(sigint, NULL) != 0 ||
        event_add(sigterm, NULL) != 0 || event_add(sigchild, NULL) != 0) {
        logmsg("ERROR", "cannot initialize signal handlers"); goto cleanup;
    }
    if (!publish_control(&router)) { logmsg("ERROR", "cannot publish control metadata"); goto cleanup; }
    published = true;
    hub_connect(&router);
    logmsg("INFO", "router ready");
    exit_code = event_base_dispatch(router.base) < 0 ? 1 : 0;
cleanup:
    router.stopping = true;
    if (owns_lock && published && metadata) unlink(metadata);
    resolver_cancel(&router);
    cJSON_Delete(root);
    drop_all_jobs(&router); drop_all_wakes(&router); wake_targets_free(&router);
    while (router.control_clients) control_close(router.control_clients);
    while (router.plugins) { plugin_t *plugin = router.plugins; router.plugins = plugin->next; free_plugin(plugin); }
    if (router.hub_bev) bufferevent_free(router.hub_bev);
    if (router.reconnect_timer) event_free(router.reconnect_timer);
    if (router.listener) evconnlistener_free(router.listener);
#ifndef __ANDROID__
    if (router.control_listener && router.control_socket) unlink(router.control_socket);
    if (router.plugin_unix_listener && router.plugin_socket) unlink(router.plugin_socket);
    free(router.control_socket); free(router.plugin_socket);
#endif
    if (router.plugin_unix_listener) evconnlistener_free(router.plugin_unix_listener);
    if (router.control_listener) evconnlistener_free(router.control_listener);
    if (sigint) event_free(sigint);
    if (sigterm) event_free(sigterm);
    if (sigchild) event_free(sigchild);
    if (router.base) event_base_free(router.base);
    if (config_bound) { cfg.hub_host = router.hub_host; cfg.device_id = router.device_id; cfg.device_token = router.device_token; }
    config_free(&cfg);
    free(router.wake_targets_path); free(router.config_path); free(router.state_dir);
    free(metadata); free(lock_path);
    if (router.lock_fd >= 0) close(router.lock_fd);
    return exit_code;
}
