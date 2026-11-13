#include "dejavu_lua_json.h"
#include "lauxlib.h"
#include "../router/third_party/cJSON/cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_LIMIT (1024 * 1024)
#define JSON_NODES 65536
#define JSON_DEPTH 64
#define JSON_SAFE_INTEGER 9007199254740991LL
#define ARRAY_META "dejavu.json.array"
#define OBJECT_META "dejavu.json.object"
#define STORAGE_META "dejavu.json.storage"
static char null_token;
struct json_storage {
    cJSON *node;
    char *output;
};
static int storage_gc(lua_State *L) {
    struct json_storage *storage = lua_touserdata(L, 1);
    cJSON_Delete(storage->node);
    free(storage->output);
    storage->node = NULL;
    storage->output = NULL;
    return 0;
}
static struct json_storage *new_storage(lua_State *L) {
    struct json_storage *storage = lua_newuserdatauv(L, sizeof(*storage), 0);
    storage->node = NULL;
    storage->output = NULL;
    luaL_setmetatable(L, STORAGE_META);
    return storage;
}

/* Reject overlong sequences, surrogate codepoints, NUL, and invalid UTF-8. */
static int valid_text(const char *text, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)text[i++];
        unsigned value, minimum;
        int extra;
        if (c == 0)
            return 0;
        if (c < 0x80)
            continue;
        if (c >= 0xc2 && c <= 0xdf) {
            value = c & 31;
            extra = 1;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            value = c & 15;
            extra = 2;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            value = c & 7;
            extra = 3;
            minimum = 0x10000;
        } else
            return 0;
        while (extra--) {
            if (i == length || ((unsigned char)text[i] & 0xc0) != 0x80)
                return 0;
            value = (value << 6) | ((unsigned char)text[i++] & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return 0;
    }
    return 1;
}

