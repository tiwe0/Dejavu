#include "dejavu_lua_sqlite.h"
#include "../third_party/sqlite3/sqlite3.h"
#include "lauxlib.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SQLITE_DB_META "dejavu.sqlite.db"
#define SQLITE_STMT_META "dejavu.sqlite.stmt"
#define SQLITE_BLOB_META "dejavu.sqlite.blob"

#define SQLITE_MAX_OPEN_DBS 64
#define SQLITE_DEFAULT_BUSY_MS 5000
#define SQLITE_DEFAULT_MAX_ROWS 100000

static char sqlite_null_token;
static char sqlite_open_count_key;

struct lua_sqlite_db;

struct lua_sqlite_stmt {
    sqlite3_stmt *stmt;
    struct lua_sqlite_db *owner;
    struct lua_sqlite_stmt *prev;
    struct lua_sqlite_stmt *next;
    int array_mode;
    int has_null_sentinel;
    lua_Integer max_rows; /* 0 means unlimited for streaming db:rows() */
    lua_Integer row_count;
};

struct lua_sqlite_db {
    sqlite3 *db;
    char *path;
    int open;
    uint64_t savepoint_seq;
    struct lua_sqlite_stmt *stmts;
};

/* Raw table field lookup: never invokes user __index metamethods. */
static int rawget_field(lua_State *L, int tbl_idx, const char *key) {
    int abs_idx = lua_absindex(L, tbl_idx);
    lua_pushstring(L, key);
    return lua_rawget(L, abs_idx);
}

static int get_open_db_count(lua_State *L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &sqlite_open_count_key);
    int count = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return count;
}

static void adjust_open_db_count(lua_State *L, int delta) {
    int count = get_open_db_count(L) + delta;
    if (count < 0) {
        count = 0;
    }
    lua_pushinteger(L, count);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &sqlite_open_count_key);
}

static void unlink_stmt(struct lua_sqlite_stmt *st) {
    if (st->owner == NULL) {
        return;
    }
    if (st->prev != NULL) {
        st->prev->next = st->next;
    } else {
        st->owner->stmts = st->next;
    }
    if (st->next != NULL) {
        st->next->prev = st->prev;
    }
    st->prev = NULL;
    st->next = NULL;
    st->owner = NULL;
}

static void finalize_stmt(struct lua_sqlite_stmt *st) {
    if (st->stmt != NULL) {
        sqlite3_finalize(st->stmt);
        st->stmt = NULL;
    }
    unlink_stmt(st);
}

static void close_db_internal(lua_State *L, struct lua_sqlite_db *ud) {
    int was_open = ud->open;
    while (ud->stmts != NULL) {
        finalize_stmt(ud->stmts);
    }
    if (ud->db != NULL) {
        sqlite3_close_v2(ud->db);
        ud->db = NULL;
    }
    ud->open = 0;
    if (was_open) {
        adjust_open_db_count(L, -1);
    }
}

static struct lua_sqlite_db *check_open_db(lua_State *L, int idx) {
    struct lua_sqlite_db *ud = (struct lua_sqlite_db *)luaL_checkudata(L, idx, SQLITE_DB_META);
    if (!ud->open || ud->db == NULL) {
        luaL_error(L, "SQLite database is closed");
    }
    return ud;
}

static struct lua_sqlite_stmt *check_open_stmt(lua_State *L, int idx) {
    struct lua_sqlite_stmt *st =
        (struct lua_sqlite_stmt *)luaL_checkudata(L, idx, SQLITE_STMT_META);
    if (st->stmt == NULL || st->owner == NULL || !st->owner->open) {
        luaL_error(L, "SQLite statement is closed");
    }
    return st;
}

static int is_null_sentinel(lua_State *L, int idx) {
    if (lua_type(L, idx) != LUA_TLIGHTUSERDATA) {
        return 0;
    }
    void *ptr = lua_touserdata(L, idx);
    if (ptr == (void *)&sqlite_null_token) {
        return 1;
    }
    /* Raw-lookup json.null without triggering _G or json metamethods. */
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_pushliteral(L, "json");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushliteral(L, "null");
        lua_rawget(L, -2);
        int matches = lua_type(L, -1) == LUA_TLIGHTUSERDATA && lua_touserdata(L, -1) == ptr;
        lua_pop(L, 3);
        return matches;
    }
    lua_pop(L, 2);
    return 0;
}

static int is_blob_udata(lua_State *L, int idx) {
    if (lua_type(L, idx) != LUA_TUSERDATA || !lua_getmetatable(L, idx)) {
        return 0;
    }
    luaL_getmetatable(L, SQLITE_BLOB_META);
    int same = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return same;
}

static int bind_one(lua_State *L, sqlite3_stmt *stmt, int param_idx, int val_idx) {
    int abs_val = lua_absindex(L, val_idx);
    int t = lua_type(L, abs_val);
    if (t == LUA_TNIL || is_null_sentinel(L, abs_val)) {
        return sqlite3_bind_null(stmt, param_idx);
    }
    if (t == LUA_TBOOLEAN) {
        return sqlite3_bind_int(stmt, param_idx, lua_toboolean(L, abs_val) ? 1 : 0);
    }
    if (t == LUA_TNUMBER) {
        if (lua_isinteger(L, abs_val)) {
            return sqlite3_bind_int64(stmt, param_idx, (sqlite3_int64)lua_tointeger(L, abs_val));
        }
        return sqlite3_bind_double(stmt, param_idx, (double)lua_tonumber(L, abs_val));
    }
    if (t == LUA_TSTRING) {
        size_t len = 0;
        const char *s = lua_tolstring(L, abs_val, &len);
        return sqlite3_bind_text64(stmt, param_idx, s, (sqlite3_uint64)len, SQLITE_TRANSIENT,
                                   SQLITE_UTF8);
    }
    if (is_blob_udata(L, abs_val)) {
        lua_getiuservalue(L, abs_val, 1);
        size_t len = 0;
        const char *s = lua_tolstring(L, -1, &len);
        int rc = sqlite3_bind_blob64(stmt, param_idx, s ? (const void *)s : (const void *)"",
                                     (sqlite3_uint64)len, SQLITE_TRANSIENT);
        lua_pop(L, 1);
        return rc;
    }
    return luaL_error(L, "unsupported SQLite parameter type for parameter %d: %s", param_idx,
                      lua_typename(L, t));
}

