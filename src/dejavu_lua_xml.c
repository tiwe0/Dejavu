#include "dejavu_lua_xml.h"
#include "lauxlib.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XML_MAX_INPUT (4 * 1024 * 1024)
#define XML_MAX_OUTPUT (4 * 1024 * 1024)
#define XML_MAX_DEPTH 64
#define XML_MAX_NODES 65536
#define XML_MAX_ATTRS 1024
#define XML_NODE_META "dejavu.xml.node"
#define XML_STORAGE_META "dejavu.xml.storage"

static int is_valid_xml_codepoint(uint32_t cp) {
    return cp == 0x9u || cp == 0xAu || cp == 0xDu || (cp >= 0x20u && cp <= 0xD7FFu) ||
           (cp >= 0xE000u && cp <= 0xFFFDu) || (cp >= 0x10000u && cp <= 0x10FFFFu);
}

/* Validate UTF-8 and enforce XML 1.0 §2.2 Char rules (rejects NUL, illegal controls, surrogates,
 * U+FFFE, U+FFFF, overlong sequences, and invalid UTF-8). */
static int valid_xml_utf8(const char *text, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)text[i++];
        uint32_t value, minimum;
        int extra;
        if (c < 0x80u) {
            if (!is_valid_xml_codepoint(c)) {
                return 0;
            }
            continue;
        }
        if (c >= 0xc2u && c <= 0xdfu) {
            value = c & 0x1fu;
            extra = 1;
            minimum = 0x80u;
        } else if (c >= 0xe0u && c <= 0xefu) {
            value = c & 0x0fu;
            extra = 2;
            minimum = 0x800u;
        } else if (c >= 0xf0u && c <= 0xf4u) {
            value = c & 0x07u;
            extra = 3;
            minimum = 0x10000u;
        } else {
            return 0;
        }
        while (extra--) {
            if (i == length || ((unsigned char)text[i] & 0xc0u) != 0x80u) {
                return 0;
            }
            value = (value << 6) | ((unsigned char)text[i++] & 0x3fu);
        }
        if (value < minimum || !is_valid_xml_codepoint(value)) {
            return 0;
        }
    }
    return 1;
}

static int is_xml_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_name_start_byte(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c >= 0x80u;
}

static int is_name_byte(unsigned char c) {
    return is_name_start_byte(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

static int valid_xml_name(const char *name, size_t len) {
    if (len == 0 || !valid_xml_utf8(name, len)) {
        return 0;
    }
    if (!is_name_start_byte((unsigned char)name[0])) {
        return 0;
    }
    /* Reject C1 controls (U+0080..U+009F) and BOM (U+FEFF) inside names. */
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x80u) {
            if (i > 0 && !is_name_byte(c)) {
                return 0;
            }
            ++i;
        } else if (c >= 0xc2u && c <= 0xdfu) {
            uint32_t cp = ((uint32_t)(c & 0x1fu) << 6) | ((unsigned char)name[i + 1] & 0x3fu);
            if (cp <= 0xA0u) {
                return 0;
            }
            i += 2;
        } else if (c >= 0xe0u && c <= 0xefu) {
            uint32_t cp = ((uint32_t)(c & 0x0fu) << 12) |
                          ((uint32_t)((unsigned char)name[i + 1] & 0x3fu) << 6) |
                          ((unsigned char)name[i + 2] & 0x3fu);
            if (cp == 0xFEFFu) {
                return 0;
            }
            i += 3;
        } else {
            i += 4;
        }
    }
    return 1;
}

