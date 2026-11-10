#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ROUTES 32
#define MAX_PLUGINS 64
#define MAX_ROUTE_LEN 64
#define MAX_LINE (1024 * 1024)
#define MAX_RESPONSE (4 * 1024 * 1024)

static volatile sig_atomic_t stop_requested = 0;

static void logmsg(const char *level, const char *fmt, ...) {
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "%s %s dejavurouter: ", stamp, level);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

static void on_signal(int signum) {
    (void)signum;
    stop_requested = 1;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* ------------------------------------------------------------------------- */
/* Tiny JSON DOM parser                                                      */

typedef enum { J_NULL, J_BOOL, J_NUMBER, J_STRING, J_OBJECT, J_ARRAY } JType;

typedef struct {
    JType type;
    int start;
    int end;
    int first;
    int last;
    int next;
    int boolean;
} JNode;

typedef struct {
    const char *text;
    size_t length;
    size_t pos;
    JNode *nodes;
    int count;
    int capacity;
    int error;
} JParser;

static void jp_skip(JParser *p) {
    while (p->pos < p->length && isspace((unsigned char)p->text[p->pos])) p->pos++;
}

static int jp_new(JParser *p, JType type, int start, int end) {
    if (p->count >= p->capacity) {
        int next = p->capacity ? p->capacity * 2 : 64;
        JNode *nodes = realloc(p->nodes, (size_t)next * sizeof(JNode));
        if (!nodes) { p->error = 1; return -1; }
        p->nodes = nodes;
        p->capacity = next;
    }
    int index = p->count++;
    memset(&p->nodes[index], 0, sizeof(JNode));
    p->nodes[index].type = type;
    p->nodes[index].start = start;
    p->nodes[index].end = end;
    p->nodes[index].first = -1;
    p->nodes[index].last = -1;
    p->nodes[index].next = -1;
    return index;
}

static int jp_parse_value(JParser *p, int depth);

static int jp_parse_string(JParser *p) {
    if (p->pos >= p->length || p->text[p->pos] != '"') { p->error = 1; return -1; }
    p->pos++;
    int start = (int)p->pos;
    while (p->pos < p->length) {
        char c = p->text[p->pos];
        if (c == '"') {
            int end = (int)p->pos;
            p->pos++;
            return jp_new(p, J_STRING, start, end);
        }
        if ((unsigned char)c < 0x20) { p->error = 1; return -1; }
        if (c == '\\') {
            p->pos++;
            if (p->pos >= p->length) { p->error = 1; return -1; }
            char e = p->text[p->pos++];
            if (e == 'u') {
                for (int i = 0; i < 4; i++) {
                    if (p->pos >= p->length || !isxdigit((unsigned char)p->text[p->pos])) { p->error = 1; return -1; }
                    p->pos++;
                }
            } else if (!strchr("\"/bfnrt\\", e)) {
                p->error = 1;
                return -1;
            }
        } else {
            p->pos++;
        }
    }
    p->error = 1;
    return -1;
}

static int jp_parse_array(JParser *p, int depth) {
    if (depth > 32) { p->error = 1; return -1; }
    int node = jp_new(p, J_ARRAY, (int)p->pos, (int)p->pos);
    if (node < 0) return -1;
    p->pos++;
    jp_skip(p);
    if (p->pos < p->length && p->text[p->pos] == ']') { p->pos++; p->nodes[node].end = (int)p->pos; return node; }
    while (p->pos < p->length) {
        int child = jp_parse_value(p, depth + 1);
        if (child < 0) return -1;
        if (p->nodes[node].last >= 0) p->nodes[p->nodes[node].last].next = child;
        else p->nodes[node].first = child;
        p->nodes[node].last = child;
        jp_skip(p);
        if (p->pos >= p->length) { p->error = 1; return -1; }
        char c = p->text[p->pos++];
        if (c == ']') { p->nodes[node].end = (int)p->pos; return node; }
        if (c != ',') { p->error = 1; return -1; }
        jp_skip(p);
    }
    p->error = 1;
    return -1;
}

static int jp_parse_object(JParser *p, int depth) {
    if (depth > 32) { p->error = 1; return -1; }
    int node = jp_new(p, J_OBJECT, (int)p->pos, (int)p->pos);
    if (node < 0) return -1;
    p->pos++;
    jp_skip(p);
    if (p->pos < p->length && p->text[p->pos] == '}') { p->pos++; p->nodes[node].end = (int)p->pos; return node; }
    while (p->pos < p->length) {
        int key = jp_parse_string(p);
        if (key < 0) return -1;
        jp_skip(p);
        if (p->pos >= p->length || p->text[p->pos++] != ':') { p->error = 1; return -1; }
        jp_skip(p);
        int value = jp_parse_value(p, depth + 1);
        if (value < 0) return -1;
        if (p->nodes[node].last >= 0) p->nodes[p->nodes[node].last].next = key;
        else p->nodes[node].first = key;
        p->nodes[node].last = key;
        p->nodes[key].next = value;
        p->nodes[node].last = value;
        jp_skip(p);
        if (p->pos >= p->length) { p->error = 1; return -1; }
        char c = p->text[p->pos++];
        if (c == '}') { p->nodes[node].end = (int)p->pos; return node; }
        if (c != ',') { p->error = 1; return -1; }
        jp_skip(p);
    }
    p->error = 1;
    return -1;
}

static int jp_parse_value(JParser *p, int depth) {
    jp_skip(p);
    if (p->pos >= p->length) { p->error = 1; return -1; }
    char c = p->text[p->pos];
    if (c == '{') return jp_parse_object(p, depth);
    if (c == '[') return jp_parse_array(p, depth);
    if (c == '"') return jp_parse_string(p);
    if (c == 't' && p->pos + 4 <= p->length && !memcmp(p->text + p->pos, "true", 4)) {
        int node = jp_new(p, J_BOOL, (int)p->pos, (int)p->pos + 4);
        if (node >= 0) { p->nodes[node].boolean = 1; p->pos += 4; }
        return node;
    }
    if (c == 'f' && p->pos + 5 <= p->length && !memcmp(p->text + p->pos, "false", 5)) {
        int node = jp_new(p, J_BOOL, (int)p->pos, (int)p->pos + 5);
        if (node >= 0) { p->nodes[node].boolean = 0; p->pos += 5; }
        return node;
    }
    if (c == 'n' && p->pos + 4 <= p->length && !memcmp(p->text + p->pos, "null", 4)) {
        int node = jp_new(p, J_NULL, (int)p->pos, (int)p->pos + 4);
        if (node >= 0) p->pos += 4;
        return node;
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        int start = (int)p->pos;
        p->pos++;
        while (p->pos < p->length) {
            char d = p->text[p->pos];
            if (isdigit((unsigned char)d) || d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') p->pos++;
            else break;
        }
        return jp_new(p, J_NUMBER, start, (int)p->pos);
    }
    p->error = 1;
    return -1;
}

static JNode *jp_parse(const char *text, size_t length, JParser *p) {
    memset(p, 0, sizeof(*p));
    p->text = text;
    p->length = length;
    int root = jp_parse_value(p, 0);
    if (root < 0) return NULL;
    jp_skip(p);
    if (p->pos != p->length) { p->error = 1; return NULL; }
    /* The root is node 0 by construction. */
    return &p->nodes[root];
}

static void jp_free(JParser *p) {
    free(p->nodes);
    memset(p, 0, sizeof(*p));
}

static int json_unescape(const char *text, int start, int end, char *out, size_t cap) {
    size_t written = 0;
    for (int i = start; i < end; i++) {
        char c = text[i];
        if (c != '\\') {
            if (written + 1 >= cap) return -1;
            out[written++] = c;
            continue;
        }
        if (++i >= end) return -1;
        char e = text[i];
        if (e == 'n') c = '\n';
        else if (e == 'r') c = '\r';
        else if (e == 't') c = '\t';
        else if (e == 'b') c = '\b';
        else if (e == 'f') c = '\f';
        else if (e == '"' || e == '\\' || e == '/') c = e;
        else if (e == 'u') {
            if (i + 4 >= end) return -1;
            char hex[5] = {text[i+1], text[i+2], text[i+3], text[i+4], 0};
            unsigned cp = (unsigned)strtoul(hex, NULL, 16);
            if (cp < 0x80) c = (char)cp;
            else if (cp < 0x800) {
                if (written + 2 >= cap) return -1;
                out[written++] = (char)(0xC0 | (cp >> 6));
                out[written++] = (char)(0x80 | (cp & 0x3F));
                i += 4;
                continue;
            } else {
                if (written + 3 >= cap) return -1;
                out[written++] = (char)(0xE0 | (cp >> 12));
                out[written++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[written++] = (char)(0x80 | (cp & 0x3F));
                i += 4;
                continue;
            }
            i += 4;
        } else return -1;
        if (written + 1 >= cap) return -1;
        out[written++] = c;
    }
    if (written >= cap) return -1;
    out[written] = 0;
    return 0;
}

static int json_copy_string(JParser *p, int node, char *out, size_t cap) {
    if (node < 0 || node >= p->count || p->nodes[node].type != J_STRING) return -1;
    return json_unescape(p->text, p->nodes[node].start, p->nodes[node].end, out, cap);
}

static int json_object_get(JParser *p, int object, const char *key) {
    if (object < 0 || object >= p->count || p->nodes[object].type != J_OBJECT) return -1;
    int child = p->nodes[object].first;
    while (child >= 0) {
        int value = p->nodes[child].next;
        if (value < 0) break;
        char name[256];
        if (json_copy_string(p, child, name, sizeof(name)) == 0 && strcmp(name, key) == 0) return value;
        child = p->nodes[value].next;
    }
    return -1;
}

static int json_array_count(JParser *p, int array) {
    if (array < 0 || array >= p->count || p->nodes[array].type != J_ARRAY) return 0;
    int count = 0;
    for (int child = p->nodes[array].first; child >= 0; child = p->nodes[child].next) count++;
    return count;
}

static int json_array_item(JParser *p, int array, int index) {
    if (array < 0 || array >= p->count || p->nodes[array].type != J_ARRAY) return -1;
    int child = p->nodes[array].first;
    for (int i = 0; child >= 0 && i < index; i++) child = p->nodes[child].next;
    return child;
}

static char *json_raw_copy(JParser *p, int node) {
    if (node < 0 || node >= p->count) return NULL;
    int start = p->nodes[node].start;
    int end = p->nodes[node].end;
    size_t length = (size_t)(end - start);
    char *out = malloc(length + 1);
    if (!out) return NULL;
    memcpy(out, p->text + start, length);
    out[length] = 0;
    return out;
}


/* ------------------------------------------------------------------------- */
/* Growable string buffer                                                    */

typedef struct { char *data; size_t len; size_t cap; } Buf;

static int buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) cap *= 2;
    char *data = realloc(b->data, cap);
    if (!data) return -1;
    b->data = data;
    b->cap = cap;
    return 0;
}

static int buf_append(Buf *b, const char *data, size_t length) {
    if (buf_reserve(b, length) != 0) return -1;
    memcpy(b->data + b->len, data, length);
    b->len += length;
    b->data[b->len] = 0;
    return 0;
}

static int buf_puts(Buf *b, const char *text) { return buf_append(b, text, strlen(text)); }

static int buf_printf(Buf *b, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) { va_end(args); return -1; }
    if (buf_reserve(b, (size_t)needed) != 0) { va_end(args); return -1; }
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, args);
    b->len += (size_t)needed;
    va_end(args);
    return 0;
}