static void bind_params(lua_State *L, sqlite3 *db, sqlite3_stmt *stmt, int params_idx) {
    if (params_idx != 0 && !lua_isnoneornil(L, params_idx)) {
        luaL_checktype(L, lua_absindex(L, params_idx), LUA_TTABLE);
    }
    sqlite3_clear_bindings(stmt);
    if (params_idx == 0 || lua_isnoneornil(L, params_idx)) {
        return;
    }
    int abs_params = lua_absindex(L, params_idx);

    int count = sqlite3_bind_parameter_count(stmt);
    for (int i = 1; i <= count; ++i) {
        const char *pname = sqlite3_bind_parameter_name(stmt, i);
        if (pname != NULL && (pname[0] == ':' || pname[0] == '@' || pname[0] == '$')) {
            lua_pushstring(L, pname + 1);
            lua_rawget(L, abs_params);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_pushstring(L, pname);
                lua_rawget(L, abs_params);
            }
        } else if (pname != NULL && pname[0] == '?' && pname[1] != '\0') {
            long long pos = strtoll(pname + 1, NULL, 10);
            lua_rawgeti(L, abs_params, (lua_Integer)pos);
        } else {
            lua_rawgeti(L, abs_params, i);
        }
        int rc = bind_one(L, stmt, i, -1);
        lua_pop(L, 1);
        if (rc != SQLITE_OK) {
            luaL_error(L, "SQLite bind error on parameter %d: %s", i, sqlite3_errmsg(db));
        }
    }
}

static void push_column_value(lua_State *L, sqlite3_stmt *stmt, int col, int null_idx) {
    int ctype = sqlite3_column_type(stmt, col);
    switch (ctype) {
    case SQLITE_INTEGER:
        lua_pushinteger(L, (lua_Integer)sqlite3_column_int64(stmt, col));
        break;
    case SQLITE_FLOAT:
        lua_pushnumber(L, (lua_Number)sqlite3_column_double(stmt, col));
        break;
    case SQLITE_TEXT: {
        const unsigned char *text = sqlite3_column_text(stmt, col);
        int bytes = sqlite3_column_bytes(stmt, col);
        if (text != NULL && bytes > 0) {
            lua_pushlstring(L, (const char *)text, (size_t)bytes);
        } else {
            lua_pushliteral(L, "");
        }
        break;
    }
    case SQLITE_BLOB: {
        const void *blob = sqlite3_column_blob(stmt, col);
        int bytes = sqlite3_column_bytes(stmt, col);
        if (blob != NULL && bytes > 0) {
            lua_pushlstring(L, (const char *)blob, (size_t)bytes);
        } else {
            lua_pushliteral(L, "");
        }
        break;
    }
    case SQLITE_NULL:
    default:
        if (null_idx != 0) {
            lua_pushvalue(L, null_idx);
        } else {
            lua_pushnil(L);
        }
        break;
    }
}

static void push_row(lua_State *L, sqlite3_stmt *stmt, int array_mode, int null_idx) {
    int cols = sqlite3_column_count(stmt);
    if (array_mode) {
        lua_createtable(L, cols, 0);
        for (int c = 0; c < cols; ++c) {
            push_column_value(L, stmt, c, null_idx);
            lua_rawseti(L, -2, c + 1);
        }
    } else {
        lua_createtable(L, 0, cols);
        for (int c = 0; c < cols; ++c) {
            const char *name = sqlite3_column_name(stmt, c);
            if (name != NULL) {
                lua_pushstring(L, name);
            } else {
                lua_pushinteger(L, c + 1);
            }
            push_column_value(L, stmt, c, null_idx);
            lua_rawset(L, -3);
        }
    }
}

static struct lua_sqlite_stmt *create_stmt_udata(lua_State *L, int db_idx,
                                                 struct lua_sqlite_db *owner, const char *sql,
                                                 size_t sql_len) {
    if (sql_len > (size_t)INT_MAX) {
        luaL_error(L, "SQL statement is too large");
    }
    if (memchr(sql, '\0', sql_len) != NULL) {
        luaL_error(L, "SQL statement contains embedded NUL bytes");
    }

    int abs_db = lua_absindex(L, db_idx);

    /* Allocate userdata and attach SQLITE_STMT_META BEFORE sqlite3_prepare_v2 so an OOM
     * during userdata creation can never leak a prepared sqlite3_stmt or leave an unlinked node. */
    struct lua_sqlite_stmt *st = (struct lua_sqlite_stmt *)lua_newuserdatauv(L, sizeof(*st), 2);
    memset(st, 0, sizeof(*st));
    lua_pushvalue(L, abs_db);
    lua_setiuservalue(L, -2, 1);
    luaL_setmetatable(L, SQLITE_STMT_META);

    sqlite3_stmt *raw = NULL;
    const char *tail = NULL;
    int rc = sqlite3_prepare_v2(owner->db, sql, (int)sql_len, &raw, &tail);
    if (rc != SQLITE_OK) {
        luaL_error(L, "SQLite prepare error: %s", sqlite3_errmsg(owner->db));
    }
    if (raw == NULL) {
        luaL_error(L, "SQL statement is empty");
    }

    /* Validate trailing text via sqlite3_prepare_v2 so comments/whitespace after ';' are
     * accepted while any second SQL statement is cleanly rejected. */
    const char *cur = tail;
    const char *end = sql + sql_len;
    while (cur != NULL && cur < end) {
        sqlite3_stmt *extra = NULL;
        const char *next_tail = NULL;
        rc = sqlite3_prepare_v2(owner->db, cur, (int)(end - cur), &extra, &next_tail);
        if (rc != SQLITE_OK) {
            char err[256];
            snprintf(err, sizeof(err), "SQLite prepare error: %s", sqlite3_errmsg(owner->db));
            sqlite3_finalize(raw);
            luaL_error(L, "%s", err);
        }
        if (extra != NULL) {
            sqlite3_finalize(extra);
            sqlite3_finalize(raw);
            luaL_error(L, "expected a single SQL statement");
        }
        if (next_tail == NULL || next_tail <= cur) {
            break;
        }
        cur = next_tail;
    }

    st->stmt = raw;
    st->owner = owner;
    st->prev = NULL;
    st->next = owner->stmts;
    if (owner->stmts != NULL) {
        owner->stmts->prev = st;
    }
    owner->stmts = st;
    return st;
}