static int is_whitespace_only(const char *s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (!is_xml_ws((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

/* Raw table access helpers: never invoke user __index / __newindex metamethods. */
static int rawget_field(lua_State *L, int tbl_idx, const char *key) {
    int abs_idx = lua_absindex(L, tbl_idx);
    lua_pushstring(L, key);
    return lua_rawget(L, abs_idx);
}

static void rawset_field(lua_State *L, int tbl_idx, const char *key) {
    int abs_idx = lua_absindex(L, tbl_idx);
    lua_pushstring(L, key);
    lua_insert(L, -2);
    lua_rawset(L, abs_idx);
}

/* ==================== GC-Managed Scratch Storage ==================== */

struct xml_buf {
    char *data;
    size_t len;
    size_t cap;
    size_t max_len;
};

struct xml_key_entry {
    const char *ptr;
    size_t len;
    lua_Integer slot;
};

struct xml_storage {
    struct xml_buf buf;
    struct xml_key_entry *keys;
    size_t keys_cap;
};

static int storage_gc(lua_State *L) {
    struct xml_storage *st = (struct xml_storage *)lua_touserdata(L, 1);
    if (st != NULL) {
        free(st->buf.data);
        st->buf.data = NULL;
        st->buf.len = 0;
        st->buf.cap = 0;
        free(st->keys);
        st->keys = NULL;
        st->keys_cap = 0;
    }
    return 0;
}

static struct xml_storage *new_storage(lua_State *L, size_t max_len) {
    struct xml_storage *st = (struct xml_storage *)lua_newuserdatauv(L, sizeof(*st), 0);
    memset(st, 0, sizeof(*st));
    st->buf.max_len = max_len;
    luaL_setmetatable(L, XML_STORAGE_META);
    return st;
}

static int buf_reserve(struct xml_buf *b, size_t add) {
    if (add > b->max_len || b->len > b->max_len - add) {
        return 0;
    }
    size_t needed = b->len + add + 1;
    if (needed <= b->cap) {
        return 1;
    }
    size_t next_cap = b->cap == 0 ? 128 : b->cap * 2;
    while (next_cap < needed) {
        if (next_cap > b->max_len / 2) {
            next_cap = b->max_len + 1;
            break;
        }
        next_cap *= 2;
    }
    if (next_cap > b->max_len + 1) {
        next_cap = b->max_len + 1;
    }
    char *grown = (char *)realloc(b->data, next_cap);
    if (grown == NULL) {
        return 0;
    }
    b->data = grown;
    b->cap = next_cap;
    return 1;
}

static int buf_append(struct xml_buf *b, const char *src, size_t n) {
    if (n == 0) {
        return 1;
    }
    if (!buf_reserve(b, n)) {
        return 0;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int buf_append_char(struct xml_buf *b, char c) {
    return buf_append(b, &c, 1);
}

static int buf_append_utf8(struct xml_buf *b, uint32_t cp) {
    char tmp[4];
    size_t n = 0;
    if (cp < 0x80u) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800u) {
        tmp[0] = (char)(0xC0u | (cp >> 6));
        tmp[1] = (char)(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp < 0x10000u) {
        tmp[0] = (char)(0xE0u | (cp >> 12));
        tmp[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        tmp[2] = (char)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else {
        tmp[0] = (char)(0xF0u | (cp >> 18));
        tmp[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        tmp[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        tmp[3] = (char)(0x80u | (cp & 0x3Fu));
        n = 4;
    }
    return buf_append(b, tmp, n);
}

/* Append raw CDATA while normalizing CRLF and standalone CR to LF (XML 1.0 §2.11). */
static int append_normalized_eol(const char *src, size_t len, struct xml_buf *out) {
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && src[i] != '\r') {
            ++i;
        }
        if (i > start && !buf_append(out, src + start, i - start)) {
            return 0;
        }
        if (i < len && src[i] == '\r') {
            if (!buf_append_char(out, '\n')) {
                return 0;
            }
            if (i + 1 < len && src[i + 1] == '\n') {
                i += 2;
            } else {
                i += 1;
            }
        }
    }
    return 1;
}

static int decode_entities(const char *src, size_t len, int is_attr, struct xml_buf *out,
                           const char **err) {
    size_t i = 0;
    while (i < len) {
        char c = src[i];
        if (c == '\r') {
            char norm = is_attr ? ' ' : '\n';
            if (!buf_append_char(out, norm)) {
                *err = "XML text exceeds size limit or out of memory";
                return 0;
            }
            if (i + 1 < len && src[i + 1] == '\n') {
                i += 2;
            } else {
                i += 1;
            }
            continue;
        }
        if (is_attr && (c == '\n' || c == '\t')) {
            /* XML 1.0 §3.3.3: literal whitespace characters in attribute values become space. */
            if (!buf_append_char(out, ' ')) {
                *err = "XML text exceeds size limit or out of memory";
                return 0;
            }
            ++i;
            continue;
        }
        if (c != '&') {
            size_t start = i;
            while (i < len && src[i] != '&' && src[i] != '\r' &&
                   (!is_attr || (src[i] != '\n' && src[i] != '\t'))) {
                ++i;
            }
            if (!buf_append(out, src + start, i - start)) {
                *err = "XML text exceeds size limit or out of memory";
                return 0;
            }
            continue;
        }
        size_t semi = i + 1;
        while (semi < len && src[semi] != ';' && semi - i <= 64) {
            ++semi;
        }
        if (semi >= len || src[semi] != ';') {
            *err = "unterminated XML entity reference";
            return 0;
        }
        const char *ent = src + i + 1;
        size_t ent_len = semi - (i + 1);
        if (ent_len == 0) {
            *err = "empty XML entity reference";
            return 0;
        }
        if (ent[0] == '#') {
            uint32_t cp = 0;
            size_t p = 1;
            int hex = 0;
            /* XML 1.0 §4.1 [66] permits only lowercase 'x' for hex character references. */
            if (p < ent_len && ent[p] == 'x') {
                hex = 1;
                ++p;
            }
            if (p >= ent_len) {
                *err = "invalid numeric XML character reference";
                return 0;
            }
            for (; p < ent_len; ++p) {
                unsigned char ch = (unsigned char)ent[p];
                uint32_t digit;
                if (ch >= '0' && ch <= '9') {
                    digit = (uint32_t)(ch - '0');
                } else if (hex && ch >= 'a' && ch <= 'f') {
                    digit = (uint32_t)(10 + ch - 'a');
                } else if (hex && ch >= 'A' && ch <= 'F') {
                    digit = (uint32_t)(10 + ch - 'A');
                } else {
                    *err = "invalid numeric XML character reference";
                    return 0;
                }
                if (cp > 0x10FFFFu) {
                    *err = "XML character reference out of Unicode range";
                    return 0;
                }
                cp = cp * (hex ? 16u : 10u) + digit;
            }
            if (!is_valid_xml_codepoint(cp)) {
                *err = "XML character reference is not a valid XML 1.0 character";
                return 0;
            }
            if (!buf_append_utf8(out, cp)) {
                *err = "XML text exceeds size limit or out of memory";
                return 0;
            }
        } else if (ent_len == 2 && memcmp(ent, "lt", 2) == 0) {
            if (!buf_append_char(out, '<')) {
                *err = "out of memory";
                return 0;
            }
        } else if (ent_len == 2 && memcmp(ent, "gt", 2) == 0) {
            if (!buf_append_char(out, '>')) {
                *err = "out of memory";
                return 0;
            }
        } else if (ent_len == 3 && memcmp(ent, "amp", 3) == 0) {
            if (!buf_append_char(out, '&')) {
                *err = "out of memory";
                return 0;
            }
        } else if (ent_len == 4 && memcmp(ent, "quot", 4) == 0) {
            if (!buf_append_char(out, '"')) {
                *err = "out of memory";
                return 0;
            }
        } else if (ent_len == 4 && memcmp(ent, "apos", 4) == 0) {
            if (!buf_append_char(out, '\'')) {
                *err = "out of memory";
                return 0;
            }
        } else {
            *err = "unknown XML entity reference";
            return 0;
        }
        i = semi + 1;
    }
    return 1;
}

static int escape_xml_into(const char *src, size_t len, int is_attr, struct xml_buf *out) {
    for (size_t i = 0; i < len; ++i) {
        char c = src[i];
        int ok = 1;
        if (c == '&') {
            ok = buf_append(out, "&amp;", 5);
        } else if (c == '<') {
            ok = buf_append(out, "&lt;", 4);
        } else if (c == '>') {
            ok = buf_append(out, "&gt;", 4);
        } else if (c == '\r') {
            ok = buf_append(out, "&#xD;", 5);
        } else if (c == '"' && is_attr) {
            ok = buf_append(out, "&quot;", 6);
        } else if (c == '\'' && is_attr) {
            ok = buf_append(out, "&apos;", 6);
        } else if (c == '\n' && is_attr) {
            ok = buf_append(out, "&#xA;", 5);
        } else if (c == '\t' && is_attr) {
            ok = buf_append(out, "&#x9;", 5);
        } else {
            ok = buf_append_char(out, c);
        }
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

/* ==================== XML Parser ==================== */

struct xml_parser {
    const char *src;
    size_t len;
    size_t pos;
    size_t nodes;
    int preserve_ws;
    struct xml_buf *scratch; /* GC-managed via xml_storage userdata */
    char error[256];
};

static void parser_error(struct xml_parser *p, const char *msg) {
    if (p->error[0] == '\0') {
        snprintf(p->error, sizeof(p->error), "XML parse error at byte %zu: %s", p->pos, msg);
    }
}

static void skip_ws(struct xml_parser *p) {
    while (p->pos < p->len && is_xml_ws((unsigned char)p->src[p->pos])) {
        ++p->pos;
    }
}

static int starts_with(const struct xml_parser *p, const char *lit, size_t lit_len) {
    return p->len - p->pos >= lit_len && memcmp(p->src + p->pos, lit, lit_len) == 0;
}

static int read_name(struct xml_parser *p, const char **out_ptr, size_t *out_len) {
    size_t start = p->pos;
    if (p->pos >= p->len || !is_name_start_byte((unsigned char)p->src[p->pos])) {
        parser_error(p, "expected XML name");
        return 0;
    }
    ++p->pos;
    while (p->pos < p->len && is_name_byte((unsigned char)p->src[p->pos])) {
        ++p->pos;
    }
    size_t nlen = p->pos - start;
    if (!valid_xml_name(p->src + start, nlen)) {
        parser_error(p, "invalid XML name");
        return 0;
    }
    *out_ptr = p->src + start;
    *out_len = nlen;
    return 1;
}

static int skip_comment(struct xml_parser *p) {
    p->pos += 4; /* <!-- */
    while (p->pos + 2 < p->len) {
        if (p->src[p->pos] == '-' && p->src[p->pos + 1] == '-') {
            if (p->src[p->pos + 2] == '>') {
                p->pos += 3;
                return 1;
            }
            parser_error(p, "'--' is not allowed inside XML comments");
            return 0;
        }
        ++p->pos;
    }
    parser_error(p, "unterminated XML comment");
    return 0;
}

static int is_xml_ci(const char *s, size_t len) {
    return len == 3 && tolower((unsigned char)s[0]) == 'x' && tolower((unsigned char)s[1]) == 'm' &&
           tolower((unsigned char)s[2]) == 'l';
}

static int skip_pi(struct xml_parser *p, int allow_xml_decl) {
    p->pos += 2; /* <? */
    const char *target = NULL;
    size_t target_len = 0;
    if (!read_name(p, &target, &target_len)) {
        return 0;
    }
    if (is_xml_ci(target, target_len)) {
        if (!allow_xml_decl || memcmp(target, "xml", 3) != 0) {
            parser_error(p, "misplaced or invalid XML declaration");
            return 0;
        }
    }
    if (!starts_with(p, "?>", 2)) {
        if (p->pos >= p->len || !is_xml_ws((unsigned char)p->src[p->pos])) {
            parser_error(p, "expected whitespace after processing instruction target");
            return 0;
        }
    }
    while (p->pos + 1 < p->len) {
        if (p->src[p->pos] == '?' && p->src[p->pos + 1] == '>') {
            p->pos += 2;
            return 1;
        }
        ++p->pos;
    }
    parser_error(p, "unterminated processing instruction");
    return 0;
}

/* Safely skip DOCTYPE without expanding any entities (prevents XXE / billion laughs). */
static int skip_doctype(struct xml_parser *p) {
    p->pos += 9; /* <!DOCTYPE */
    if (p->pos >= p->len || !is_xml_ws((unsigned char)p->src[p->pos])) {
        parser_error(p, "expected whitespace after <!DOCTYPE");
        return 0;
    }
    int in_subset = 0;
    int decl_depth = 0;
    while (p->pos < p->len) {
        if (in_subset && decl_depth == 0) {
            if (starts_with(p, "<!--", 4)) {
                if (!skip_comment(p)) {
                    return 0;
                }
                continue;
            }
            if (starts_with(p, "<?", 2)) {
                if (!skip_pi(p, 0)) {
                    return 0;
                }
                continue;
            }
        }
        char c = p->src[p->pos++];
        if (c == '"' || c == '\'') {
            while (p->pos < p->len && p->src[p->pos] != c) {
                ++p->pos;
            }
            if (p->pos >= p->len) {
                parser_error(p, "unterminated string in DOCTYPE");
                return 0;
            }
            ++p->pos;
        } else if (c == '[') {
            if (in_subset || decl_depth != 0) {
                parser_error(p, "unexpected '[' in DOCTYPE");
                return 0;
            }
            in_subset = 1;
        } else if (c == ']') {
            if (!in_subset || decl_depth != 0) {
                parser_error(p, "unexpected ']' in DOCTYPE");
                return 0;
            }
            in_subset = 0;
        } else if (c == '<') {
            if (!in_subset) {
                parser_error(p, "unexpected '<' in DOCTYPE");
                return 0;
            }
            ++decl_depth;
        } else if (c == '>') {
            if (in_subset) {
                if (decl_depth <= 0) {
                    parser_error(p, "unexpected '>' in DOCTYPE internal subset");
                    return 0;
                }
                --decl_depth;
            } else {
                return 1;
            }
        }
    }
    parser_error(p, "unterminated DOCTYPE declaration");
    return 0;
}

static int parse_element(lua_State *L, struct xml_parser *p, unsigned depth) {
    if (depth >= XML_MAX_DEPTH) {
        parser_error(p, "XML nesting depth limit exceeded");
        return 0;
    }
    if (p->nodes >= XML_MAX_NODES) {
        parser_error(p, "XML node count limit exceeded");
        return 0;
    }
    ++p->nodes;

    if (!lua_checkstack(L, 12)) {
        parser_error(p, "Lua stack overflow while parsing XML");
        return 0;
    }

    /* Consume '<' */
    ++p->pos;
    const char *tag_ptr = NULL;
    size_t tag_len = 0;
    if (!read_name(p, &tag_ptr, &tag_len)) {
        return 0;
    }

    /* Populate plain table first, attach XML_NODE_META at the very end so __newindex never fires.
     */
    lua_createtable(L, 0, 3);
    int node_idx = lua_gettop(L);
    lua_pushlstring(L, tag_ptr, tag_len);
    rawset_field(L, node_idx, "tag");

    lua_newtable(L); /* attrs */
    int attrs_idx = lua_gettop(L);
    size_t attr_count = 0;

    int self_closing = 0;
    for (;;) {
        size_t before_ws = p->pos;
        skip_ws(p);
        if (p->pos >= p->len) {
            parser_error(p, "unterminated opening tag");
            lua_pop(L, 2);
            return 0;
        }
        if (p->src[p->pos] == '>') {
            ++p->pos;
            break;
        }
        if (starts_with(p, "/>", 2)) {
            p->pos += 2;
            self_closing = 1;
            break;
        }
        if (p->pos == before_ws) {
            parser_error(p, "expected whitespace before XML attribute");
            lua_pop(L, 2);
            return 0;
        }
        if (attr_count >= XML_MAX_ATTRS) {
            parser_error(p, "XML attribute count limit exceeded");
            lua_pop(L, 2);
            return 0;
        }

        const char *aname = NULL;
        size_t aname_len = 0;
        if (!read_name(p, &aname, &aname_len)) {
            lua_pop(L, 2);
            return 0;
        }
        skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != '=') {
            parser_error(p, "expected '=' after attribute name");
            lua_pop(L, 2);
            return 0;
        }
        ++p->pos;
        skip_ws(p);
        if (p->pos >= p->len || (p->src[p->pos] != '"' && p->src[p->pos] != '\'')) {
            parser_error(p, "expected quoted attribute value");
            lua_pop(L, 2);
            return 0;
        }
        char quote = p->src[p->pos++];
        size_t val_start = p->pos;
        while (p->pos < p->len && p->src[p->pos] != quote) {
            if (p->src[p->pos] == '<') {
                parser_error(p, "'<' is not allowed in attribute value");
                lua_pop(L, 2);
                return 0;
            }
            ++p->pos;
        }
        if (p->pos >= p->len) {
            parser_error(p, "unterminated attribute value");
            lua_pop(L, 2);
            return 0;
        }
        size_t val_len = p->pos - val_start;
        ++p->pos; /* closing quote */

        /* Check for duplicate attribute on the same element */
        lua_pushlstring(L, aname, aname_len);
        lua_pushvalue(L, -1);
        if (lua_rawget(L, attrs_idx) != LUA_TNIL) {
            parser_error(p, "duplicate XML attribute");
            lua_pop(L, 4);
            return 0;
        }
        lua_pop(L, 1);

        p->scratch->len = 0;
        const char *ent_err = NULL;
        if (!decode_entities(p->src + val_start, val_len, 1, p->scratch, &ent_err)) {
            parser_error(p, ent_err);
            lua_pop(L, 3);
            return 0;
        }
        lua_pushlstring(L, p->scratch->data ? p->scratch->data : "", p->scratch->len);
        lua_rawset(L, attrs_idx);
        ++attr_count;
    }
    rawset_field(L, node_idx, "attrs");

    lua_newtable(L); /* children */
    int children_idx = lua_gettop(L);
    lua_Integer child_count = 0;
    int has_child_elements = 0;

    if (!self_closing) {
        /* Always flushed before recursing into child elements, so sharing p->scratch is safe. */
        p->scratch->len = 0;
        int has_cdata_in_acc = 0;

        for (;;) {
            if (p->pos >= p->len) {
                parser_error(p, "unterminated XML element (missing closing tag)");
                lua_pop(L, 2);
                return 0;
            }
            if (p->src[p->pos] != '<') {
                size_t start = p->pos;
                while (p->pos < p->len && p->src[p->pos] != '<') {
                    if (p->src[p->pos] == ']' && starts_with(p, "]]>", 3)) {
                        parser_error(p, "']]>' is not allowed in character data");
                        lua_pop(L, 2);
                        return 0;
                    }
                    ++p->pos;
                }
                const char *ent_err = NULL;
                if (!decode_entities(p->src + start, p->pos - start, 0, p->scratch, &ent_err)) {
                    parser_error(p, ent_err);
                    lua_pop(L, 2);
                    return 0;
                }
                continue;
            }

            if (starts_with(p, "<![CDATA[", 9)) {
                p->pos += 9;
                size_t cdata_start = p->pos;
                while (p->pos + 2 < p->len && !starts_with(p, "]]>", 3)) {
                    ++p->pos;
                }
                if (!starts_with(p, "]]>", 3)) {
                    parser_error(p, "unterminated CDATA section");
                    lua_pop(L, 2);
                    return 0;
                }
                size_t cdata_len = p->pos - cdata_start;
                if (!append_normalized_eol(p->src + cdata_start, cdata_len, p->scratch)) {
                    parser_error(p, "CDATA exceeds size limit or out of memory");
                    lua_pop(L, 2);
                    return 0;
                }
                has_cdata_in_acc = 1;
                p->pos += 3;
                continue;
            }

            if (starts_with(p, "<!--", 4)) {
                if (!skip_comment(p)) {
                    lua_pop(L, 2);
                    return 0;
                }
                continue;
            }

            if (starts_with(p, "<?", 2)) {
                if (!skip_pi(p, 0)) {
                    lua_pop(L, 2);
                    return 0;
                }
                continue;
            }

            if (starts_with(p, "</", 2)) {
                if (p->scratch->len > 0 || has_cdata_in_acc) {
                    int drop_ws = !p->preserve_ws && has_child_elements && !has_cdata_in_acc &&
                                  is_whitespace_only(p->scratch->data, p->scratch->len);
                    if (!drop_ws && (p->scratch->len > 0 || has_cdata_in_acc)) {
                        lua_pushlstring(L, p->scratch->data ? p->scratch->data : "",
                                        p->scratch->len);
                        lua_rawseti(L, children_idx, ++child_count);
                    }
                    p->scratch->len = 0;
                    has_cdata_in_acc = 0;
                }
                p->pos += 2;
                const char *close_ptr = NULL;
                size_t close_len = 0;
                if (!read_name(p, &close_ptr, &close_len)) {
                    lua_pop(L, 2);
                    return 0;
                }
                skip_ws(p);
                if (p->pos >= p->len || p->src[p->pos] != '>') {
                    parser_error(p, "expected '>' in closing tag");
                    lua_pop(L, 2);
                    return 0;
                }
                ++p->pos;
                if (close_len != tag_len || memcmp(close_ptr, tag_ptr, tag_len) != 0) {
                    parser_error(p, "mismatched XML closing tag");
                    lua_pop(L, 2);
                    return 0;
                }
                break;
            }

            /* Child element starts here: flush any preceding text before reusing p->scratch. */
            if (p->scratch->len > 0 || has_cdata_in_acc) {
                int drop_ws = !p->preserve_ws && !has_cdata_in_acc &&
                              is_whitespace_only(p->scratch->data, p->scratch->len);
                if (!drop_ws && (p->scratch->len > 0 || has_cdata_in_acc)) {
                    lua_pushlstring(L, p->scratch->data ? p->scratch->data : "", p->scratch->len);
                    lua_rawseti(L, children_idx, ++child_count);
                }
                p->scratch->len = 0;
                has_cdata_in_acc = 0;
            }

            if (!parse_element(L, p, depth + 1)) {
                lua_pop(L, 2);
                return 0;
            }
            has_child_elements = 1;
            lua_rawseti(L, children_idx, ++child_count);
            p->scratch->len = 0;
            has_cdata_in_acc = 0;
        }
    }

    rawset_field(L, node_idx, "children");
    luaL_setmetatable(L, XML_NODE_META);
    return 1;
}

static int lua_xml_parse(lua_State *L) {
    size_t len = 0;
    const char *src = luaL_checklstring(L, 1, &len);
    if (len > XML_MAX_INPUT) {
        return luaL_error(L, "XML input exceeds %d bytes", XML_MAX_INPUT);
    }
    if (!valid_xml_utf8(src, len)) {
        return luaL_error(L, "XML input is not valid UTF-8 or contains illegal XML 1.0 characters");
    }

    int preserve_ws = 0;
    if (lua_istable(L, 2)) {
        rawget_field(L, 2, "preserve_whitespace");
        preserve_ws = lua_toboolean(L, -1);
        lua_pop(L, 1);
    } else if (!lua_isnoneornil(L, 2)) {
        return luaL_argerror(L, 2, "expected options table or nil");
    }

    struct xml_storage *storage = new_storage(L, XML_MAX_INPUT);

    struct xml_parser p;
    memset(&p, 0, sizeof(p));
    p.src = src;
    p.len = len;
    p.preserve_ws = preserve_ws;
    p.scratch = &storage->buf;

    /* Strip optional UTF-8 BOM */
    size_t start_pos = 0;
    if (len >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF) {
        start_pos = 3;
        p.pos = 3;
    }

    /* Prolog: optional XML declaration ONLY at start_pos, then whitespace/comments/PIs/DOCTYPE */
    int seen_doctype = 0;
    if (starts_with(&p, "<?xml", 5) &&
        (p.pos + 5 == p.len || is_xml_ws((unsigned char)p.src[p.pos + 5]) ||
         starts_with(&p, "<?xml?>", 7))) {
        if (!skip_pi(&p, 1)) {
            return luaL_error(L, "%s", p.error);
        }
    }
    for (;;) {
        skip_ws(&p);
        if (starts_with(&p, "<!--", 4)) {
            if (!skip_comment(&p)) {
                return luaL_error(L, "%s", p.error);
            }
        } else if (starts_with(&p, "<?", 2)) {
            if (!skip_pi(&p, 0)) {
                return luaL_error(L, "%s", p.error);
            }
        } else if (starts_with(&p, "<!DOCTYPE", 9)) {
            if (seen_doctype) {
                return luaL_error(L, "XML parse error: duplicate DOCTYPE declaration");
            }
            seen_doctype = 1;
            if (!skip_doctype(&p)) {
                return luaL_error(L, "%s", p.error);
            }
        } else {
            break;
        }
    }
    (void)start_pos;

    if (p.pos >= p.len || p.src[p.pos] != '<' || starts_with(&p, "</", 2) ||
        starts_with(&p, "<!", 2)) {
        return luaL_error(L, "XML parse error at byte %zu: expected root element", p.pos);
    }

    if (!parse_element(L, &p, 0)) {
        return luaL_error(L, "%s", p.error);
    }

    /* Epilog: only whitespace, comments, and non-xml PIs allowed after root element */
    for (;;) {
        skip_ws(&p);
        if (p.pos >= p.len) {
            break;
        }
        if (starts_with(&p, "<!--", 4)) {
            if (!skip_comment(&p)) {
                return luaL_error(L, "%s", p.error);
            }
        } else if (starts_with(&p, "<?", 2)) {
            if (!skip_pi(&p, 0)) {
                return luaL_error(L, "%s", p.error);
            }
        } else {
            return luaL_error(L, "XML parse error at byte %zu: unexpected content after root",
                              p.pos);
        }
    }

    return 1;
}

/* ==================== XML Encoder ==================== */

struct encode_ctx {
    const void *ancestors[XML_MAX_DEPTH];
    size_t nodes;
    char indent[17];
    size_t indent_len;
    int has_indent;
    struct xml_storage *storage; /* GC-managed via userdata on stack */
    char error[256];
};

static int cmp_key_entry(const void *a, const void *b) {
    const struct xml_key_entry *ka = (const struct xml_key_entry *)a;
    const struct xml_key_entry *kb = (const struct xml_key_entry *)b;
    size_t min_len = ka->len < kb->len ? ka->len : kb->len;
    int cmp = memcmp(ka->ptr, kb->ptr, min_len);
    if (cmp != 0) {
        return cmp;
    }
    return ka->len < kb->len ? -1 : (ka->len > kb->len ? 1 : 0);
}

static int append_indent(struct encode_ctx *ctx, unsigned depth) {
    for (unsigned i = 0; i < depth; ++i) {
        if (!buf_append(&ctx->storage->buf, ctx->indent, ctx->indent_len)) {
            return 0;
        }
    }
    return 1;
}

static int format_scalar_attr(lua_State *L, int idx, char *buf, size_t buf_size,
                              const char **out_ptr, size_t *out_len) {
    int t = lua_type(L, idx);
    if (t == LUA_TSTRING) {
        *out_ptr = lua_tolstring(L, idx, out_len);
        return 1;
    }
    if (t == LUA_TBOOLEAN) {
        if (lua_toboolean(L, idx)) {
            *out_ptr = "true";
            *out_len = 4;
        } else {
            *out_ptr = "false";
            *out_len = 5;
        }
        return 1;
    }
    if (t == LUA_TNUMBER) {
        if (lua_isinteger(L, idx)) {
            int n = snprintf(buf, buf_size, "%lld", (long long)lua_tointeger(L, idx));
            if (n < 0 || (size_t)n >= buf_size) {
                return 0;
            }
            *out_ptr = buf;
            *out_len = (size_t)n;
            return 1;
        }
        double d = (double)lua_tonumber(L, idx);
        if (!isfinite(d)) {
            return 0;
        }
        int n = snprintf(buf, buf_size, "%.17g", d);
        if (n < 0 || (size_t)n >= buf_size) {
            return 0;
        }
        for (int i = 0; i < n; ++i) {
            if (buf[i] == ',') {
                buf[i] = '.';
            }
        }
        *out_ptr = buf;
        *out_len = (size_t)n;
        return 1;
    }
    return 0;
}

static int encode_node(lua_State *L, int idx, unsigned depth, int emit_indent,
                       struct encode_ctx *ctx) {
    if (depth >= XML_MAX_DEPTH) {
        snprintf(ctx->error, sizeof(ctx->error), "XML encode depth limit exceeded");
        return 0;
    }
    if (ctx->nodes >= XML_MAX_NODES) {
        snprintf(ctx->error, sizeof(ctx->error), "XML encode node limit exceeded");
        return 0;
    }
    if (!lua_istable(L, idx)) {
        snprintf(ctx->error, sizeof(ctx->error), "XML node must be a table");
        return 0;
    }
    if (!lua_checkstack(L, 12)) {
        snprintf(ctx->error, sizeof(ctx->error), "Lua stack overflow while encoding XML");
        return 0;
    }

    const void *ptr = lua_topointer(L, idx);
    for (unsigned i = 0; i < depth; ++i) {
        if (ctx->ancestors[i] == ptr) {
            snprintf(ctx->error, sizeof(ctx->error), "cyclic XML node reference");
            return 0;
        }
    }
    ctx->ancestors[depth] = ptr;
    ++ctx->nodes;

    int abs_idx = lua_absindex(L, idx);
    rawget_field(L, abs_idx, "tag");
    if (lua_type(L, -1) != LUA_TSTRING) {
        lua_pop(L, 1);
        snprintf(ctx->error, sizeof(ctx->error), "XML node missing string 'tag'");
        return 0;
    }
    size_t tag_len = 0;
    const char *tag = lua_tolstring(L, -1, &tag_len);
    if (!valid_xml_name(tag, tag_len)) {
        lua_pop(L, 1);
        snprintf(ctx->error, sizeof(ctx->error), "invalid XML tag name");
        return 0;
    }

    if (emit_indent && depth > 0) {
        if (!append_indent(ctx, depth)) {
            lua_pop(L, 1);
            snprintf(ctx->error, sizeof(ctx->error), "XML output exceeds size limit");
            return 0;
        }
    }
    if (!buf_append_char(&ctx->storage->buf, '<') ||
        !buf_append(&ctx->storage->buf, tag, tag_len)) {
        lua_pop(L, 1);
        snprintf(ctx->error, sizeof(ctx->error), "XML output exceeds size limit");
        return 0;
    }

    /* Serialize attributes in sorted key order; root all key TStrings in a Lua table on stack. */
    rawget_field(L, abs_idx, "attrs");
    if (lua_istable(L, -1)) {
        int attrs_idx = lua_gettop(L);
        lua_newtable(L); /* keys_tbl: roots every key string across the entire loop */
        int keys_tbl_idx = lua_gettop(L);
        size_t key_count = 0;

        lua_pushnil(L);
        while (lua_next(L, attrs_idx) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) {
                lua_pop(L, 5);
                snprintf(ctx->error, sizeof(ctx->error), "XML attribute name must be a string");
                return 0;
            }
            size_t klen = 0;
            const char *kstr = lua_tolstring(L, -2, &klen);
            if (!valid_xml_name(kstr, klen)) {
                lua_pop(L, 5);
                snprintf(ctx->error, sizeof(ctx->error), "invalid XML attribute name");
                return 0;
            }
            if (key_count >= XML_MAX_ATTRS) {
                lua_pop(L, 5);
                snprintf(ctx->error, sizeof(ctx->error), "XML attribute count limit exceeded");
                return 0;
            }
            if (key_count >= ctx->storage->keys_cap) {
                size_t next = ctx->storage->keys_cap == 0 ? 8 : ctx->storage->keys_cap * 2;
                if (next > XML_MAX_ATTRS) {
                    next = XML_MAX_ATTRS;
                }
                struct xml_key_entry *grown =
                    (struct xml_key_entry *)realloc(ctx->storage->keys, next * sizeof(*grown));
                if (grown == NULL) {
                    lua_pop(L, 5);
                    snprintf(ctx->error, sizeof(ctx->error), "out of memory");
                    return 0;
                }
                ctx->storage->keys = grown;
                ctx->storage->keys_cap = next;
            }
            lua_Integer slot = (lua_Integer)(key_count + 1);
            lua_pushvalue(L, -2); /* copy key string */
            lua_rawseti(L, keys_tbl_idx, slot);
            ctx->storage->keys[key_count].ptr = kstr;
            ctx->storage->keys[key_count].len = klen;
            ctx->storage->keys[key_count].slot = slot;
            ++key_count;
            lua_pop(L, 1); /* pop value, keep key for lua_next */
        }

        if (key_count > 1) {
            qsort(ctx->storage->keys, key_count, sizeof(*ctx->storage->keys), cmp_key_entry);
        }
        for (size_t i = 0; i < key_count; ++i) {
            const struct xml_key_entry *ke = &ctx->storage->keys[i];
            lua_rawgeti(L, keys_tbl_idx, ke->slot);
            lua_rawget(L, attrs_idx);
            char num_buf[64];
            const char *vstr = NULL;
            size_t vlen = 0;
            if (!format_scalar_attr(L, -1, num_buf, sizeof(num_buf), &vstr, &vlen)) {
                lua_pop(L, 4);
                snprintf(ctx->error, sizeof(ctx->error),
                         "XML attribute value must be string, finite number, or boolean");
                return 0;
            }
            if (!valid_xml_utf8(vstr, vlen)) {
                lua_pop(L, 4);
                snprintf(
                    ctx->error, sizeof(ctx->error),
                    "XML attribute value contains invalid UTF-8 or illegal XML 1.0 characters");
                return 0;
            }
            if (!buf_append_char(&ctx->storage->buf, ' ') ||
                !buf_append(&ctx->storage->buf, ke->ptr, ke->len) ||
                !buf_append(&ctx->storage->buf, "=\"", 2) ||
                !escape_xml_into(vstr, vlen, 1, &ctx->storage->buf) ||
                !buf_append_char(&ctx->storage->buf, '"')) {
                lua_pop(L, 4);
                snprintf(ctx->error, sizeof(ctx->error), "XML output exceeds size limit");
                return 0;
            }
            lua_pop(L, 1); /* pop attr value */
        }
        lua_pop(L, 1); /* pop keys_tbl */
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 2);
        snprintf(ctx->error, sizeof(ctx->error), "XML node 'attrs' must be a table or nil");
        return 0;
    }
    lua_pop(L, 1); /* pop attrs */

    /* Inspect children: only pretty-print when element has child elements and NO inline text. */
    rawget_field(L, abs_idx, "children");
    lua_Integer child_count = 0;
    int has_elem_child = 0;
    int has_text_child = 0;
    if (lua_istable(L, -1)) {
        child_count = (lua_Integer)lua_rawlen(L, -1);
        for (lua_Integer i = 1; i <= child_count; ++i) {
            int ct = lua_rawgeti(L, -1, i);
            if (ct == LUA_TTABLE) {
                has_elem_child = 1;
            } else if (ct == LUA_TSTRING) {
                has_text_child = 1;
            }
            lua_pop(L, 1);
            if (has_elem_child && has_text_child) {
                break;
            }
        }
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 2);
        snprintf(ctx->error, sizeof(ctx->error), "XML node 'children' must be a table or nil");
        return 0;
    }

    if (child_count == 0) {
        lua_pop(L, 2); /* children, tag */
        if (!buf_append(&ctx->storage->buf, "/>", 2)) {
            snprintf(ctx->error, sizeof(ctx->error), "XML output exceeds size limit");
            return 0;
        }
        return 1;
    }

    if (!buf_append_char(&ctx->storage->buf, '>')) {
        lua_pop(L, 2);
        snprintf(ctx->error, sizeof(ctx->error), "XML output exceeds size limit");
        return 0;
    }

    int children_idx = lua_gettop(L);
    int pretty_children = ctx->has_indent && has_elem_child && !has_text_child;
    if (pretty_children && !buf_append_char(&ctx->storage->buf, '\n')) {
        lua_pop(L, 2);
        snprintf(ctx->error, sizeof(ctx->error), "XML output exceeds size limit");
        return 0;
    }

    for (lua_Integer i = 1; i <= child_count; ++i) {
        int ctype = lua_rawgeti(L, children_idx, i);
        if (ctype == LUA_TSTRING) {
            size_t tlen = 0;
            const char *tstr = lua_tolstring(L, -1, &tlen);
            if (!valid_xml_utf8(tstr, tlen)) {
                lua_pop(L, 3);
                snprintf(ctx->error, sizeof(ctx->error),
                         "XML text child contains invalid UTF-8 or illegal XML 1.0 characters");
                return 0;
            }
            if (!escape_xml_into(tstr, tlen, 0, &ctx->storage->buf)) {
                lua_pop(L, 3);
                snprintf(ctx->error, sizeof(ctx->error), "XML output exceeds size limit");
                return 0;
            }
        } else if (ctype == LUA_TTABLE) {
            if (!encode_node(L, -1, depth + 1, pretty_children, ctx)) {
                lua_pop(L, 3);
                return 0;
            }
            if (pretty_children && !buf_append_char(&ctx->storage->buf, '\n')) {
                lua_pop(L, 3);
                snprintf(ctx->error, sizeof(ctx->error), "XML output exceeds size limit");
                return 0;
            }
        } else {
            lua_pop(L, 3);
            snprintf(ctx->error, sizeof(ctx->error), "XML child must be a node table or string");
            return 0;
        }
        lua_pop(L, 1);
    }

    if (pretty_children && !append_indent(ctx, depth)) {
        lua_pop(L, 2);
        snprintf(ctx->error, sizeof(ctx->error), "XML output exceeds size limit");
        return 0;
    }
    if (!buf_append(&ctx->storage->buf, "</", 2) || !buf_append(&ctx->storage->buf, tag, tag_len) ||
        !buf_append_char(&ctx->storage->buf, '>')) {
        lua_pop(L, 2);
        snprintf(ctx->error, sizeof(ctx->error), "XML output exceeds size limit");
        return 0;
    }

    lua_pop(L, 2); /* children, tag */
    return 1;
}

static int lua_xml_encode(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    int declaration = 0;
    struct encode_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (lua_istable(L, 2)) {
        rawget_field(L, 2, "declaration");
        declaration = lua_toboolean(L, -1);
        lua_pop(L, 1);

        rawget_field(L, 2, "indent");
        if (lua_type(L, -1) == LUA_TBOOLEAN) {
            if (lua_toboolean(L, -1)) {
                memcpy(ctx.indent, "  ", 3);
                ctx.indent_len = 2;
                ctx.has_indent = 1;
            }
        } else if (lua_type(L, -1) == LUA_TSTRING) {
            size_t ilen = 0;
            const char *istr = lua_tolstring(L, -1, &ilen);
            if (ilen > 16 || !is_whitespace_only(istr, ilen)) {
                lua_pop(L, 1);
                return luaL_error(L, "XML indent option must be <= 16 whitespace characters");
            }
            memcpy(ctx.indent, istr, ilen);
            ctx.indent[ilen] = '\0';
            ctx.indent_len = ilen;
            ctx.has_indent = 1;
        } else if (!lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(L, "XML indent option must be boolean or string");
        }
        lua_pop(L, 1);
    } else if (!lua_isnoneornil(L, 2)) {
        return luaL_argerror(L, 2, "expected options table or nil");
    }

    ctx.storage = new_storage(L, XML_MAX_OUTPUT);

    if (declaration) {
        static const char kDecl[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        if (!buf_append(&ctx.storage->buf, kDecl, sizeof(kDecl) - 1)) {
            return luaL_error(L, "out of memory");
        }
    }

    if (!encode_node(L, 1, 0, 0, &ctx)) {
        return luaL_error(L, "%s", ctx.error[0] ? ctx.error : "XML encode failed");
    }

    lua_pushlstring(L, ctx.storage->buf.data ? ctx.storage->buf.data : "", ctx.storage->buf.len);
    return 1;
}

static int lua_xml_escape(lua_State *L) {
    size_t len = 0;
    const char *src = luaL_checklstring(L, 1, &len);
    if (len > XML_MAX_INPUT) {
        return luaL_error(L, "XML text exceeds size limit");
    }
    if (!valid_xml_utf8(src, len)) {
        return luaL_error(L, "XML text is not valid UTF-8 or contains illegal XML 1.0 characters");
    }
    struct xml_storage *st = new_storage(L, XML_MAX_OUTPUT);
    if (!escape_xml_into(src, len, 1, &st->buf)) {
        return luaL_error(L, "XML escape exceeds output limit");
    }
    lua_pushlstring(L, st->buf.data ? st->buf.data : "", st->buf.len);
    return 1;
}

static int lua_xml_unescape(lua_State *L) {
    size_t len = 0;
    const char *src = luaL_checklstring(L, 1, &len);
    if (len > XML_MAX_INPUT) {
        return luaL_error(L, "XML text exceeds size limit");
    }
    if (!valid_xml_utf8(src, len)) {
        return luaL_error(L, "XML text is not valid UTF-8 or contains illegal XML 1.0 characters");
    }
    struct xml_storage *st = new_storage(L, XML_MAX_OUTPUT);
    const char *err = NULL;
    if (!decode_entities(src, len, 0, &st->buf, &err)) {
        return luaL_error(L, "%s", err ? err : "XML unescape failed");
    }
    lua_pushlstring(L, st->buf.data ? st->buf.data : "", st->buf.len);
    return 1;
}

static int lua_xml_node(lua_State *L) {
    int nargs = lua_gettop(L);
    size_t tag_len = 0;
    const char *tag = luaL_checklstring(L, 1, &tag_len);
    if (!valid_xml_name(tag, tag_len)) {
        return luaL_argerror(L, 1, "invalid XML tag name");
    }

    int has_attrs = (nargs >= 2 && !lua_isnil(L, 2));
    if (has_attrs && !lua_istable(L, 2)) {
        return luaL_argerror(L, 2, "expected attrs table or nil");
    }
    int has_children = (nargs >= 3 && !lua_isnil(L, 3));
    if (has_children && !lua_istable(L, 3) && lua_type(L, 3) != LUA_TSTRING) {
        return luaL_argerror(L, 3, "expected children table, string, or nil");
    }

    lua_createtable(L, 0, 3);
    int node_idx = lua_gettop(L);
    lua_pushlstring(L, tag, tag_len);
    rawset_field(L, node_idx, "tag");

    if (has_attrs) {
        lua_pushvalue(L, 2);
    } else {
        lua_newtable(L);
    }
    rawset_field(L, node_idx, "attrs");

    if (has_children && lua_istable(L, 3)) {
        int is_single_node = 0;
        if (lua_getmetatable(L, 3)) {
            luaL_getmetatable(L, XML_NODE_META);
            is_single_node = lua_rawequal(L, -1, -2);
            lua_pop(L, 2);
        }
        if (is_single_node) {
            lua_createtable(L, 1, 0);
            lua_pushvalue(L, 3);
            lua_rawseti(L, -2, 1);
        } else {
            lua_pushvalue(L, 3);
        }
    } else if (has_children && lua_type(L, 3) == LUA_TSTRING) {
        lua_createtable(L, 1, 0);
        lua_pushvalue(L, 3);
        lua_rawseti(L, -2, 1);
    } else {
        lua_newtable(L);
    }
    rawset_field(L, node_idx, "children");
    luaL_setmetatable(L, XML_NODE_META);
    return 1;
}

static int lua_xml_is_node(lua_State *L) {
    if (!lua_istable(L, 1) || !lua_getmetatable(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    luaL_getmetatable(L, XML_NODE_META);
    int same = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    lua_pushboolean(L, same);
    return 1;
}

/* Built-in node methods (extended further with query/sugar methods in resources.lua) */

struct text_ctx {
    const void *ancestors[XML_MAX_DEPTH];
    size_t nodes;
    struct xml_buf *out;
    char error[256];
};

static int collect_node_text(lua_State *L, int idx, unsigned depth, struct text_ctx *ctx) {
    if (depth >= XML_MAX_DEPTH) {
        snprintf(ctx->error, sizeof(ctx->error), "XML text nesting depth limit exceeded");
        return 0;
    }
    if (ctx->nodes >= XML_MAX_NODES) {
        snprintf(ctx->error, sizeof(ctx->error), "XML text node limit exceeded");
        return 0;
    }
    if (!lua_istable(L, idx) || !lua_checkstack(L, 6)) {
        snprintf(ctx->error, sizeof(ctx->error), "Lua stack overflow while reading XML text");
        return 0;
    }
    const void *ptr = lua_topointer(L, idx);
    for (unsigned i = 0; i < depth; ++i) {
        if (ctx->ancestors[i] == ptr) {
            snprintf(ctx->error, sizeof(ctx->error), "cyclic XML node reference");
            return 0;
        }
    }
    ctx->ancestors[depth] = ptr;
    ++ctx->nodes;

    int abs_idx = lua_absindex(L, idx);
    rawget_field(L, abs_idx, "children");
    if (lua_istable(L, -1)) {
        lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
        for (lua_Integer i = 1; i <= n; ++i) {
            int t = lua_rawgeti(L, -1, i);
            if (t == LUA_TSTRING) {
                size_t slen = 0;
                const char *s = lua_tolstring(L, -1, &slen);
                if (!buf_append(ctx->out, s, slen)) {
                    lua_pop(L, 2);
                    snprintf(ctx->error, sizeof(ctx->error), "XML text exceeds size limit");
                    return 0;
                }
            } else if (t == LUA_TTABLE) {
                if (!collect_node_text(L, -1, depth + 1, ctx)) {
                    lua_pop(L, 2);
                    return 0;
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return 1;
}

static int node_text(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    struct xml_storage *st = new_storage(L, XML_MAX_OUTPUT);
    struct text_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = &st->buf;
    if (!collect_node_text(L, 1, 0, &ctx)) {
        return luaL_error(L, "%s", ctx.error[0] ? ctx.error : "XML text extraction failed");
    }
    lua_pushlstring(L, st->buf.data ? st->buf.data : "", st->buf.len);
    return 1;
}

static int node_attr(lua_State *L) {
    int nargs = lua_gettop(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkstring(L, 2);
    rawget_field(L, 1, "attrs");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
        if (!lua_isnil(L, -1)) {
            return 1;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (nargs >= 3) {
        lua_pushvalue(L, 3);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int node_to_xml(lua_State *L) {
    return lua_xml_encode(L);
}

static int node_tostring(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    rawget_field(L, 1, "tag");
    const char *tag = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "?";

    lua_Integer attr_count = 0;
    rawget_field(L, 1, "attrs");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            ++attr_count;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_Integer child_count = 0;
    rawget_field(L, 1, "children");
    if (lua_istable(L, -1)) {
        child_count = (lua_Integer)lua_rawlen(L, -1);
    }
    lua_pop(L, 1);

    lua_pushfstring(L, "<xml.node <%s> (%I attrs, %I children)>", tag, (lua_Integer)attr_count,
                    (lua_Integer)child_count);
    lua_remove(L, -2);
    return 1;
}

void dejavu_lua_xml_open(lua_State *L) {
    if (luaL_newmetatable(L, XML_STORAGE_META)) {
        lua_pushcfunction(L, storage_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);

    /* Idempotent: reuse existing XML_NODE_META.__index table if already initialized. */
    int methods_idx = 0;
    if (luaL_newmetatable(L, XML_NODE_META)) {
        lua_newtable(L);
        lua_pushcfunction(L, node_text);
        lua_setfield(L, -2, "text");
        lua_pushcfunction(L, node_attr);
        lua_setfield(L, -2, "attr");
        lua_pushcfunction(L, node_to_xml);
        lua_setfield(L, -2, "to_xml");
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "__index");
        lua_pushcfunction(L, node_tostring);
        lua_setfield(L, -3, "__tostring");
        methods_idx = lua_gettop(L);
    } else {
        rawget_field(L, -1, "__index");
        methods_idx = lua_gettop(L);
    }

    lua_newtable(L);
    lua_pushcfunction(L, lua_xml_parse);
    lua_setfield(L, -2, "parse");
    lua_pushcfunction(L, lua_xml_parse);
    lua_setfield(L, -2, "decode");
    lua_pushcfunction(L, lua_xml_encode);
    lua_setfield(L, -2, "encode");
    lua_pushcfunction(L, lua_xml_encode);
    lua_setfield(L, -2, "stringify");
    lua_pushcfunction(L, lua_xml_escape);
    lua_setfield(L, -2, "escape");
    lua_pushcfunction(L, lua_xml_unescape);
    lua_setfield(L, -2, "unescape");
    lua_pushcfunction(L, lua_xml_node);
    lua_setfield(L, -2, "node");
    lua_pushcfunction(L, lua_xml_is_node);
    lua_setfield(L, -2, "is_node");
    lua_pushvalue(L, methods_idx);
    lua_setfield(L, -2, "_node_methods");

    lua_setglobal(L, "xml");
    lua_pop(L, 2); /* pop methods_idx and XML_NODE_META */
}