static int buf_json_string(Buf *b, const char *text) {
    if (buf_puts(b, "\"") != 0) return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        switch (*p) {
            case '"': if (buf_puts(b, "\\\"") != 0) return -1; break;
            case '\\': if (buf_puts(b, "\\\\") != 0) return -1; break;
            case '\n': if (buf_puts(b, "\\n") != 0) return -1; break;
            case '\r': if (buf_puts(b, "\\r") != 0) return -1; break;
            case '\t': if (buf_puts(b, "\\t") != 0) return -1; break;
            default:
                if (*p < 0x20) { if (buf_printf(b, "\\u%04x", *p) != 0) return -1; }
                else if (buf_append(b, (const char *)p, 1) != 0) return -1;
        }
    }
    return buf_puts(b, "\"");
}

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */

typedef struct {
    char name[MAX_ROUTE_LEN];
    char app[128];
    char wake[1024];
    int plugin_wait;
} Route;

typedef struct {
    char hub_url[512];
    char device_id[128];
    char device_token[256];
    char listen_host[128];
    int listen_port;
    char plugin_token[256];
    int poll_wait;
    int plugin_wait;
    Route routes[MAX_ROUTES];
    int route_count;
} Config;

static int json_string_value(JParser *p, int object, const char *key, char *out, size_t cap) {
    int node = json_object_get(p, object, key);
    if (node < 0) return -1;
    return json_copy_string(p, node, out, cap);
}