/* ==================== Blob API ==================== */

static int lua_sqlite_blob(lua_State *L) {
    luaL_checkstring(L, 1);
    lua_newuserdatauv(L, 1, 1);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);
    luaL_setmetatable(L, SQLITE_BLOB_META);
    return 1;
}

static int lua_sqlite_is_blob(lua_State *L) {
    lua_pushboolean(L, is_blob_udata(L, 1));
    return 1;
}

static int blob_bytes(lua_State *L) {
    luaL_checkudata(L, 1, SQLITE_BLOB_META);
    lua_getiuservalue(L, 1, 1);
    return 1;
}

static int blob_len(lua_State *L) {
    luaL_checkudata(L, 1, SQLITE_BLOB_META);
    lua_getiuservalue(L, 1, 1);
    size_t len = 0;
    lua_tolstring(L, -1, &len);
    lua_pop(L, 1);
    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}

static int blob_tostring(lua_State *L) {
    luaL_checkudata(L, 1, SQLITE_BLOB_META);
    lua_getiuservalue(L, 1, 1);
    size_t len = 0;
    lua_tolstring(L, -1, &len);
    lua_pop(L, 1);
    lua_pushfstring(L, "<sqlite.blob %I bytes>", (lua_Integer)len);
    return 1;
}

/* ==================== DB Open / Close ==================== */

static int parse_open_flags(lua_State *L, int opt_idx, int *out_flags, int *out_busy_ms,
                            int *out_fk, int *out_wal) {
    *out_flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    *out_busy_ms = SQLITE_DEFAULT_BUSY_MS;
    *out_fk = -1;
    *out_wal = 0;

    if (lua_isnoneornil(L, opt_idx)) {
        return 1;
    }
    if (lua_type(L, opt_idx) == LUA_TSTRING) {
        const char *mode = lua_tostring(L, opt_idx);
        if (strcmp(mode, "ro") == 0 || strcmp(mode, "readonly") == 0) {
            *out_flags = SQLITE_OPEN_READONLY;
        } else if (strcmp(mode, "rw") == 0) {
            *out_flags = SQLITE_OPEN_READWRITE;
        } else if (strcmp(mode, "rwc") == 0) {
            *out_flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        } else {
            return luaL_argerror(L, opt_idx, "mode must be 'ro', 'rw', or 'rwc'");
        }
        return 1;
    }
    if (!lua_istable(L, opt_idx)) {
        return luaL_argerror(L, opt_idx, "expected mode string or options table");
    }

    rawget_field(L, opt_idx, "mode");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *mode = lua_tostring(L, -1);
        if (strcmp(mode, "ro") == 0 || strcmp(mode, "readonly") == 0) {
            *out_flags = SQLITE_OPEN_READONLY;
        } else if (strcmp(mode, "rw") == 0) {
            *out_flags = SQLITE_OPEN_READWRITE;
        } else if (strcmp(mode, "rwc") == 0) {
            *out_flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        } else {
            lua_pop(L, 1);
            return luaL_error(L, "invalid SQLite mode: must be 'ro', 'rw', or 'rwc'");
        }
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "SQLite option 'mode' must be a string");
    }
    lua_pop(L, 1);

    rawget_field(L, opt_idx, "readonly");
    if (lua_toboolean(L, -1)) {
        *out_flags = SQLITE_OPEN_READONLY;
    }
    lua_pop(L, 1);

    rawget_field(L, opt_idx, "uri");
    if (lua_toboolean(L, -1)) {
        *out_flags |= SQLITE_OPEN_URI;
    }
    lua_pop(L, 1);

    rawget_field(L, opt_idx, "busy_timeout");
    if (lua_isinteger(L, -1)) {
        lua_Integer ms = lua_tointeger(L, -1);
        if (ms < 0 || ms > 600000) {
            lua_pop(L, 1);
            return luaL_error(L, "busy_timeout must be between 0 and 600000 ms");
        }
        *out_busy_ms = (int)ms;
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "busy_timeout must be an integer");
    }
    lua_pop(L, 1);

    rawget_field(L, opt_idx, "foreign_keys");
    if (!lua_isnil(L, -1)) {
        *out_fk = lua_toboolean(L, -1) ? 1 : 0;
    }
    lua_pop(L, 1);

    rawget_field(L, opt_idx, "wal");
    if (lua_toboolean(L, -1)) {
        *out_wal = 1;
    }
    lua_pop(L, 1);

    return 1;
}

