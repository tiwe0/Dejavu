#include "dejavu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file_all(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "unable to open %s\n", path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_len != NULL) {
        *out_len = n;
    }
    return buf;
}

/* Simple in-memory mock filesystem for testing xml.load / xml.save / xml.prefs(path). */
struct mock_fs {
    char path[256];
    char *content;
    size_t len;
};

static int mock_fs_write(dejavu_control_session *session, void *context,
                         const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                         char *error, size_t error_size) {
    (void)session;
    struct mock_fs *fs = (struct mock_fs *)context;
    if (argument_count != 2 || arguments[0].type != DEJAVU_VALUE_STRING ||
        arguments[1].type != DEJAVU_VALUE_STRING) {
        snprintf(error, error_size, "fs.write expects (path, content)");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    snprintf(fs->path, sizeof(fs->path), "%.*s", (int)arguments[0].string_size,
             arguments[0].string_value);
    free(fs->content);
    fs->content = (char *)malloc(arguments[1].string_size + 1);
    if (fs->content == NULL) {
        snprintf(error, error_size, "out of memory");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(fs->content, arguments[1].string_value, arguments[1].string_size);
    fs->content[arguments[1].string_size] = '\0';
    fs->len = arguments[1].string_size;
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = 1;
    return DEJAVU_OK;
}

static int mock_fs_read(dejavu_control_session *session, void *context,
                        const dejavu_value *arguments, size_t argument_count, dejavu_value *result,
                        char *error, size_t error_size) {
    (void)session;
    struct mock_fs *fs = (struct mock_fs *)context;
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_STRING) {
        snprintf(error, error_size, "fs.read expects (path)");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (fs->content == NULL || strlen(fs->path) != arguments[0].string_size ||
        memcmp(fs->path, arguments[0].string_value, arguments[0].string_size) != 0) {
        snprintf(error, error_size, "mock file not found");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    char *copy = (char *)malloc(fs->len + 1);
    if (copy == NULL) {
        snprintf(error, error_size, "out of memory");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, fs->content, fs->len + 1);
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = fs->len;
    return DEJAVU_OK;
}

static void run_lua_or_die(dejavu_control_session *session, const char *script, const char *label) {
    dejavu_value result;
    char error[512];
    memset(&result, 0, sizeof(result));
    int rc = dejavu_control_session_eval(session, script, &result, error, sizeof(error));
    if (rc != DEJAVU_OK) {
        fprintf(stderr, "FAIL [%s]: %s\n", label, error);
        exit(1);
    }
    dejavu_value_release(&result);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <resources.lua> <temp-db-path>\n", argv[0]);
        return 2;
    }
    const char *resources_path = argv[1];
    const char *temp_db_path = argv[2];

    dejavu_engine *engine = dejavu_engine_create();
    if (engine == NULL) {
        fprintf(stderr, "dejavu_engine_create failed\n");
        return 1;
    }
    dejavu_control_session *session = NULL;
    char error[512];
    if (dejavu_control_session_create(engine, &session, error, sizeof(error)) != DEJAVU_OK) {
        fprintf(stderr, "dejavu_control_session_create failed: %s\n", error);
        dejavu_engine_destroy(engine);
        return 1;
    }

    struct mock_fs fs;
    memset(&fs, 0, sizeof(fs));
    if (dejavu_control_session_register(session, "fs", "read", mock_fs_read, &fs, error,
                                        sizeof(error)) != DEJAVU_OK ||
        dejavu_control_session_register(session, "fs", "write", mock_fs_write, &fs, error,
                                        sizeof(error)) != DEJAVU_OK) {
        fprintf(stderr, "register mock fs failed: %s\n", error);
        return 1;
    }

    char *resources_src = read_file_all(resources_path, NULL);
    if (resources_src == NULL) {
        return 1;
    }
    run_lua_or_die(session, resources_src, "load resources.lua");
    free(resources_src);

    /* Expose temp db path to Lua */
    char set_path_script[512];
    snprintf(set_path_script, sizeof(set_path_script), "TEST_DB_PATH = [[%s]]", temp_db_path);
    run_lua_or_die(session, set_path_script, "set TEST_DB_PATH");

    /* 1) XML C parser/encoder + Lua sugar + Android SharedPreferences */
    static const char *kXmlTests =
        "assert(type(xml) == 'table')\n"
        "local raw = '\\239\\187\\191<?xml version=\"1.0\" encoding=\"utf-8\"?>\\r\\n'\n"
        "  .. '<!DOCTYPE manifest [<!ENTITY xxe \"blocked\">]>\\n'\n"
        "  .. '<!-- top comment -->\\n'\n"
        "  .. '<manifest package=\"com.demo.app\" "
        "xmlns:android=\"http://schemas.android.com/apk/res/android\">\\n'\n"
        "  .. '  <application android:label=\"A &amp; B &lt;1&gt; &#65; &#x4E2D;\">\\n'\n"
        "  .. '    <meta-data android:name=\"api/url\" "
        "android:value=\"https://example.test?a=1&amp;b=2\"/>\\n'\n"
        "  .. '    <meta-data android:name=\"second\" android:value=\"v2\"/>\\n'\n"
        "  .. '    <note><![CDATA[raw <tag> & text]]></note>\\n'\n"
        "  .. '    <spaces>   </spaces>\\n'\n"
        "  .. '  </application>\\n'\n"
        "  .. '</manifest><!-- trailing -->'\n"
        "local root = xml.parse(raw)\n"
        "assert(xml.is_node(root) == true and xml.is_node({}) == false)\n"
        "assert(root.tag == 'manifest' and root:attr('package') == 'com.demo.app')\n"
        "assert(root:attr('missing', 'def') == 'def')\n"
        "local app = root:child('application')\n"
        "assert(app:attr('android:label') == 'A & B <1> A 中')\n"
        "assert(app:child('note'):text() == 'raw <tag> & text')\n"
        "assert(app:child('spaces'):text() == '   ', 'leaf whitespace must be preserved')\n"
        "local md1 = root:find(\"application/meta-data[@android:name='api/url']\")\n"
        "assert(md1 ~= nil and md1:attr('android:value') == 'https://example.test?a=1&b=2')\n"
        "local md2 = root:find('//meta-data[2]')\n"
        "assert(md2 ~= nil and md2:attr('android:name') == 'second')\n"
        "assert(#root:find_all('//meta-data[@android:value]') == 2)\n"
        "assert(#root:find_all('*/note') == 1)\n"
        "-- Mutation, serialization, round-trip, escape/unescape\n"
        "local extra = xml.node('item', { id = '3' }, 'hello <world>')\n"
        "extra:set_attr('flag', true):set_text('updated & ok')\n"
        "app:add(extra)\n"
        "assert(app:find(\"item[@id='3']\"):text() == 'updated & ok')\n"
        "local encoded = root:to_xml({ declaration = true, indent = '  ' })\n"
        "local reparsed = xml.parse(encoded)\n"
        "assert(reparsed:find(\"//item[@id='3']\"):text() == 'updated & ok')\n"
        "assert(reparsed:find(\"//item[@id='3']\"):attr('flag') == 'true')\n"
        "assert(app:remove(extra) == extra and app:find(\"item[@id='3']\") == nil)\n"
        "assert(xml.unescape(xml.escape('<a & \"b\" \\'c\\'>')) == '<a & \"b\" \\'c\\'>')\n"
        "local plain = root:to_table()\n"
        "assert(plain.tag == 'manifest' and plain.attrs.package == 'com.demo.app')\n"
        "-- Strict error checks (XXE entity, mismatch, dup attr, -- in comment, &#0;, trailing "
        "junk)\n"
        "assert(not pcall(xml.parse, '<r>&xxe;</r>'))\n"
        "assert(not pcall(xml.parse, '<a></b>'))\n"
        "assert(not pcall(xml.parse, '<a x=\"1\" x=\"2\"/>'))\n"
        "assert(not pcall(xml.parse, '<!-- bad -- comment --><a/>'))\n"
        "assert(not pcall(xml.parse, '<a/><b></b>'))\n"
        "assert(not pcall(xml.parse, '<a>&#0;</a>'))\n"
        "assert(not pcall(xml.parse, '<a>&#xD800;</a>'))\n"
        "local cyc = xml.node('a'); cyc:add(cyc)\n"
        "assert(not pcall(xml.encode, cyc))\n"
        "-- Android SharedPreferences sugar + file save/load\n"
        "local prefs_in = {\n"
        "  token = 'abc<1>&2',\n"
        "  count = 42,\n"
        "  big = 9007199254740000,\n"
        "  ratio = 1.5,\n"
        "  enabled = true,\n"
        "  disabled = false,\n"
        "  tags = { 'alpha', 'beta' },\n"
        "}\n"
        "local prefs_xml = xml.encode_prefs(prefs_in)\n"
        "local p1 = xml.prefs(prefs_xml)\n"
        "assert(p1.token == 'abc<1>&2' and p1.count == 42 and p1.big == 9007199254740000)\n"
        "assert(p1.ratio == 1.5 and p1.enabled == true and p1.disabled == false)\n"
        "assert(#p1.tags == 2 and p1.tags[1] == 'alpha' and p1.tags[2] == 'beta')\n"
        "xml.save_prefs('/mock/prefs.xml', prefs_in)\n"
        "local p2 = xml.prefs('/mock/prefs.xml')\n"
        "assert(p2.token == 'abc<1>&2' and p2.count == 42)\n"
        "assert(xml.load('/mock/prefs.xml').tag == 'map')\n"
        "-- Hardened XML C & Lua regression tests\n"
        "-- 1. Indent injection rejected + fresh indent string + evil __index ignored\n"
        "assert(not pcall(xml.encode, root, { indent = '<bad/>' }))\n"
        "local evil_opts = setmetatable({}, { __index = function() error('must not run') end })\n"
        "local single_child_node = xml.node('r', nil, xml.node('c', nil, 'v'))\n"
        "assert(xml.encode(single_child_node, evil_opts) == '<r><c>v</c></r>')\n"
        "local dyn_xml = xml.encode(single_child_node, { indent = string.rep(' ', 2) })\n"
        "collectgarbage('collect')\n"
        "assert(dyn_xml:find('  <c>v</c>', 1, true), dyn_xml)\n"
        "-- 2. Attribute key rooting under GC pressure + non-string attr formatting\n"
        "local many_attrs = {}\n"
        "for i = 1, 20 do many_attrs['k' .. tostring(i)] = i end\n"
        "many_attrs.flag = true\n"
        "local many_node = xml.node('m', many_attrs)\n"
        "local many_out = xml.encode(many_node)\n"
        "collectgarbage('collect')\n"
        "local many_back = xml.parse(many_out)\n"
        "assert(many_back:attr('k1') == '1' and many_back:attr('k20') == '20' and "
        "many_back:attr('flag') == 'true')\n"
        "-- 3. Cyclic node:text(), find(), to_table() must error/terminate cleanly\n"
        "assert(not pcall(function() return cyc:text() end))\n"
        "assert(cyc:find('//missing') == nil)\n"
        "assert(not pcall(function() return cyc:to_table() end))\n"
        "-- 4. DOCTYPE with comment containing ]> + strict whitespace after DOCTYPE\n"
        "local dt_doc = xml.parse('<!DOCTYPE a [ <!-- ] > --> ]><a/>')\n"
        "assert(dt_doc.tag == 'a')\n"
        "assert(not pcall(xml.parse, '<!DOCTYPEa><a/>'))\n"
        "-- 5. Mixed-content pretty-print preserves exact inline text\n"
        "local mixed = xml.parse('<p>Hello <b>world</b>!</p>')\n"
        "local mixed_enc = xml.encode(mixed, { indent = '  ' })\n"
        "assert(mixed_enc == '<p>Hello <b>world</b>!</p>', mixed_enc)\n"
        "assert(xml.parse(mixed_enc):text() == 'Hello world!')\n"
        "-- 6. Illegal XML 1.0 control chars rejected on parse & encode\n"
        "assert(not pcall(xml.parse, '<a>\\1</a>'))\n"
        "assert(not pcall(xml.encode, xml.node('a', nil, '\\1')))\n"
        "assert(not pcall(xml.escape, '\\1'))\n"
        "-- 7. CDATA EOL normalization + attribute whitespace normalization & round-trip\n"
        "assert(xml.parse('<n><![CDATA[a\\r\\nb\\rc]]></n>'):text() == 'a\\nb\\nc')\n"
        "assert(xml.parse('<a k=\"x\\ty\\nz\"/>'):attr('k') == 'x y z')\n"
        "local nl_attr = xml.encode(xml.node('a', { k = 'line1\\nline2\\tend' }))\n"
        "assert(nl_attr:find('&#xA;', 1, true) and nl_attr:find('&#x9;', 1, true), nl_attr)\n"
        "assert(xml.parse(nl_attr):attr('k') == 'line1\\nline2\\tend')\n"
        "-- 8. SharedPreferences NaN/Inf + strict <set> array validation\n"
        "local sp_edge = xml.encode_prefs({ inf = math.huge, ninf = -math.huge, nan = 0/0 })\n"
        "local sp_back = xml.prefs(sp_edge)\n"
        "assert(sp_back.inf == math.huge and sp_back.ninf == -math.huge and sp_back.nan ~= "
        "sp_back.nan)\n"
        "assert(not pcall(xml.encode_prefs, { bad = { a = '1' } }))\n"
        "assert(not pcall(xml.encode_prefs, { bad = { [1] = 'a', [3] = 'b' } }))\n"
        "assert(not pcall(xml.encode_prefs, { bad = { 1, 2 } }))\n"
        "-- 9. Selector whitespace + strict syntax + remove bounds + set_attr validation\n"
        "assert(root:find(\"application/meta-data[ @android:name = 'second' ]\") ~= nil)\n"
        "assert(not pcall(function() root:find('a[1][2]') end))\n"
        "assert(not pcall(function() root:find('a[1]b') end))\n"
        "assert(extra:remove(99) == nil and extra:remove(0) == nil)\n"
        "assert(not pcall(function() extra:set_attr('x', {}) end))\n"
        "assert(not pcall(function() extra:set_attr('x', math.huge) end))\n";
    run_lua_or_die(session, kXmlTests, "XML core + sugar + prefs");

    /* 2) SQLite C layer + Lua CRUD/tx/scope sugar + file RO/RW + REPL completion */
    static const char *kSqliteTests =
        "assert(type(sqlite) == 'table' and type(sqlite.VERSION) == 'string')\n"
        "-- In-memory DB with <close>, CRUD sugar, blobs, named/positional params, nested "
        "savepoints\n"
        "do\n"
        "  local db <close> = sqlite.memory({ foreign_keys = true })\n"
        "  assert(sqlite.is_db(db) and db:is_open())\n"
        "  assert(db:pragma('foreign_keys') == 1)\n"
        "  -- PRAGMA setter + validation\n"
        "  assert(db:pragma('cache_size', -2000) == -2000)\n"
        "  assert(not pcall(function() db:pragma('cache_size', math.huge) end))\n"
        "  assert(not pcall(function() db:pragma('journal_mode', 'WAL\\0;DROP') end))\n"
        "  -- TEMP tables work (SQLITE_TEMP_STORE=3 in-memory temp store)\n"
        "  db:exec('CREATE TEMP TABLE tmp_check (v INTEGER); INSERT INTO tmp_check VALUES (7);')\n"
        "  assert(db:scalar('SELECT v FROM tmp_check') == 7)\n"
        "  db:exec([[\n"
        "    CREATE TABLE users (\n"
        "      id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "      name TEXT NOT NULL UNIQUE,\n"
        "      age INTEGER,\n"
        "      score REAL,\n"
        "      raw BLOB,\n"
        "      note TEXT\n"
        "    );\n"
        "  ]])\n"
        "  local tbls = db:tables()\n"
        "  assert(#tbls == 1 and tbls[1] == 'users')\n"
        "  assert(#db:columns('users') == 6)\n"
        "  assert(db:schema('users'):find('CREATE TABLE users', 1, true))\n"
        "  assert(db:schema().users:find('CREATE TABLE users', 1, true))\n"
        "\n"
        "  -- Single insert + binary blob with embedded NULs + int64\n"
        "  local bin = '\\0\\1\\255\\0hello'\n"
        "  local bobj = sqlite.blob(bin)\n"
        "  assert(sqlite.is_blob(bobj) and #bobj == #bin and bobj:bytes() == bin)\n"
        "  local id1 = db:insert('users', { name = 'alice', age = 30, score = 9.5, raw = "
        "bobj, note = sqlite.NULL })\n"
        "  assert(id1 == 1)\n"
        "  -- Batch insert + strict NUL-key / sparse batch validation\n"
        "  local n_ins = db:insert('users', {\n"
        "    { name = 'bob', age = 25, score = 7.25, note = 'hi' },\n"
        "    { name = 'carol', age = 40, score = 8.0, note = json.null },\n"
        "  })\n"
        "  assert(n_ins == 2 and db:count('users') == 3)\n"
        "  assert(db:count('users', { note = sqlite.NULL }) == 2)\n"
        "  assert(not pcall(function() db:insert('users', { ['a\\0b'] = 1 }) end))\n"
        "  assert(not pcall(function() db:insert('users', { [1] = { name = 'x' }, [3] = { name = "
        "'y' } }) end))\n"
        "\n"
        "  -- Verify binary blob preservation, trailing comments, multi-stmt rejection, NUL "
        "rejection\n"
        "  local alice = db:one('SELECT * FROM users WHERE name = :name; -- trailing comment', "
        "{ name = 'alice' })\n"
        "  assert(alice ~= nil and alice.raw == bin and #alice.raw == #bin and alice.note == nil)\n"
        "  assert(not pcall(function() db:query('SELECT 1; SELECT 2') end))\n"
        "  assert(not pcall(function() db:exec('SELECT 1\\0SELECT 2') end))\n"
        "  local alice_nulls = db:one('SELECT name, note FROM users WHERE name = @n', { n = "
        "'alice' }, { null = sqlite.NULL })\n"
        "  assert(alice_nulls.note == sqlite.NULL)\n"
        "  local arr_rows = db:query('SELECT id, name FROM users ORDER BY id', nil, { mode = "
        "'array' })\n"
        "  assert(#arr_rows == 3 and arr_rows[1][1] == 1 and arr_rows[1][2] == 'alice')\n"
        "\n"
        "  -- Named params & nil-hole positional params in CRUD sugar\n"
        "  assert(db:count('users', 'name = :n', { n = 'alice' }) == 1)\n"
        "  assert(db:count('users', 'note IS ? AND name = ?', { [2] = 'alice' }) == 1)\n"
        "  assert(db:first('users', 'name = :n', { params = { n = 'bob' } }).age == 25)\n"
        "  assert(db:update('users', { age = 32 }, 'name = :n', { n = 'alice' }) == 1)\n"
        "  assert(db:first('users', { name = 'alice' }).age == 32)\n"
        "  assert(db:update('users', { age = 31 }, 'note IS ? AND name = ?', { [2] = 'alice' }) "
        "== 1)\n"
        "  assert(db:first('users', { name = 'alice' }).age == 31)\n"
        "  assert(not pcall(function() db:update('users', { age = 1 }, 'name = ?1', { 'alice' }) "
        "end))\n"
        "  assert(not pcall(function() db:find('users', 'name = ?', { 'alice' }) end))\n"
        "  assert(not pcall(function() db:select('users', { cols = {} }) end))\n"
        "  assert(not pcall(function() db:select('users', { cols = 123 }) end))\n"
        "  assert(not pcall(function() db:delete('users', '1=1; DROP TABLE users') end))\n"
        "\n"
        "  -- Upsert, update, delete safety, find, first, tuple ORDER BY\n"
        "  db:upsert('users', { name = 'bob', age = 26, score = 8.5, note = 'updated' }, 'name')\n"
        "  local bob = db:first('users', { name = 'bob' })\n"
        "  assert(bob.age == 26 and bob.note == 'updated')\n"
        "  assert(not pcall(function() db:update('users', { age = 1 }) end), 'update without where "
        "must fail')\n"
        "  assert(not pcall(function() db:delete('users') end), 'delete without where must fail')\n"
        "  local top2 = db:find('users', nil, { cols = { 'name', 'age' }, order = { { 'age', "
        "'DESC' } }, limit = 2 })\n"
        "  assert(#top2 == 2 and top2[1].name == 'carol' and top2[2].name == 'alice')\n"
        "\n"
        "  -- Streaming Lua 5.4 <close> iterator with early break + max_rows cap\n"
        "  local seen = {}\n"
        "  for row in db:rows('SELECT name FROM users ORDER BY id') do\n"
        "    seen[#seen + 1] = row.name\n"
        "    if #seen == 2 then break end\n"
        "  end\n"
        "  assert(#seen == 2 and seen[1] == 'alice' and seen[2] == 'bob')\n"
        "  local ok_cap = pcall(function()\n"
        "    for _ in db:rows('SELECT name FROM users', nil, { max_rows = 2 }) do end\n"
        "  end)\n"
        "  assert(not ok_cap, 'db:rows must enforce max_rows')\n"
        "\n"
        "  -- Prepared statement reuse + <close> + evil opts.__index safety (no SIGSEGV/UAF)\n"
        "  do\n"
        "    local st <close> = db:prepare('SELECT name FROM users WHERE age >= ? ORDER BY age')\n"
        "    local cols = st:columns()\n"
        "    assert(#cols == 1 and cols[1] == 'name')\n"
        "    local r30 = st:all({ 30 })\n"
        "    assert(#r30 == 2 and r30[1].name == 'alice' and r30[2].name == 'carol')\n"
        "    local r40 = st:one({ 40 })\n"
        "    assert(r40.name == 'carol')\n"
        "    local evil_opts = setmetatable({}, { __index = function() st:close(); return 'array' "
        "end })\n"
        "    local r_step = st:step(evil_opts)\n"
        "    assert(r_step == nil or type(r_step) == 'table')\n"
        "    assert(st:is_open() == true, 'rawget must bypass evil __index')\n"
        "  end\n"
        "  -- Evil json metatable must not fire inside is_null_sentinel\n"
        "  do\n"
        "    local saved_json = _G.json\n"
        "    _G.json = setmetatable({}, { __index = function() error('must not run') end })\n"
        "    assert(db:scalar('SELECT ?', { 'ok' }) == 'ok')\n"
        "    _G.json = saved_json\n"
        "  end\n"
        "\n"
        "  -- Nested transactions via SAVEPOINT + invalid nested mode + closing db inside tx\n"
        "  local tx_ret = db:tx(function()\n"
        "    assert(db:in_transaction() == true)\n"
        "    assert(not pcall(function() db:tx('bogus', function() end) end))\n"
        "    db:insert('users', { name = 'dave', age = 20 })\n"
        "    local ok_inner = pcall(function()\n"
        "      db:tx(function()\n"
        "        db:insert('users', { name = 'eve', age = 22 })\n"
        "        error('rollback inner only')\n"
        "      end)\n"
        "    end)\n"
        "    assert(ok_inner == false)\n"
        "    return 'committed'\n"
        "  end)\n"
        "  assert(tx_ret == 'committed' and db:in_transaction() == false)\n"
        "  assert(db:first('users', { name = 'dave' }) ~= nil)\n"
        "  assert(db:first('users', { name = 'eve' }) == nil)\n"
        "  assert(db:delete('users', 'name = :n', { n = 'dave' }) == 1)\n"
        "  assert(db:first('users', { name = 'dave' }) == nil)\n"
        "\n"
        "  -- Closing db inside tx callback must raise an error\n"
        "  local tx_close_db = sqlite.memory()\n"
        "  assert(not pcall(function() tx_close_db:tx(function() tx_close_db:close() end) end))\n"
        "\n"
        "  -- Closing db with an open child stmt must finalize cleanly\n"
        "  local dangling = db:prepare('SELECT 1')\n"
        "  db:close()\n"
        "  assert(not db:is_open())\n"
        "  assert(not pcall(function() dangling:step() end))\n"
        "end\n"
        "\n"
        "-- Real file persistence + sqlite.with + sqlite.open_ro + dejavu.scope():track_sqlite\n"
        "local written = sqlite.with(TEST_DB_PATH, { wal = true }, function(db)\n"
        "  db:exec('DROP TABLE IF EXISTS kv; CREATE TABLE kv (k TEXT PRIMARY KEY, v TEXT);')\n"
        "  db:upsert('kv', { k = 'greeting', v = '你好' }, 'k')\n"
        "  return db:scalar('SELECT v FROM kv WHERE k = ?', { 'greeting' })\n"
        "end)\n"
        "assert(written == '你好')\n"
        "local ro_ref\n"
        "do\n"
        "  local sc <close> = dejavu.scope()\n"
        "  local ro = sc:track_sqlite(sqlite.open_ro(TEST_DB_PATH))\n"
        "  ro_ref = ro\n"
        "  assert(ro:scalar('SELECT v FROM kv WHERE k = ?', { 'greeting' }) == '你好')\n"
        "  assert(not pcall(function() ro:insert('kv', { k = 'x', v = 'y' }) end), 'read-only db "
        "must reject writes')\n"
        "  _G._test_ro_db = ro\n"
        "  _G._test_xml_node = xml.parse('<a k=\"1\"/>')\n"
        "  -- Live REPL Tab completion on userdata (sqlite.db) and xml.node (methods only on ':')\n"
        "  local c_db = dejavu._repl_complete('_test_ro_db:ins')\n"
        "  assert(c_db:find('\"_test_ro_db:insert\"', 1, true), c_db)\n"
        "  local c_xml = dejavu._repl_complete('_test_xml_node:fi')\n"
        "  assert(c_xml:find('\"_test_xml_node:find\"', 1, true) and "
        "c_xml:find('\"_test_xml_node:find_all\"', 1, true), c_xml)\n"
        "  local c_xml_no_field = dejavu._repl_complete('_test_xml_node:ta')\n"
        "  assert(not c_xml_no_field:find('\"_test_xml_node:tag\"', 1, true), c_xml_no_field)\n"
        "  local c_sq = dejavu._repl_complete('sqlite.o')\n"
        "  assert(c_sq:find('\"sqlite.open\"', 1, true) and c_sq:find('\"sqlite.open_ro\"', 1, "
        "true), c_sq)\n"
        "  local c_sq_no_internal = dejavu._repl_complete('sqlite.')\n"
        "  assert(not c_sq_no_internal:find('_db_methods', 1, true), c_sq_no_internal)\n"
        "  _G._test_ro_db = nil\n"
        "  _G._test_xml_node = nil\n"
        "end\n"
        "assert(not ro_ref:is_open(), 'scope:track_sqlite must close db on scope exit')\n";
    run_lua_or_die(session, kSqliteTests, "SQLite core + sugar + REPL completion");

    free(fs.content);
    dejavu_control_session_destroy(session);
    dejavu_engine_destroy(engine);
    printf("OK: Lua XML parser/encoder/prefs and SQLite read/write/tx/sugar/REPL tests passed\n");
    return 0;
}