static int json_int_value(JParser *p, int object, const char *key, int fallback) {
    int node = json_object_get(p, object, key);
    if (node < 0 || p->nodes[node].type != J_NUMBER) return fallback;
    char buf[64];
    size_t length = (size_t)(p->nodes[node].end - p->nodes[node].start);
    if (length >= sizeof(buf)) return fallback;
    memcpy(buf, p->text + p->nodes[node].start, length);
    buf[length] = 0;
    return atoi(buf);
}

static int config_load(const char *path, Config *cfg, char *err, size_t errcap) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(err, errcap, "cannot open config: %s", strerror(errno)); return -1; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); snprintf(err, errcap, "cannot seek config"); return -1; }
    long length = ftell(fp);
    if (length < 0 || length > 4 * 1024 * 1024) { fclose(fp); snprintf(err, errcap, "config is too large"); return -1; }
    rewind(fp);
    char *text = malloc((size_t)length + 1);
    if (!text) { fclose(fp); snprintf(err, errcap, "out of memory"); return -1; }
    size_t got = fread(text, 1, (size_t)length, fp);
    fclose(fp);
    text[got] = 0;
    JParser parser;
    JNode *root = jp_parse(text, got, &parser);
    if (!root || root->type != J_OBJECT) { snprintf(err, errcap, "config is not a JSON object"); jp_free(&parser); free(text); return -1; }
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_port = 39111;
    cfg->poll_wait = 25;
    cfg->plugin_wait = 20;
    snprintf(cfg->listen_host, sizeof(cfg->listen_host), "127.0.0.1");
    snprintf(cfg->device_id, sizeof(cfg->device_id), "dejavu-device");
    if (json_string_value(&parser, 0, "hub_url", cfg->hub_url, sizeof(cfg->hub_url)) != 0) {
        snprintf(err, errcap, "hub_url is required"); jp_free(&parser); free(text); return -1;
    }
    json_string_value(&parser, 0, "device_id", cfg->device_id, sizeof(cfg->device_id));
    json_string_value(&parser, 0, "device_token", cfg->device_token, sizeof(cfg->device_token));
    json_string_value(&parser, 0, "listen_host", cfg->listen_host, sizeof(cfg->listen_host));
    json_string_value(&parser, 0, "plugin_token", cfg->plugin_token, sizeof(cfg->plugin_token));
    cfg->listen_port = json_int_value(&parser, 0, "listen_port", cfg->listen_port);
    cfg->poll_wait = json_int_value(&parser, 0, "poll_wait_seconds", cfg->poll_wait);
    cfg->plugin_wait = json_int_value(&parser, 0, "plugin_wait_seconds", cfg->plugin_wait);
    int routes = json_object_get(&parser, 0, "routes");
    if (routes >= 0 && parser.nodes[routes].type == J_OBJECT) {
        int key = parser.nodes[routes].first;
        while (key >= 0 && cfg->route_count < MAX_ROUTES) {
            int value = parser.nodes[key].next;
            if (value < 0) break;
            Route *route = &cfg->routes[cfg->route_count];
            memset(route, 0, sizeof(*route));
            if (json_copy_string(&parser, key, route->name, sizeof(route->name)) != 0) break;
            if (parser.nodes[value].type == J_OBJECT) {
                json_string_value(&parser, value, "app", route->app, sizeof(route->app));
                json_string_value(&parser, value, "wake", route->wake, sizeof(route->wake));
                route->plugin_wait = json_int_value(&parser, value, "plugin_wait_seconds", cfg->plugin_wait);
            }
            cfg->route_count++;
            key = parser.nodes[value].next;
        }
    }
    jp_free(&parser);
    free(text);
    if (cfg->listen_port <= 0 || cfg->listen_port > 65535) { snprintf(err, errcap, "invalid listen_port"); return -1; }
    return 0;
}