static int lua_sqlite_open(lua_State *L) {
    size_t path_len = 0;
    const char *path = luaL_checklstring(L, 1, &path_len);
    if (path_len == 0 || memchr(path, '\0', path_len) != NULL) {
        return luaL_argerror(L, 1, "invalid database path");
    }

    int flags = 0;
    int busy_ms = 0;
    int fk = -1;
    int wal = 0;
    parse_open_flags(L, 2, &flags, &busy_ms, &fk, &wal);

    if (get_open_db_count(L) >= SQLITE_MAX_OPEN_DBS) {
        return luaL_error(L, "too many open SQLite databases (limit %d)", SQLITE_MAX_OPEN_DBS);
    }

    /* Allocate userdata and attach SQLITE_DB_META first so OOM never leaks raw sqlite3/path. */
    struct lua_sqlite_db *ud = (struct lua_sqlite_db *)lua_newuserdatauv(L, sizeof(*ud), 0);
    memset(ud, 0, sizeof(*ud));
    luaL_setmetatable(L, SQLITE_DB_META);

    char *path_copy = (char *)malloc(path_len + 1);
    if (path_copy == NULL) {
        return luaL_error(L, "out of memory");
    }
    memcpy(path_copy, path, path_len + 1);
    ud->path = path_copy;

    sqlite3 *raw = NULL;
    int rc = sqlite3_open_v2(path, &raw, flags, NULL);
    if (rc != SQLITE_OK) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "%s",
                 raw != NULL ? sqlite3_errmsg(raw) : sqlite3_errstr(rc));
        if (raw != NULL) {
            sqlite3_close_v2(raw);
        }
        return luaL_error(L, "unable to open SQLite database '%s': %s", path, errmsg);
    }
    ud->db = raw;
    ud->open = 1;
    adjust_open_db_count(L, 1);

    sqlite3_busy_timeout(raw, busy_ms);
    if (fk >= 0) {
        rc = sqlite3_exec(raw, fk ? "PRAGMA foreign_keys = ON;" : "PRAGMA foreign_keys = OFF;",
                          NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            char errmsg[256];
            snprintf(errmsg, sizeof(errmsg), "%s", sqlite3_errmsg(raw));
            close_db_internal(L, ud);
            return luaL_error(L, "SQLite PRAGMA foreign_keys failed: %s", errmsg);
        }
    }
    if (wal && sqlite3_db_readonly(raw, "main") == 0) {
        rc = sqlite3_exec(raw, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            char errmsg[256];
            snprintf(errmsg, sizeof(errmsg), "%s", sqlite3_errmsg(raw));
            close_db_internal(L, ud);
            return luaL_error(L, "SQLite PRAGMA journal_mode = WAL failed: %s", errmsg);
        }
    }

    return 1;
}

static int lua_sqlite_is_db(lua_State *L) {
    if (lua_type(L, 1) != LUA_TUSERDATA || !lua_getmetatable(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    luaL_getmetatable(L, SQLITE_DB_META);
    int same = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    lua_pushboolean(L, same);
    return 1;
}

static int db_close(lua_State *L) {
    struct lua_sqlite_db *ud = (struct lua_sqlite_db *)luaL_checkudata(L, 1, SQLITE_DB_META);
    close_db_internal(L, ud);
    lua_pushboolean(L, 1);
    return 1;
}

static int db_gc(lua_State *L) {
    struct lua_sqlite_db *ud = (struct lua_sqlite_db *)luaL_checkudata(L, 1, SQLITE_DB_META);
    free(ud->path);
    ud->path = NULL;
    close_db_internal(L, ud);
    return 0;
}

static int db_is_open(lua_State *L) {
    struct lua_sqlite_db *ud = (struct lua_sqlite_db *)luaL_checkudata(L, 1, SQLITE_DB_META);
    lua_pushboolean(L, ud->open && ud->db != NULL);
    return 1;
}

static int db_path(lua_State *L) {
    struct lua_sqlite_db *ud = (struct lua_sqlite_db *)luaL_checkudata(L, 1, SQLITE_DB_META);
    lua_pushstring(L, ud->path ? ud->path : "");
    return 1;
}

static int db_tostring(lua_State *L) {
    struct lua_sqlite_db *ud = (struct lua_sqlite_db *)luaL_checkudata(L, 1, SQLITE_DB_META);
    lua_pushfstring(L, "<sqlite.db %s (%s)>", ud->path ? ud->path : "?",
                    ud->open ? "open" : "closed");
    return 1;
}

static int db_changes(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    lua_pushinteger(L, (lua_Integer)sqlite3_changes64(ud->db));
    return 1;
}

static int db_total_changes(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    lua_pushinteger(L, (lua_Integer)sqlite3_total_changes64(ud->db));
    return 1;
}

static int db_last_insert_rowid(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    lua_pushinteger(L, (lua_Integer)sqlite3_last_insert_rowid(ud->db));
    return 1;
}

static int db_in_transaction(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    lua_pushboolean(L, sqlite3_get_autocommit(ud->db) == 0);
    return 1;
}

static int db_busy_timeout(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    lua_Integer ms = luaL_checkinteger(L, 2);
    if (ms < 0 || ms > 600000) {
        return luaL_argerror(L, 2, "busy_timeout must be between 0 and 600000 ms");
    }
    sqlite3_busy_timeout(ud->db, (int)ms);
    lua_pushboolean(L, 1);
    return 1;
}

/* ==================== Execution & Queries ==================== */

static int db_exec(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    size_t sql_len = 0;
    const char *sql = luaL_checklstring(L, 2, &sql_len);
    if (sql_len > (size_t)INT_MAX) {
        return luaL_error(L, "SQL statement is too large");
    }
    if (memchr(sql, '\0', sql_len) != NULL) {
        return luaL_error(L, "SQL statement contains embedded NUL bytes");
    }

    if (!lua_isnoneornil(L, 3)) {
        /* Single parameterized statement */
        struct lua_sqlite_stmt *st = create_stmt_udata(L, 1, ud, sql, sql_len);
        int st_idx = lua_gettop(L);
        bind_params(L, ud->db, st->stmt, 3);
        int rc;
        do {
            rc = sqlite3_step(st->stmt);
        } while (rc == SQLITE_ROW);
        if (rc != SQLITE_DONE) {
            char err[256];
            snprintf(err, sizeof(err), "SQLite exec error: %s", sqlite3_errmsg(ud->db));
            finalize_stmt(st);
            lua_remove(L, st_idx);
            return luaL_error(L, "%s", err);
        }
        finalize_stmt(st);
        lua_remove(L, st_idx);
        lua_pushinteger(L, (lua_Integer)sqlite3_changes64(ud->db));
        lua_pushinteger(L, (lua_Integer)sqlite3_last_insert_rowid(ud->db));
        return 2;
    }

    /* Multi-statement non-parameterized script */
    const char *cur = sql;
    const char *end = sql + sql_len;
    sqlite3_int64 total_before = sqlite3_total_changes64(ud->db);
    while (cur < end) {
        sqlite3_stmt *raw = NULL;
        const char *tail = NULL;
        int rc = sqlite3_prepare_v2(ud->db, cur, (int)(end - cur), &raw, &tail);
        if (rc != SQLITE_OK) {
            return luaL_error(L, "SQLite prepare error: %s", sqlite3_errmsg(ud->db));
        }
        if (raw != NULL) {
            do {
                rc = sqlite3_step(raw);
            } while (rc == SQLITE_ROW);
            if (rc != SQLITE_DONE) {
                char err[256];
                snprintf(err, sizeof(err), "SQLite exec error: %s", sqlite3_errmsg(ud->db));
                sqlite3_finalize(raw);
                return luaL_error(L, "%s", err);
            }
            sqlite3_finalize(raw);
        }
        if (tail == NULL || tail <= cur) {
            break;
        }
        cur = tail;
    }
    sqlite3_int64 delta = sqlite3_total_changes64(ud->db) - total_before;
    lua_pushinteger(L, (lua_Integer)delta);
    lua_pushinteger(L, (lua_Integer)sqlite3_last_insert_rowid(ud->db));
    return 2;
}

static void parse_query_opts(lua_State *L, int opt_idx, int *array_mode, lua_Integer *max_rows,
                             int *max_rows_set, int *null_idx) {
    *array_mode = 0;
    *max_rows = SQLITE_DEFAULT_MAX_ROWS;
    if (max_rows_set != NULL) {
        *max_rows_set = 0;
    }
    *null_idx = 0;
    if (lua_isnoneornil(L, opt_idx)) {
        return;
    }
    luaL_checktype(L, opt_idx, LUA_TTABLE);
    rawget_field(L, opt_idx, "mode");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *m = lua_tostring(L, -1);
        if (strcmp(m, "array") == 0) {
            *array_mode = 1;
        } else if (strcmp(m, "map") != 0 && strcmp(m, "object") != 0) {
            lua_pop(L, 1);
            luaL_error(L, "query option 'mode' must be 'map' or 'array'");
        }
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "query option 'mode' must be a string");
    }
    lua_pop(L, 1);

    rawget_field(L, opt_idx, "max_rows");
    if (lua_isinteger(L, -1)) {
        lua_Integer lim = lua_tointeger(L, -1);
        if (lim <= 0 || lim > 1000000) {
            lua_pop(L, 1);
            luaL_error(L, "max_rows must be between 1 and 1000000");
        }
        *max_rows = lim;
        if (max_rows_set != NULL) {
            *max_rows_set = 1;
        }
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "max_rows must be an integer");
    }
    lua_pop(L, 1);

    rawget_field(L, opt_idx, "null");
    if (!lua_isnil(L, -1)) {
        *null_idx = lua_gettop(L); /* Caller keeps sentinel on stack */
    } else {
        lua_pop(L, 1);
    }
}