struct encode_context {
    const void *ancestors[JSON_DEPTH];
    size_t nodes, bytes;
    const char *error;
};
static cJSON *encode_value(lua_State *L, int index, unsigned depth, struct encode_context *ctx) {
    cJSON *node = NULL;
    int type = lua_type(L, index);
    size_t length;
    const char *text;
    if (++ctx->nodes > JSON_NODES || depth > JSON_DEPTH) {
        ctx->error = "JSON complexity limit";
        return NULL;
    }
    if (!lua_checkstack(L, 8)) {
        ctx->error = "JSON Lua stack limit";
        return NULL;
    }
    index = lua_absindex(L, index);
    switch (type) {
    case LUA_TNIL:
        node = cJSON_CreateNull();
        break;
    case LUA_TBOOLEAN:
        node = cJSON_CreateBool(lua_toboolean(L, index));
        break;
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(L, index) == &null_token)
            node = cJSON_CreateNull();
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(L, index)) {
            lua_Integer integer = lua_tointeger(L, index);
            char decimal[32];
            if (integer < -JSON_SAFE_INTEGER || integer > JSON_SAFE_INTEGER) {
                ctx->error = "JSON integer exceeds safe range; use strings";
                return NULL;
            }
            snprintf(decimal, sizeof(decimal), "%lld", (long long)integer);
            node = cJSON_CreateRaw(decimal);
        } else {
            double number = lua_tonumber(L, index);
            if (!isfinite(number)) {
                ctx->error = "nonfinite JSON number";
                return NULL;
            }
            if (trunc(number) == number) {
                char decimal[32];
                if (fabs(number) > JSON_SAFE_INTEGER) {
                    ctx->error = "JSON integer exceeds safe range; use strings";
                    return NULL;
                }
                snprintf(decimal, sizeof(decimal), "%.0f", number);
                node = cJSON_CreateRaw(decimal);
            } else
                node = cJSON_CreateNumber(number);
        }
        break;
    case LUA_TSTRING:
        text = lua_tolstring(L, index, &length);
        ctx->bytes += length;
        if (ctx->bytes > JSON_LIMIT || !valid_text(text, length)) {
            ctx->error = "invalid or oversized JSON string";
            return NULL;
        }
        node = cJSON_CreateString(text);
        break;
    case LUA_TTABLE: {
        size_t count = 0, maximum = 0;
        int strings = 0, integers = 0, array_mark = 0, object_mark = 0;
        const void *identity = lua_topointer(L, index);
        if (depth == JSON_DEPTH) {
            ctx->error = "JSON depth limit";
            return NULL;
        }
        for (unsigned i = 0; i < depth; ++i)
            if (ctx->ancestors[i] == identity) {
                ctx->error = "JSON cycle";
                return NULL;
            }
        ctx->ancestors[depth] = identity;
        if (lua_getmetatable(L, index)) {
            luaL_getmetatable(L, ARRAY_META);
            array_mark = lua_rawequal(L, -1, -2);
            lua_pop(L, 1);
            luaL_getmetatable(L, OBJECT_META);
            object_mark = lua_rawequal(L, -1, -2);
            lua_pop(L, 2);
        }
        lua_pushnil(L);
        while (lua_next(L, index)) {
            ++count;
            if (lua_type(L, -2) == LUA_TSTRING)
                strings = 1;
            else if (lua_isinteger(L, -2) && lua_tointeger(L, -2) > 0 &&
                     (lua_Unsigned)lua_tointeger(L, -2) <= JSON_NODES) {
                size_t key = (size_t)lua_tointeger(L, -2);
                integers = 1;
                if (key > maximum)
                    maximum = key;
            } else {
                lua_pop(L, 2);
                ctx->error = "invalid JSON table key";
                return NULL;
            }
            lua_pop(L, 1);
            if (count > JSON_NODES) {
                lua_pop(L, 1);
                ctx->error = "JSON node limit";
                return NULL;
            }
        }
        if ((strings && integers) || (integers && maximum != count) || (array_mark && strings) ||
            (object_mark && integers)) {
            ctx->error = "mixed or sparse JSON table";
            return NULL;
        }
        if (array_mark || integers) {
            node = cJSON_CreateArray();
            if (!node)
                break;
            for (size_t i = 1; i <= count; ++i) {
                cJSON *child;
                lua_rawgeti(L, index, (lua_Integer)i);
                child = encode_value(L, -1, depth + 1, ctx);
                lua_pop(L, 1);
                if (!child) {
                    cJSON_Delete(node);
                    return NULL;
                }
                cJSON_AddItemToArray(node, child);
            }
        } else {
            node = cJSON_CreateObject();
            if (!node)
                break;
            lua_pushnil(L);
            while (lua_next(L, index)) {
                cJSON *child;
                text = lua_tolstring(L, -2, &length);
                ctx->bytes += length;
                if (ctx->bytes > JSON_LIMIT || !valid_text(text, length)) {
                    lua_pop(L, 2);
                    cJSON_Delete(node);
                    ctx->error = "invalid JSON object key";
                    return NULL;
                }
                child = encode_value(L, -1, depth + 1, ctx);
                if (!child) {
                    lua_pop(L, 2);
                    cJSON_Delete(node);
                    return NULL;
                }
                if (!cJSON_AddItemToObject(node, text, child)) {
                    lua_pop(L, 2);
                    cJSON_Delete(child);
                    cJSON_Delete(node);
                    ctx->error = "JSON allocation failed";
                    return NULL;
                }
                lua_pop(L, 1);
            }
        }
        break;
    }
    default:
        break;
    }
    if (!node && !ctx->error)
        ctx->error = "unsupported JSON value or allocation failed";
    return node;
}
static int json_encode(lua_State *L) {
    struct encode_context ctx = {{0}, 0, 0, NULL};
    cJSON *node;
    char *output;
    struct json_storage *storage;
    luaL_checkany(L, 1);
    storage = new_storage(L);
    node = encode_value(L, 1, 0, &ctx);
    if (!node)
        return luaL_error(L, "%s", ctx.error);
    storage->node = node;
    output = malloc(JSON_LIMIT + 6);
    if (!output) {
        cJSON_Delete(node);
        storage->node = NULL;
        return luaL_error(L, "JSON allocation failed");
    }
    storage->output = output;
    if (!cJSON_PrintPreallocated(node, output, JSON_LIMIT + 6, 0) || strlen(output) > JSON_LIMIT) {
        free(output);
        cJSON_Delete(node);
        storage->output = NULL;
        storage->node = NULL;
        return luaL_error(L, "JSON output limit");
    }
    cJSON_Delete(node);
    storage->node = NULL;
    lua_pushstring(L, output);
    free(output);
    storage->output = NULL;
    return 1;
}