static Route *route_find(Config *cfg, const char *name) {
    for (int i = 0; i < cfg->route_count; i++) if (!strcmp(cfg->routes[i].name, name)) return &cfg->routes[i];
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Minimal HTTP/1.1 client (http:// only; TLS terminates in a local proxy)   */

typedef struct { char host[256]; int port; char path[1024]; } Url;

static int url_parse(const char *url, Url *out, char *err, size_t errcap) {
    if (!url || strncmp(url, "http://", 7) != 0) {
        snprintf(err, errcap, "only http:// hub URLs are supported by this build");
        return -1;
    }
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    const char *end = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(end - p));
    size_t host_len = (size_t)((colon ? colon : end) - p);
    if (host_len == 0 || host_len >= sizeof(out->host)) { snprintf(err, errcap, "invalid hub host"); return -1; }
    memcpy(out->host, p, host_len);
    out->host[host_len] = 0;
    out->port = 80;
    if (colon) out->port = atoi(colon + 1);
    if (out->port <= 0 || out->port > 65535) { snprintf(err, errcap, "invalid hub port"); return -1; }
    if (slash) snprintf(out->path, sizeof(out->path), "%s", slash);
    else snprintf(out->path, sizeof(out->path), "/");
    return 0;
}

static int connect_tcp(const char *host, int port, int timeout_seconds) {
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char service[16];
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &result) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *entry = result; entry; entry = entry->ai_next) {
        fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { .tv_sec = timeout_seconds, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    return fd;
}

static int send_all(int fd, const char *data, size_t length) {
    size_t done = 0;
    while (done < length) {
        ssize_t n = send(fd, data + done, length - done, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int http_request(const char *url, const char *method, const char *token,
                        const char *body, int timeout_seconds, int *status_out,
                        char **response_out) {
    Url parsed;
    char err[256];
    if (url_parse(url, &parsed, err, sizeof(err)) != 0) {
        logmsg("ERROR", "%s", err);
        return -1;
    }
    int fd = connect_tcp(parsed.host, parsed.port, timeout_seconds);
    if (fd < 0) { logmsg("WARN", "connect %s:%d failed: %s", parsed.host, parsed.port, strerror(errno)); return -1; }
    Buf request = {0};
    size_t body_len = body ? strlen(body) : 0;
    buf_printf(&request, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nAccept: application/json\r\n", method, parsed.path, parsed.host, parsed.port);
    if (token && *token) buf_printf(&request, "Authorization: Bearer %s\r\n", token);
    if (body) buf_printf(&request, "Content-Type: application/json\r\nContent-Length: %zu\r\n", body_len);
    buf_puts(&request, "\r\n");
    if (body) buf_append(&request, body, body_len);
    int rc = send_all(fd, request.data, request.len);
    free(request.data);
    if (rc != 0) { close(fd); return -1; }
    Buf response = {0};
    char chunk[8192];
    while (1) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
        if (response.len + (size_t)n > MAX_RESPONSE) break;
        buf_append(&response, chunk, (size_t)n);
    }
    close(fd);
    if (!response.data) return -1;
    char *header_end = strstr(response.data, "\r\n\r\n");
    if (!header_end) { free(response.data); return -1; }
    int status = 0;
    if (sscanf(response.data, "HTTP/%*s %d", &status) != 1) { free(response.data); return -1; }
    char *body_start = header_end + 4;
    size_t body_size = response.len - (size_t)(body_start - response.data);
    long content_length = -1;
    char *line = response.data;
    while (line < header_end) {
        char *line_end = strstr(line, "\r\n");
        if (!line_end) break;
        if (!strncasecmp(line, "Content-Length:", 15)) content_length = strtol(line + 15, NULL, 10);
        line = line_end + 2;
    }
    if (content_length >= 0 && (size_t)content_length <= body_size) body_size = (size_t)content_length;
    char *out = malloc(body_size + 1);
    if (!out) { free(response.data); return -1; }
    memcpy(out, body_start, body_size);
    out[body_size] = 0;
    free(response.data);
    if (status_out) *status_out = status;
    if (response_out) *response_out = out;
    else free(out);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Plugin server and registry                                                */

typedef struct Pending {
    char id[64];
    char *response;
    int done;
    pthread_cond_t cond;
    struct Pending *next;
} Pending;

typedef struct Plugin {
    int fd;
    pthread_t thread;
    char app[128];
    char name[128];
    char routes[16][MAX_ROUTE_LEN];
    int route_count;
    int alive;
    pthread_mutex_t lock;
    Pending *pending;
    struct Plugin *next;
} Plugin;

typedef struct {
    Plugin *head;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} Registry;

static Registry registry;
static int plugin_serial = 0;

static int write_all_fd(int fd, const char *data, size_t length) {
    size_t done = 0;
    while (done < length) {
        ssize_t n = send(fd, data + done, length - done, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int read_line_fd(int fd, char **line) {
    Buf buf = {0};
    char c;
    while (1) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) { free(buf.data); return -1; }
        if (c == '\n') {
            if (buf.len > 0 && buf.data[buf.len - 1] == '\r') buf.len--;
            if (!buf.data) buf.data = calloc(1, 1);
            else buf.data[buf.len] = 0;
            *line = buf.data;
            return 0;
        }
        if (buf.len >= MAX_LINE) { free(buf.data); return -1; }
        if (buf_append(&buf, &c, 1) != 0) { free(buf.data); return -1; }
    }
}

static Plugin *registry_find_route(const char *route) {
    Plugin *plugin = registry.head;
    while (plugin) {
        if (plugin->alive) {
            for (int i = 0; i < plugin->route_count; i++) {
                if (!strcmp(plugin->routes[i], route)) return plugin;
            }
        }
        plugin = plugin->next;
    }
    return NULL;
}

static Plugin *registry_wait_route(const char *route, int timeout_seconds) {
    long long deadline = now_ms() + (long long)timeout_seconds * 1000LL;
    pthread_mutex_lock(&registry.lock);
    Plugin *found = NULL;
    while (!stop_requested) {
        found = registry_find_route(route);
        if (found) break;
        long long remaining = deadline - now_ms();
        if (remaining <= 0) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += remaining / 1000;
        ts.tv_nsec += (remaining % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&registry.cond, &registry.lock, &ts);
    }
    pthread_mutex_unlock(&registry.lock);
    return found;
}

static int plugin_send(Plugin *plugin, const char *line) {
    pthread_mutex_lock(&plugin->lock);
    int rc = plugin->alive ? write_all_fd(plugin->fd, line, strlen(line)) : -1;
    if (rc == 0) rc = write_all_fd(plugin->fd, "\n", 1);
    pthread_mutex_unlock(&plugin->lock);
    return rc;
}

static void plugin_register(Plugin *plugin) {
    pthread_mutex_lock(&registry.lock);
    plugin->next = registry.head;
    registry.head = plugin;
    pthread_cond_broadcast(&registry.cond);
    pthread_mutex_unlock(&registry.lock);
    logmsg("INFO", "plugin connected app=%s plugin=%s routes=%d", plugin->app, plugin->name, plugin->route_count);
}

static void *plugin_reader(void *arg) {
    Plugin *plugin = arg;
    char *line = NULL;
    while (!stop_requested && plugin->alive && read_line_fd(plugin->fd, &line) == 0) {
        JParser parser;
        JNode *root = jp_parse(line, strlen(line), &parser);
        if (!root || root->type != J_OBJECT) { jp_free(&parser); free(line); continue; }
        char type[32] = {0};
        json_string_value(&parser, 0, "type", type, sizeof(type));
        if (!strcmp(type, "response")) {
            char id[64] = {0};
            json_string_value(&parser, 0, "id", id, sizeof(id));
            pthread_mutex_lock(&plugin->lock);
            Pending *pending = plugin->pending;
            while (pending && strcmp(pending->id, id)) pending = pending->next;
            if (pending) {
                free(pending->response);
                pending->response = strdup(line);
                pending->done = 1;
                pthread_cond_broadcast(&pending->cond);
            }
            pthread_mutex_unlock(&plugin->lock);
        } else if (!strcmp(type, "ping")) {
            plugin_send(plugin, "{\"type\":\"pong\"}");
        }
        jp_free(&parser);
        free(line);
        line = NULL;
    }
    free(line);
    pthread_mutex_lock(&plugin->lock);
    plugin->alive = 0;
    pthread_mutex_unlock(&plugin->lock);
    pthread_mutex_lock(&registry.lock);
    pthread_cond_broadcast(&registry.cond);
    pthread_mutex_unlock(&registry.lock);
    logmsg("INFO", "plugin disconnected app=%s plugin=%s", plugin->app, plugin->name);
    close(plugin->fd);
    return NULL;
}

static void *plugin_listener(void *arg) {
    Config *cfg = arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { logmsg("ERROR", "plugin socket failed"); return NULL; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg->listen_port);
    if (inet_pton(AF_INET, cfg->listen_host, &addr.sin_addr) != 1) {
        logmsg("ERROR", "invalid listen_host"); close(fd); return NULL;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        logmsg("ERROR", "plugin bind %s:%d failed: %s", cfg->listen_host, cfg->listen_port, strerror(errno));
        close(fd); return NULL;
    }
    if (listen(fd, 32) != 0) { close(fd); return NULL; }
    logmsg("INFO", "plugin router listening on %s:%d", cfg->listen_host, cfg->listen_port);
    while (!stop_requested) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 500);
        if (ready <= 0) continue;
        int client = accept(fd, NULL, NULL);
        if (client < 0) continue;
        char *hello_line = NULL;
        if (read_line_fd(client, &hello_line) != 0) { close(client); continue; }
        JParser parser;
        JNode *root = jp_parse(hello_line, strlen(hello_line), &parser);
        if (!root || root->type != J_OBJECT) { free(hello_line); jp_free(&parser); close(client); continue; }
        char token[256] = {0}, app[128] = {0}, name[128] = {0};
        json_string_value(&parser, 0, "token", token, sizeof(token));
        json_string_value(&parser, 0, "app", app, sizeof(app));
        json_string_value(&parser, 0, "plugin", name, sizeof(name));
        if (*cfg->plugin_token && strcmp(token, cfg->plugin_token)) {
            const char *error = "{\"type\":\"error\",\"error\":\"unauthorized\"}\n";
            write_all_fd(client, error, strlen(error));
            free(hello_line); jp_free(&parser); close(client); continue;
        }
        Plugin *plugin = calloc(1, sizeof(Plugin));
        if (!plugin) { free(hello_line); jp_free(&parser); close(client); continue; }
        plugin->fd = client;
        plugin->alive = 1;
        snprintf(plugin->app, sizeof(plugin->app), "%s", app);
        snprintf(plugin->name, sizeof(plugin->name), "%s", *name ? name : "plugin");
        pthread_mutex_init(&plugin->lock, NULL);
        int routes = json_object_get(&parser, 0, "routes");
        if (routes >= 0) {
            int count = json_array_count(&parser, routes);
            if (count > 16) count = 16;
            for (int i = 0; i < count; i++) {
                int item = json_array_item(&parser, routes, i);
                if (item >= 0) json_copy_string(&parser, item, plugin->routes[plugin->route_count++], MAX_ROUTE_LEN);
            }
        }
        plugin_register(plugin);
        const char *ok = "{\"type\":\"hello_ok\",\"version\":1}\n";
        write_all_fd(client, ok, strlen(ok));
        pthread_create(&plugin->thread, NULL, plugin_reader, plugin);
        free(hello_line);
        jp_free(&parser);
    }
    close(fd);
    return NULL;
}

static int plugin_call(Plugin *plugin, const char *request_id, const char *request_line, int timeout_seconds, char **response, char *err, size_t errcap) {
    Pending pending;
    memset(&pending, 0, sizeof(pending));
    snprintf(pending.id, sizeof(pending.id), "%s", request_id);
    pthread_cond_init(&pending.cond, NULL);
    pthread_mutex_lock(&plugin->lock);
    pending.next = plugin->pending;
    plugin->pending = &pending;
    pthread_mutex_unlock(&plugin->lock);
    if (plugin_send(plugin, request_line) != 0) {
        snprintf(err, errcap, "plugin send failed");
    } else {
        long long deadline = now_ms() + (long long)(timeout_seconds > 0 ? timeout_seconds : 0) * 1000LL;
        pthread_mutex_lock(&plugin->lock);
        while (!pending.done && plugin->alive) {
            long long remaining = deadline - now_ms();
            if (remaining <= 0) break;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += remaining / 1000;
            ts.tv_nsec += (remaining % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&pending.cond, &plugin->lock, &ts);
        }
        if (!pending.done) snprintf(err, errcap, "plugin response timed out");
        pthread_mutex_unlock(&plugin->lock);
    }
    /* remove from plugin list */
    pthread_mutex_lock(&plugin->lock);
    Pending **slot = &plugin->pending;
    while (*slot && *slot != &pending) slot = &(*slot)->next;
    if (*slot == &pending) *slot = pending.next;
    pthread_mutex_unlock(&plugin->lock);
    if (*err == 0 && pending.done && pending.response) {
        *response = strdup(pending.response);
        pthread_cond_destroy(&pending.cond);
        return 0;
    }
    if (*err == 0) {
        snprintf(err, errcap, pending.done ? "plugin_empty_response" : "plugin_disconnected");
    }
    pthread_cond_destroy(&pending.cond);
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Routing and wake                                                          */

static int run_wake(const char *command) {
    if (!command || !*command) return 0;
    logmsg("INFO", "wake: %s", command);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        logmsg("WARN", "wake command exit=%d", WEXITSTATUS(status));
        return -1;
    }
    return 0;
}

static char *route_job(Config *cfg, JParser *parser, char *err, size_t errcap) {
    int route_node = json_object_get(parser, 0, "route");
    int method_node = json_object_get(parser, 0, "method");
    char route[MAX_ROUTE_LEN] = {0};
    char method[128] = {0};
    if (method_node >= 0) json_copy_string(parser, method_node, method, sizeof(method));
    if (route_node >= 0) json_copy_string(parser, route_node, route, sizeof(route));
    if (!*route) {
        char *dot = strchr(method, '.');
        if (dot) {
            size_t n = (size_t)(dot - method);
            if (n >= sizeof(route)) n = sizeof(route) - 1;
            memcpy(route, method, n);
        }
    }
    if (!*route && !strncmp(method, "x.", 2)) snprintf(route, sizeof(route), "x");
    if (!*route) { snprintf(err, errcap, "route_required"); return NULL; }
    Route *route_cfg = route_find(cfg, route);
    int wait_seconds = route_cfg ? route_cfg->plugin_wait : cfg->plugin_wait;
    Plugin *plugin = registry_find_route(route);
    if (!plugin) {
        if (route_cfg) run_wake(route_cfg->wake);
        plugin = registry_wait_route(route, wait_seconds);
    }
    if (!plugin) {
        snprintf(err, errcap, "plugin_unavailable");
        return NULL;
    }
    int params_node = json_object_get(parser, 0, "params");
    char *params = params_node >= 0 ? json_raw_copy(parser, params_node) : strdup("{}");
    char request_id[64];
    snprintf(request_id, sizeof(request_id), "rpc_%d_%lld",
             __sync_add_and_fetch(&plugin_serial, 1), now_ms());
    Buf request = {0};
    buf_puts(&request, "{\"type\":\"request\",\"id\":");
    buf_json_string(&request, request_id);
    buf_puts(&request, ",\"route\":");
    buf_json_string(&request, route);
    buf_puts(&request, ",\"method\":");
    buf_json_string(&request, method);
    buf_puts(&request, ",\"params\":");
    buf_puts(&request, params ? params : "{}");
    buf_puts(&request, "}");
    free(params);
    int deadline_ms = json_int_value(parser, 0, "deadline_ms", 60000);
    int timeout = deadline_ms > 0 ? (deadline_ms + 999) / 1000 : 60;
    char call_err[128] = {0};
    char *response = NULL;
    if (plugin_call(plugin, request_id, request.data, timeout, &response, call_err, sizeof(call_err)) != 0) {
        free(request.data);
        snprintf(err, errcap, "%s", *call_err ? call_err : "plugin_error");
        return NULL;
    }
    free(request.data);
    if (!response) { snprintf(err, errcap, "plugin_empty_response"); return NULL; }
    JParser resp_parser;
    JNode *resp_root = jp_parse(response, strlen(response), &resp_parser);
    if (!resp_root || resp_root->type != J_OBJECT) {
        jp_free(&resp_parser); free(response); snprintf(err, errcap, "plugin_invalid_response"); return NULL;
    }
    int ok_node = json_object_get(&resp_parser, 0, "ok");
    int ok = ok_node >= 0 && resp_parser.nodes[ok_node].type == J_BOOL && resp_parser.nodes[ok_node].boolean;
    char *result = NULL;
    if (ok) {
        int result_node = json_object_get(&resp_parser, 0, "result");
        result = result_node >= 0 ? json_raw_copy(&resp_parser, result_node) : strdup("{}");
    } else {
        int error_node = json_object_get(&resp_parser, 0, "error");
        char *raw_error = error_node >= 0 ? json_raw_copy(&resp_parser, error_node) : NULL;
        if (raw_error) {
            snprintf(err, errcap, "%.*s", (int)strlen(raw_error), raw_error);
            free(raw_error);
        } else snprintf(err, errcap, "plugin_error");
    }
    jp_free(&resp_parser);
    free(response);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Hub loop                                                                  */

static int hub_hello(Config *cfg) {
    Buf body = {0};
    buf_puts(&body, "{\"device_id\":");
    buf_json_string(&body, cfg->device_id);
    buf_puts(&body, ",\"protocol\":1,\"accounts\":[],\"capabilities\":[\"router.plugin\"]}");
    Buf url = {0};
    buf_puts(&url, cfg->hub_url);
    buf_puts(&url, "/device/hello");
    int status = 0;
    char *response = NULL;
    int rc = http_request(url.data, "POST", cfg->device_token, body.data, 20, &status, &response);
    free(url.data);
    free(body.data);
    if (rc != 0 || status / 100 != 2) {
        free(response);
        logmsg("WARN", "hub hello failed status=%d", status);
        return -1;
    }
    free(response);
    return 0;
}

static char *hub_poll(Config *cfg) {
    Buf url = {0};
    buf_puts(&url, cfg->hub_url);
    buf_printf(&url, "/device/poll?device_id=%s&wait=%d", cfg->device_id, cfg->poll_wait);
    int status = 0;
    char *response = NULL;
    int rc = http_request(url.data, "GET", cfg->device_token, NULL, cfg->poll_wait + 10, &status, &response);
    free(url.data);
    if (rc != 0) { free(response); return NULL; }
    if (status == 204 || !response || !*response) { free(response); return NULL; }
    if (status / 100 != 2) { free(response); return NULL; }
    JParser parser;
    JNode *root = jp_parse(response, strlen(response), &parser);
    if (!root || root->type != J_OBJECT) { jp_free(&parser); free(response); return NULL; }
    int job_node = json_object_get(&parser, 0, "job");
    char *job = job_node >= 0 ? json_raw_copy(&parser, job_node) : NULL;
    jp_free(&parser);
    free(response);
    return job;
}

static int hub_send_result(Config *cfg, const char *job_id, int ok, const char *payload) {
    Buf body = {0};
    buf_printf(&body, "{\"device_id\":");
    buf_json_string(&body, cfg->device_id);
    buf_puts(&body, ",\"id\":");
    buf_json_string(&body, job_id);
    buf_puts(&body, ",\"ok\":");
    buf_puts(&body, ok ? "true" : "false");
    if (ok) {
        buf_puts(&body, ",\"result\":");
        buf_puts(&body, payload ? payload : "{}");
    } else {
        buf_puts(&body, ",\"error\":{\"code\":");
        buf_json_string(&body, payload ? payload : "plugin_error");
        buf_puts(&body, ",\"message\":\"router error\",\"retryable\":true}");
    }
    buf_puts(&body, "}");
    Buf url = {0};
    buf_puts(&url, cfg->hub_url);
    buf_puts(&url, "/device/result");
    int status = 0;
    char *response = NULL;
    int rc = http_request(url.data, "POST", cfg->device_token, body.data, 20, &status, &response);
    free(url.data);
    free(body.data);
    free(response);
    return rc == 0 && status / 100 == 2 ? 0 : -1;
}

static void *hub_loop(void *arg) {
    Config *cfg = arg;
    int backoff = 1;
    while (!stop_requested) {
        if (hub_hello(cfg) != 0) {
            sleep((unsigned)backoff);
            if (backoff < 30) backoff *= 2;
            continue;
        }
        backoff = 1;
        logmsg("INFO", "registered device=%s", cfg->device_id);
        while (!stop_requested) {
            char *job = hub_poll(cfg);
            if (!job) continue;
            JParser parser;
            JNode *root = jp_parse(job, strlen(job), &parser);
            if (!root || root->type != J_OBJECT) { jp_free(&parser); free(job); continue; }
            char job_id[128] = {0};
            json_string_value(&parser, 0, "id", job_id, sizeof(job_id));
            char error[512] = {0};
            char *result = route_job(cfg, &parser, error, sizeof(error));
            if (result) {
                hub_send_result(cfg, job_id, 1, result);
                free(result);
                logmsg("INFO", "job %s ok", job_id);
            } else {
                hub_send_result(cfg, job_id, 0, error);
                logmsg("WARN", "job %s failed: %s", job_id, error);
            }
            jp_free(&parser);
            free(job);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *config_path = argc > 1 ? argv[1] : "config.json";
    Config cfg;
    char err[512] = {0};
    if (config_load(config_path, &cfg, err, sizeof(err)) != 0) {
        logmsg("ERROR", "%s", err);
        return 2;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    pthread_mutex_init(&registry.lock, NULL);
    pthread_cond_init(&registry.cond, NULL);
    pthread_t listener;
    if (pthread_create(&listener, NULL, plugin_listener, &cfg) != 0) {
        logmsg("ERROR", "cannot start plugin listener");
        return 1;
    }
    logmsg("INFO", "dejavurouter starting hub=%s device=%s", cfg.hub_url, cfg.device_id);
    hub_loop(&cfg);
    stop_requested = 1;
    pthread_join(listener, NULL);
    return 0;
}