static int db_query(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    size_t sql_len = 0;
    const char *sql = luaL_checklstring(L, 2, &sql_len);
    int params_idx = !lua_isnoneornil(L, 3) ? 3 : 0;
    int array_mode = 0;
    lua_Integer max_rows = SQLITE_DEFAULT_MAX_ROWS;
    int null_idx = 0;
    parse_query_opts(L, 4, &array_mode, &max_rows, NULL, &null_idx);

    struct lua_sqlite_stmt *st = create_stmt_udata(L, 1, ud, sql, sql_len);
    int st_idx = lua_gettop(L);
    bind_params(L, ud->db, st->stmt, params_idx);

    lua_newtable(L);
    int rows_idx = lua_gettop(L);
    lua_Integer count = 0;

    for (;;) {
        int rc = sqlite3_step(st->stmt);
        if (rc == SQLITE_ROW) {
            if (count >= max_rows) {
                finalize_stmt(st);
                return luaL_error(L, "SQLite query exceeded max_rows limit (%I)", max_rows);
            }
            push_row(L, st->stmt, array_mode, null_idx);
            lua_rawseti(L, rows_idx, ++count);
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            char err[256];
            snprintf(err, sizeof(err), "SQLite query error: %s", sqlite3_errmsg(ud->db));
            finalize_stmt(st);
            return luaL_error(L, "%s", err);
        }
    }

    finalize_stmt(st);
    lua_remove(L, st_idx);
    if (null_idx != 0) {
        lua_remove(L, null_idx);
    }
    return 1;
}

static int db_one(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    size_t sql_len = 0;
    const char *sql = luaL_checklstring(L, 2, &sql_len);
    int params_idx = !lua_isnoneornil(L, 3) ? 3 : 0;
    int array_mode = 0;
    lua_Integer max_rows = 1;
    int null_idx = 0;
    parse_query_opts(L, 4, &array_mode, &max_rows, NULL, &null_idx);

    struct lua_sqlite_stmt *st = create_stmt_udata(L, 1, ud, sql, sql_len);
    int st_idx = lua_gettop(L);
    bind_params(L, ud->db, st->stmt, params_idx);

    int rc = sqlite3_step(st->stmt);
    if (rc == SQLITE_ROW) {
        push_row(L, st->stmt, array_mode, null_idx);
    } else if (rc == SQLITE_DONE) {
        lua_pushnil(L);
    } else {
        char err[256];
        snprintf(err, sizeof(err), "SQLite query error: %s", sqlite3_errmsg(ud->db));
        finalize_stmt(st);
        return luaL_error(L, "%s", err);
    }

    finalize_stmt(st);
    lua_remove(L, st_idx);
    if (null_idx != 0) {
        lua_remove(L, null_idx);
    }
    return 1;
}

static int db_scalar(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    size_t sql_len = 0;
    const char *sql = luaL_checklstring(L, 2, &sql_len);
    int params_idx = !lua_isnoneornil(L, 3) ? 3 : 0;

    struct lua_sqlite_stmt *st = create_stmt_udata(L, 1, ud, sql, sql_len);
    int st_idx = lua_gettop(L);
    bind_params(L, ud->db, st->stmt, params_idx);

    int rc = sqlite3_step(st->stmt);
    if (rc == SQLITE_ROW && sqlite3_column_count(st->stmt) > 0) {
        push_column_value(L, st->stmt, 0, 0);
    } else if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
        lua_pushnil(L);
    } else {
        char err[256];
        snprintf(err, sizeof(err), "SQLite query error: %s", sqlite3_errmsg(ud->db));
        finalize_stmt(st);
        return luaL_error(L, "%s", err);
    }

    finalize_stmt(st);
    lua_remove(L, st_idx);
    return 1;
}