/* Validate before cJSON parses: its strings cannot represent embedded NUL. */
static int valid_input(const char *s, size_t size) {
    int quoted = 0, depth = 0;
    size_t separators = 0;
    if (size > JSON_LIMIT || !valid_text(s, size))
        return 0;
    for (size_t i = 0; i < size; ++i) {
        if (quoted) {
            if ((unsigned char)s[i] < 0x20)
                return 0;
            if (s[i] == '\\') {
                if (++i >= size)
                    return 0;
                if (s[i] == 'u' && i + 4 < size && memcmp(s + i + 1, "0000", 4) == 0)
                    return 0;
            } else if (s[i] == '"')
                quoted = 0;
        } else if (s[i] == '-' || (s[i] >= '0' && s[i] <= '9')) {
            size_t cursor = i;
            if (s[cursor] == '-')
                ++cursor;
            if (cursor >= size)
                return 0;
            if (s[cursor] == '0')
                ++cursor;
            else {
                if (s[cursor] < '1' || s[cursor] > '9')
                    return 0;
                while (cursor < size && s[cursor] >= '0' && s[cursor] <= '9')
                    ++cursor;
            }
            if (cursor < size && s[cursor] == '.') {
                ++cursor;
                if (cursor >= size || s[cursor] < '0' || s[cursor] > '9')
                    return 0;
                while (cursor < size && s[cursor] >= '0' && s[cursor] <= '9')
                    ++cursor;
            }
            if (cursor < size && (s[cursor] == 'e' || s[cursor] == 'E')) {
                ++cursor;
                if (cursor < size && (s[cursor] == '+' || s[cursor] == '-'))
                    ++cursor;
                if (cursor >= size || s[cursor] < '0' || s[cursor] > '9')
                    return 0;
                while (cursor < size && s[cursor] >= '0' && s[cursor] <= '9')
                    ++cursor;
            }
            if (cursor < size && s[cursor] != ' ' && s[cursor] != '\t' && s[cursor] != '\r' &&
                s[cursor] != '\n' && s[cursor] != ',' && s[cursor] != ']' && s[cursor] != '}')
                return 0;
            i = cursor - 1;
        } else if ((unsigned char)s[i] <= 0x20 && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' &&
                   s[i] != '\n')
            return 0;
        else if (s[i] == '"')
            quoted = 1;
        else if (s[i] == '[' || s[i] == '{') {
            if (++depth > JSON_DEPTH || ++separators > JSON_NODES)
                return 0;
        } else if (s[i] == ',' || s[i] == ':') {
            if (++separators > JSON_NODES)
                return 0;
        } else if (s[i] == ']' || s[i] == '}') {
            if (--depth < 0)
                return 0;
        }
    }
    return !quoted && depth == 0;
}
static const char *decode_value(lua_State *L, const cJSON *node, unsigned depth, size_t *nodes) {
    const cJSON *child;
    if (++*nodes > JSON_NODES || depth > JSON_DEPTH)
        return "JSON complexity limit";
    if (!lua_checkstack(L, 8))
        return "JSON Lua stack limit";
    if (cJSON_IsNull(node))
        lua_pushlightuserdata(L, &null_token);
    else if (cJSON_IsBool(node))
        lua_pushboolean(L, cJSON_IsTrue(node));
    else if (cJSON_IsNumber(node)) {
        double number = node->valuedouble;
        if (!isfinite(number))
            return "nonfinite JSON number";
        if (trunc(number) == number) {
            if (fabs(number) > JSON_SAFE_INTEGER)
                return "JSON integer exceeds safe range; use strings";
            lua_pushinteger(L, (lua_Integer)number);
        } else
            lua_pushnumber(L, number);
    } else if (cJSON_IsString(node)) {
        if (!valid_text(node->valuestring, strlen(node->valuestring)))
            return "invalid JSON string";
        lua_pushstring(L, node->valuestring);
    } else if (cJSON_IsArray(node) || cJSON_IsObject(node)) {
        int array = cJSON_IsArray(node), index = 1;
        lua_newtable(L);
        luaL_setmetatable(L, array ? ARRAY_META : OBJECT_META);
        for (child = node->child; child; child = child->next) {
            const char *error;
            if (!array) {
                if (!child->string || !valid_text(child->string, strlen(child->string)))
                    return "invalid JSON key";
                lua_pushstring(L, child->string);
                lua_rawget(L, -2);
                if (!lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    return "duplicate JSON key";
                }
                lua_pop(L, 1);
                lua_pushstring(L, child->string);
            }
            error = decode_value(L, child, depth + 1, nodes);
            if (error)
                return error;
            if (array)
                lua_rawseti(L, -2, index++);
            else
                lua_rawset(L, -3);
        }
    } else
        return "invalid JSON value";
    return NULL;
}
static int json_decode(lua_State *L) {
    size_t size, nodes = 0;
    const char *input = luaL_checklstring(L, 1, &size), *end, *error;
    cJSON *node;
    struct json_storage *storage;
    if (!valid_input(input, size))
        return luaL_error(L, "invalid or oversized JSON input");
    storage = new_storage(L);
    node = cJSON_ParseWithLengthOpts(input, size + 1, &end, 1);
    if (!node)
        return luaL_error(L, "invalid JSON input");
    storage->node = node;
    error = decode_value(L, node, 0, &nodes);
    cJSON_Delete(node);
    storage->node = NULL;
    if (error)
        return luaL_error(L, "%s", error);
    return 1;
}
static int json_mark(lua_State *L) {
    if (lua_isnoneornil(L, 1))
        lua_newtable(L);
    else {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_settop(L, 1);
    }
    luaL_setmetatable(L, lua_toboolean(L, lua_upvalueindex(1)) ? ARRAY_META : OBJECT_META);
    return 1;
}
void dejavu_lua_json_open(lua_State *L) {
    luaL_newmetatable(L, STORAGE_META);
    lua_pushcfunction(L, storage_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newmetatable(L, ARRAY_META);
    lua_pop(L, 1);
    luaL_newmetatable(L, OBJECT_META);
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, json_encode);
    lua_setfield(L, -2, "encode");
    lua_pushcfunction(L, json_decode);
    lua_setfield(L, -2, "decode");
    lua_pushlightuserdata(L, &null_token);
    lua_setfield(L, -2, "null");
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, json_mark, 1);
    lua_setfield(L, -2, "array");
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, json_mark, 1);
    lua_setfield(L, -2, "object");
    lua_setglobal(L, "json");
}
