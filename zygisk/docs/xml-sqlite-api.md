# Lua XML & SQLite API

Dejavu embeds zero-dependency C11 **XML** and **SQLite 3.54.1** bindings directly
into `libdejavu.so`, enhanced with ergonomic Lua sugar in `resources.lua`. Both
work identically in Android target processes (without relying on private platform
`libsqlite.so`), the interactive CLI REPL, and host tests.

---

## `xml` Module

### Parsing, Encoding & Escaping

```lua
local root = xml.parse(text [, { preserve_whitespace = false }]) -- alias: xml.decode
local out  = xml.encode(root [, { indent = "  ", declaration = true }]) -- alias: xml.stringify
local safe = xml.escape('<a & "b">')
local raw  = xml.unescape('&lt;a &amp; &quot;b&quot;&gt;')
local node = xml.node("item", { id = "1" }, "hello")
xml.is_node(node) -- true
```

Security & limits:
- Strict UTF-8 validation (rejects embedded NUL, invalid UTF-8, `&#0;`, UTF-16 surrogates, duplicate attributes, mismatched tags).
- `<!DOCTYPE ...>` is skipped safely with **zero external entity expansion (XXE-free)**.
- Bounded limits: 4 MiB input/output, max nesting depth 64, max 65,536 nodes, cycle detection on `xml.encode`.

### Node Methods & XPath-lite Selectors

Every parsed or constructed node has `{ tag, attrs, children }` plus chainable methods:

```lua
node:text()                      -- concatenated descendant text
node:set_text("new text")        -- replace children with text (chainable)
node:attr("name" [, default])    -- read attribute or default
node:set_attr("k", "v")          -- set/remove (nil) attribute (chainable)
node:elements([tag])             -- direct child element array
node:child([tag])                -- first direct child element
node:add(child_or_string)        -- append child (chainable)
node:remove(child_or_index)      -- remove child by reference or index
node:to_xml([opts])              -- shorthand for xml.encode(node, opts)
node:to_table()                  -- plain Lua table snapshot (JSON-friendly)

-- XPath-lite selectors (first match / all matches)
local md = root:find("application/meta-data[@android:name='API_KEY']")
local all = root:find_all("//string[@name]")
local second = root:find("//items/item[2]")
```

### File & Android SharedPreferences Sugar

File helpers (`xml.load`, `xml.save`, `xml.save_prefs`, and `xml.prefs(path)`) use the
agent's `fs.read` / `fs.write` helpers in target processes. `xml.prefs` also accepts a raw
XML string directly, and `xml.encode_prefs` returns the serialized `<map>` XML without touching
the filesystem.

```lua
local doc = xml.load("/data/data/com.example/files/config.xml") -- alias: xml.parse_file
xml.save("/data/data/com.example/files/config.xml", doc, { indent = "  ", declaration = true }) -- alias: xml.write_file

-- Read Android SharedPreferences (<map>...</map>) from a file path or raw XML string
local prefs = xml.prefs("/data/data/com.example/shared_prefs/app.xml")
local prefs_from_str = xml.prefs('<map><string name="k">v</string></map>')
print(prefs.token, prefs.logged_in, prefs.user_id)

-- Serialize or write back
local raw_xml = xml.encode_prefs(prefs)
prefs.logged_in = true
xml.save_prefs("/data/data/com.example/shared_prefs/app.xml", prefs)
```

---

## `sqlite` Module

### Opening, Lifetimes & Resource Safety

SQLite handles support Lua 5.4 `<close>`, `sqlite.with(...)`, and `dejavu.scope():track_sqlite(db)`:

```lua
-- Open read-only (safe for inspecting live Android app databases)
local db <close> = sqlite.open_ro("/data/data/com.example/databases/app.db")

-- Open read-write/create with options
local db2 <close> = sqlite.open("/data/data/com.example/databases/custom.db", {
    mode = "rwc",         -- "ro" | "rw" | "rwc" (default)
    busy_timeout = 5000,  -- default 5000 ms
    foreign_keys = true,
    wal = true,
})

-- In-memory database or scoped callback
local mem <close> = sqlite.memory()
local count = sqlite.with("/data/data/com.example/databases/app.db", "ro", function(d)
    return d:count("messages")
end)
```

### Raw Parameterized Queries, Streaming & Transactions

Both positional (`?`, `?1`) and named (`:name`, `@name`, `$name`) parameters are supported.
Use `sqlite.blob(str)` to bind binary data as `SQLITE_BLOB` (preserving embedded NUL bytes;
inspect with `sqlite.is_blob(b)`, `b:bytes()`, `#b`) and `sqlite.NULL` / `sqlite.null` (or
`nil` / `json.null`) for SQL `NULL`.

```lua
db:exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT UNIQUE, raw BLOB)")
local changes, rowid = db:exec("INSERT INTO users (name, raw) VALUES (:n, :b)", {
    n = "alice",
    b = sqlite.blob("\0\1\2\3"),
})

local rows = db:query("SELECT * FROM users WHERE id > ?", { 0 })
local one  = db:one("SELECT * FROM users WHERE name = :n", { n = "alice" })
local n    = db:scalar("SELECT COUNT(*) FROM users")

-- Streaming Lua 5.4 <close> iterator (safe on early break or error)
for row in db:rows("SELECT id, name FROM users") do
    print(row.id, row.name)
    if row.id == 10 then break end
end

-- Prepared statements for hot loops
local stmt <close> = db:prepare("INSERT INTO users (name) VALUES (?)")
stmt:run({ "bob" })

-- Transactions (automatically uses SAVEPOINT when nested)
db:tx(function()
    db:insert("users", { name = "carol" })
    db:tx(function()
        db:insert("users", { name = "dave" })
    end)
end)
```

### CRUD & Introspection Sugar

All values are strictly bound as parameters and all table/column identifiers are safely quoted:

```lua
db:tables()                                -- { "users", ... }
db:columns("users")                        -- PRAGMA table_info("users")
db:schema("users")                         -- CREATE TABLE SQL (or map of all tables)
db:pragma("journal_mode")                  -- getter / setter

local rowid = db:insert("users", { name = "alice", age = 30 })
local count = db:insert("users", { { name = "b" }, { name = "c" } }) -- batched in tx
db:upsert("users", { name = "alice", age = 31 }, "name")
db:update("users", { age = 32 }, { name = "alice" })
db:update("users", { age = 33 }, "name = :n", { n = "alice" }) -- named or positional where_params
db:delete("users", { name = "bob" })       -- requires explicit WHERE (or true for all)

local rows = db:select("users", {
    cols = { "id", "name" },
    where = "age >= :min",
    params = { min = 18 },
    order = "id DESC",                     -- or { { "column name", "DESC" } }
    limit = 20,
    offset = 0,
})
local filtered = db:find("users", { active = 1 }, { order = "id DESC", limit = 20 })
local first = db:first("users", { name = "alice" })
local total = db:count("users", { active = 1 })
```