/* Streaming Lua 5.4 generic-for <close> iterator */
static int rows_iter_next(lua_State *L) {
    struct lua_sqlite_stmt *st = (struct lua_sqlite_stmt *)luaL_checkudata(L, 1, SQLITE_STMT_META);
    if (st->stmt == NULL || st->owner == NULL || !st->owner->open) {
        return 0;
    }
    int rc = sqlite3_step(st->stmt);
    if (rc == SQLITE_ROW) {
        if (st->max_rows > 0 && st->row_count >= st->max_rows) {
            lua_Integer lim = st->max_rows;
            finalize_stmt(st);
            return luaL_error(L, "SQLite rows iterator exceeded max_rows limit (%I)", lim);
        }
        ++st->row_count;
        int null_idx = 0;
        if (st->has_null_sentinel) {
            lua_getiuservalue(L, 1, 2);
            null_idx = lua_gettop(L);
        }
        push_row(L, st->stmt, st->array_mode, null_idx);
        if (null_idx != 0) {
            lua_remove(L, null_idx);
        }
        return 1;
    }
    if (rc == SQLITE_DONE) {
        finalize_stmt(st);
        return 0;
    }
    char err[256];
    snprintf(err, sizeof(err), "SQLite step error: %s", sqlite3_errmsg(st->owner->db));
    finalize_stmt(st);
    return luaL_error(L, "%s", err);
}

static int db_rows(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    size_t sql_len = 0;
    const char *sql = luaL_checklstring(L, 2, &sql_len);
    int params_idx = !lua_isnoneornil(L, 3) ? 3 : 0;
    int array_mode = 0;
    lua_Integer max_rows = 0;
    int max_rows_set = 0;
    int null_idx = 0;
    parse_query_opts(L, 4, &array_mode, &max_rows, &max_rows_set, &null_idx);

    struct lua_sqlite_stmt *st = create_stmt_udata(L, 1, ud, sql, sql_len);
    st->array_mode = array_mode;
    st->max_rows = max_rows_set ? max_rows : 0;
    st->row_count = 0;
    if (null_idx != 0) {
        st->has_null_sentinel = 1;
        lua_pushvalue(L, null_idx);
        lua_setiuservalue(L, -2, 2);
    }
    bind_params(L, ud->db, st->stmt, params_idx);

    int st_idx = lua_gettop(L);
    lua_pushcfunction(L, rows_iter_next);
    lua_pushvalue(L, st_idx);
    lua_pushnil(L);
    lua_pushvalue(L, st_idx); /* 4th value: Lua 5.4 to-be-closed handle */
    return 4;
}

/* ==================== Transactions (with nested SAVEPOINT support) ==================== */

static int db_transaction(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    const char *mode = "deferred";
    if (!lua_isnoneornil(L, 3)) {
        mode = luaL_checkstring(L, 3);
    }
    if (strcmp(mode, "deferred") != 0 && strcmp(mode, "immediate") != 0 &&
        strcmp(mode, "exclusive") != 0) {
        return luaL_argerror(L, 3,
                             "transaction mode must be 'deferred', 'immediate', or 'exclusive'");
    }

    int nested = (sqlite3_get_autocommit(ud->db) == 0);
    char begin_sql[64];
    char commit_sql[64];
    char rollback_sql[128];

    if (nested) {
        uint64_t sp_id = ++ud->savepoint_seq;
        snprintf(begin_sql, sizeof(begin_sql), "SAVEPOINT _djv_sp_%llu;",
                 (unsigned long long)sp_id);
        snprintf(commit_sql, sizeof(commit_sql), "RELEASE SAVEPOINT _djv_sp_%llu;",
                 (unsigned long long)sp_id);
        snprintf(rollback_sql, sizeof(rollback_sql),
                 "ROLLBACK TO SAVEPOINT _djv_sp_%llu; RELEASE SAVEPOINT _djv_sp_%llu;",
                 (unsigned long long)sp_id, (unsigned long long)sp_id);
    } else {
        if (strcmp(mode, "deferred") == 0) {
            snprintf(begin_sql, sizeof(begin_sql), "BEGIN DEFERRED;");
        } else if (strcmp(mode, "immediate") == 0) {
            snprintf(begin_sql, sizeof(begin_sql), "BEGIN IMMEDIATE;");
        } else {
            snprintf(begin_sql, sizeof(begin_sql), "BEGIN EXCLUSIVE;");
        }
        snprintf(commit_sql, sizeof(commit_sql), "COMMIT;");
        snprintf(rollback_sql, sizeof(rollback_sql), "ROLLBACK;");
    }

    if (sqlite3_exec(ud->db, begin_sql, NULL, NULL, NULL) != SQLITE_OK) {
        return luaL_error(L, "SQLite transaction start error: %s", sqlite3_errmsg(ud->db));
    }

    int base_top = lua_gettop(L);
    lua_pushvalue(L, 2); /* fn */
    lua_pushvalue(L, 1); /* db */
    int call_status = lua_pcall(L, 1, LUA_MULTRET, 0);
    if (call_status != LUA_OK) {
        if (ud->open && ud->db != NULL) {
            sqlite3_exec(ud->db, rollback_sql, NULL, NULL, NULL);
        }
        return lua_error(L);
    }

    int nret = lua_gettop(L) - base_top;
    if (!ud->open || ud->db == NULL) {
        lua_settop(L, base_top);
        return luaL_error(L, "SQLite database closed during transaction");
    }
    if (sqlite3_exec(ud->db, commit_sql, NULL, NULL, NULL) != SQLITE_OK) {
        char err[256];
        snprintf(err, sizeof(err), "SQLite commit error: %s", sqlite3_errmsg(ud->db));
        sqlite3_exec(ud->db, rollback_sql, NULL, NULL, NULL);
        lua_settop(L, base_top);
        return luaL_error(L, "%s", err);
    }
    return nret;
}

/* ==================== Prepared Statements ==================== */

static int db_prepare(lua_State *L) {
    struct lua_sqlite_db *ud = check_open_db(L, 1);
    size_t sql_len = 0;
    const char *sql = luaL_checklstring(L, 2, &sql_len);
    create_stmt_udata(L, 1, ud, sql, sql_len);
    return 1;
}

static int stmt_bind(lua_State *L) {
    struct lua_sqlite_stmt *st = check_open_stmt(L, 1);
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
    }
    sqlite3_reset(st->stmt);
    bind_params(L, st->owner->db, st->stmt, 2);
    lua_pushvalue(L, 1);
    return 1;
}

static int stmt_reset(lua_State *L) {
    struct lua_sqlite_stmt *st = check_open_stmt(L, 1);
    sqlite3_reset(st->stmt);
    lua_pushvalue(L, 1);
    return 1;
}

static int stmt_step(lua_State *L) {
    struct lua_sqlite_stmt *st = check_open_stmt(L, 1);
    int array_mode = 0;
    lua_Integer max_rows = 1;
    int null_idx = 0;
    parse_query_opts(L, 2, &array_mode, &max_rows, NULL, &null_idx);

    int rc = sqlite3_step(st->stmt);
    if (rc == SQLITE_ROW) {
        push_row(L, st->stmt, array_mode, null_idx);
        if (null_idx != 0) {
            lua_remove(L, null_idx);
        }
        return 1;
    }
    if (null_idx != 0) {
        lua_remove(L, null_idx);
    }
    if (rc == SQLITE_DONE) {
        lua_pushnil(L);
        return 1;
    }
    char err[256];
    snprintf(err, sizeof(err), "SQLite step error: %s", sqlite3_errmsg(st->owner->db));
    sqlite3_reset(st->stmt);
    return luaL_error(L, "%s", err);
}

static int stmt_all(lua_State *L) {
    struct lua_sqlite_stmt *st = check_open_stmt(L, 1);
    int params_idx = !lua_isnoneornil(L, 2) ? 2 : 0;
    if (params_idx != 0) {
        luaL_checktype(L, params_idx, LUA_TTABLE);
    }
    int array_mode = 0;
    lua_Integer max_rows = SQLITE_DEFAULT_MAX_ROWS;
    int null_idx = 0;
    parse_query_opts(L, 3, &array_mode, &max_rows, NULL, &null_idx);

    sqlite3_reset(st->stmt);
    if (params_idx != 0) {
        bind_params(L, st->owner->db, st->stmt, params_idx);
    }

    lua_newtable(L);
    int rows_idx = lua_gettop(L);
    lua_Integer count = 0;
    for (;;) {
        int rc = sqlite3_step(st->stmt);
        if (rc == SQLITE_ROW) {
            if (count >= max_rows) {
                sqlite3_reset(st->stmt);
                return luaL_error(L, "SQLite query exceeded max_rows limit (%I)", max_rows);
            }
            push_row(L, st->stmt, array_mode, null_idx);
            lua_rawseti(L, rows_idx, ++count);
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            char err[256];
            snprintf(err, sizeof(err), "SQLite step error: %s", sqlite3_errmsg(st->owner->db));
            sqlite3_reset(st->stmt);
            return luaL_error(L, "%s", err);
        }
    }
    sqlite3_reset(st->stmt);
    if (null_idx != 0) {
        lua_remove(L, null_idx);
    }
    return 1;
}

static int stmt_one(lua_State *L) {
    struct lua_sqlite_stmt *st = check_open_stmt(L, 1);
    int params_idx = !lua_isnoneornil(L, 2) ? 2 : 0;
    if (params_idx != 0) {
        luaL_checktype(L, params_idx, LUA_TTABLE);
    }
    int array_mode = 0;
    lua_Integer max_rows = 1;
    int null_idx = 0;
    parse_query_opts(L, 3, &array_mode, &max_rows, NULL, &null_idx);

    sqlite3_reset(st->stmt);
    if (params_idx != 0) {
        bind_params(L, st->owner->db, st->stmt, params_idx);
    }

    int rc = sqlite3_step(st->stmt);
    if (rc == SQLITE_ROW) {
        push_row(L, st->stmt, array_mode, null_idx);
    } else if (rc == SQLITE_DONE) {
        lua_pushnil(L);
    } else {
        char err[256];
        snprintf(err, sizeof(err), "SQLite step error: %s", sqlite3_errmsg(st->owner->db));
        sqlite3_reset(st->stmt);
        return luaL_error(L, "%s", err);
    }
    sqlite3_reset(st->stmt);
    if (null_idx != 0) {
        lua_remove(L, null_idx);
    }
    return 1;
}

static int stmt_run(lua_State *L) {
    struct lua_sqlite_stmt *st = check_open_stmt(L, 1);
    int params_idx = !lua_isnoneornil(L, 2) ? 2 : 0;
    if (params_idx != 0) {
        luaL_checktype(L, params_idx, LUA_TTABLE);
    }
    sqlite3_reset(st->stmt);
    if (params_idx != 0) {
        bind_params(L, st->owner->db, st->stmt, params_idx);
    }
    int rc;
    do {
        rc = sqlite3_step(st->stmt);
    } while (rc == SQLITE_ROW);
    if (rc != SQLITE_DONE) {
        char err[256];
        snprintf(err, sizeof(err), "SQLite step error: %s", sqlite3_errmsg(st->owner->db));
        sqlite3_reset(st->stmt);
        return luaL_error(L, "%s", err);
    }
    sqlite3_reset(st->stmt);
    lua_pushinteger(L, (lua_Integer)sqlite3_changes64(st->owner->db));
    lua_pushinteger(L, (lua_Integer)sqlite3_last_insert_rowid(st->owner->db));
    return 2;
}

static int stmt_columns(lua_State *L) {
    struct lua_sqlite_stmt *st = check_open_stmt(L, 1);
    int cols = sqlite3_column_count(st->stmt);
    lua_createtable(L, cols, 0);
    for (int c = 0; c < cols; ++c) {
        const char *name = sqlite3_column_name(st->stmt, c);
        lua_pushstring(L, name ? name : "");
        lua_rawseti(L, -2, c + 1);
    }
    return 1;
}

static int stmt_close(lua_State *L) {
    struct lua_sqlite_stmt *st = (struct lua_sqlite_stmt *)luaL_checkudata(L, 1, SQLITE_STMT_META);
    finalize_stmt(st);
    lua_pushboolean(L, 1);
    return 1;
}

static int stmt_is_open(lua_State *L) {
    struct lua_sqlite_stmt *st = (struct lua_sqlite_stmt *)luaL_checkudata(L, 1, SQLITE_STMT_META);
    lua_pushboolean(L, st->stmt != NULL);
    return 1;
}

static int stmt_gc(lua_State *L) {
    struct lua_sqlite_stmt *st = (struct lua_sqlite_stmt *)luaL_checkudata(L, 1, SQLITE_STMT_META);
    finalize_stmt(st);
    return 0;
}

static int stmt_tostring(lua_State *L) {
    struct lua_sqlite_stmt *st = (struct lua_sqlite_stmt *)luaL_checkudata(L, 1, SQLITE_STMT_META);
    lua_pushfstring(L, "<sqlite.stmt (%s)>", st->stmt != NULL ? "open" : "closed");
    return 1;
}

/* ==================== Module Registration ==================== */

void dejavu_lua_sqlite_open(lua_State *L) {
    /* Pre-seed open DB counter with a lightuserdata key so gc/close never allocates registry slots.
     */
    lua_rawgetp(L, LUA_REGISTRYINDEX, &sqlite_open_count_key);
    if (lua_isnil(L, -1)) {
        lua_pushinteger(L, 0);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &sqlite_open_count_key);
    }
    lua_pop(L, 1);

    /* Blob metatable */
    if (luaL_newmetatable(L, SQLITE_BLOB_META)) {
        lua_newtable(L);
        lua_pushcfunction(L, blob_bytes);
        lua_setfield(L, -2, "bytes");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, blob_len);
        lua_setfield(L, -2, "__len");
        lua_pushcfunction(L, blob_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);

    /* Statement methods table (idempotent reuse on repeat open) */
    int stmt_methods_idx = 0;
    if (luaL_newmetatable(L, SQLITE_STMT_META)) {
        lua_newtable(L);
        static const luaL_Reg kStmtMethods[] = {
            {"bind", stmt_bind},       {"reset", stmt_reset},
            {"step", stmt_step},       {"all", stmt_all},
            {"one", stmt_one},         {"run", stmt_run},
            {"columns", stmt_columns}, {"close", stmt_close},
            {"is_open", stmt_is_open}, {NULL, NULL},
        };
        luaL_setfuncs(L, kStmtMethods, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "__index");
        lua_pushcfunction(L, stmt_gc);
        lua_setfield(L, -3, "__gc");
        lua_pushcfunction(L, stmt_gc);
        lua_setfield(L, -3, "__close");
        lua_pushcfunction(L, stmt_tostring);
        lua_setfield(L, -3, "__tostring");
        stmt_methods_idx = lua_gettop(L);
    } else {
        rawget_field(L, -1, "__index");
        stmt_methods_idx = lua_gettop(L);
    }

    /* Database methods table (idempotent reuse on repeat open) */
    int db_methods_idx = 0;
    if (luaL_newmetatable(L, SQLITE_DB_META)) {
        lua_newtable(L);
        static const luaL_Reg kDbMethods[] = {
            {"close", db_close},
            {"is_open", db_is_open},
            {"path", db_path},
            {"exec", db_exec},
            {"query", db_query},
            {"one", db_one},
            {"query_one", db_one},
            {"scalar", db_scalar},
            {"value", db_scalar},
            {"rows", db_rows},
            {"prepare", db_prepare},
            {"transaction", db_transaction},
            {"tx", db_transaction},
            {"changes", db_changes},
            {"total_changes", db_total_changes},
            {"last_insert_rowid", db_last_insert_rowid},
            {"in_transaction", db_in_transaction},
            {"busy_timeout", db_busy_timeout},
            {NULL, NULL},
        };
        luaL_setfuncs(L, kDbMethods, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "__index");
        lua_pushcfunction(L, db_gc);
        lua_setfield(L, -3, "__gc");
        lua_pushcfunction(L, db_close);
        lua_setfield(L, -3, "__close");
        lua_pushcfunction(L, db_tostring);
        lua_setfield(L, -3, "__tostring");
        db_methods_idx = lua_gettop(L);
    } else {
        rawget_field(L, -1, "__index");
        db_methods_idx = lua_gettop(L);
    }

    /* Module table */
    lua_newtable(L);
    lua_pushcfunction(L, lua_sqlite_open);
    lua_setfield(L, -2, "open");
    lua_pushcfunction(L, lua_sqlite_is_db);
    lua_setfield(L, -2, "is_db");
    lua_pushcfunction(L, lua_sqlite_blob);
    lua_setfield(L, -2, "blob");
    lua_pushcfunction(L, lua_sqlite_is_blob);
    lua_setfield(L, -2, "is_blob");
    lua_pushlightuserdata(L, (void *)&sqlite_null_token);
    lua_setfield(L, -2, "NULL");
    lua_pushlightuserdata(L, (void *)&sqlite_null_token);
    lua_setfield(L, -2, "null");
    lua_pushstring(L, sqlite3_libversion());
    lua_setfield(L, -2, "VERSION");
    lua_pushvalue(L, db_methods_idx);
    lua_setfield(L, -2, "_db_methods");
    lua_pushvalue(L, stmt_methods_idx);
    lua_setfield(L, -2, "_stmt_methods");

    lua_setglobal(L, "sqlite");
    lua_pop(L, 4); /* pop db_methods_idx, SQLITE_DB_META, stmt_methods_idx, SQLITE_STMT_META */
}
