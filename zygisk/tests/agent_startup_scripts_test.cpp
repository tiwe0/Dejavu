#include "agent_startup_scripts.h"
#include "control_channel.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace {

void write_file(int root, const char *path, const std::string &text) {
    const int fd = openat(root, path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    assert(close(fd) == 0);
}

class Session {
  public:
    ~Session() {
        if (state != nullptr) {
            lua_close(state);
        }
    }

    bool start(const std::string &script, std::string *error) {
        assert(state == nullptr);
        stages.push_back("init");
        state = luaL_newstate();
        assert(state != nullptr);
        luaL_openlibs(state);
        return evaluate(script, error);
    }

    bool early(const std::string &script, std::string *error) {
        stages.push_back("early");
        return evaluate(script, error);
    }

    bool evaluate(const std::string &script, std::string *error) {
        error->clear();
        int status = luaL_loadbufferx(state, script.data(), script.size(), "startup-test", "t");
        if (status == LUA_OK) {
            status = lua_pcall(state, 0, 0, 0);
        }
        if (status != LUA_OK) {
            error->assign(lua_tostring(state, -1));
            lua_pop(state, 1);
            return false;
        }
        return true;
    }

    AgentStartupResult initialize(const std::string &init, const std::string &early_script) {
        return agent_start_control_scripts(
            init, early_script,
            [&](const std::string &script, std::string *error) { return start(script, error); },
            [&](const std::string &script, std::string *error) { return early(script, error); });
    }

    std::vector<std::string> stages;
    lua_State *state = nullptr;
};

void test_reads() {
    char directory[] = "/tmp/dejavu-startup-scripts.XXXXXX";
    assert(mkdtemp(directory) != nullptr);
    const int root = open(directory, O_RDONLY | O_DIRECTORY);
    assert(root >= 0);
    assert(mkdirat(root, "config", 0700) == 0);
    std::string script = "stale", error;
    assert(agent_read_early_hook_at(root, "com.example.app", &script, &error) ==
           AgentScriptReadStatus::kMissing);
    assert(script.empty());
    assert(mkdirat(root, "config/early-hooks", 0700) == 0);
    write_file(root, "config/early-hooks/com.example.app.lua", "shared = 'main'");
    write_file(root, "config/early-hooks/com.example.app:worker.lua", "shared = 'worker'");
    assert(agent_read_early_hook_at(root, "com.example.app", &script, &error) ==
           AgentScriptReadStatus::kLoaded);
    assert(script == "shared = 'main'" && error.empty());
    assert(agent_read_early_hook_at(root, "com.example.app:worker", &script, &error) ==
           AgentScriptReadStatus::kLoaded);
    assert(script == "shared = 'worker'");
    for (const char *process : {"com.example", "com.example.app:other"}) {
        assert(agent_read_early_hook_at(root, process, &script, &error) ==
               AgentScriptReadStatus::kMissing);
        assert(script.empty());
    }
    for (const char *process : {"", "../com.example.app", "/com.example.app", "com\\app"}) {
        assert(agent_read_early_hook_at(root, process, &script, &error) ==
               AgentScriptReadStatus::kError);
        assert(!error.empty() && script.empty());
    }
    assert(agent_read_early_hook_at(-1, "com.example.app", &script, &error) ==
           AgentScriptReadStatus::kError);
    assert(!error.empty());

    write_file(root, "config/init.lua", "return true");
    assert(agent_read_script_at(root, "config/init.lua", &script, &error) ==
           AgentScriptReadStatus::kLoaded);
    assert(script == "return true");
    write_file(root, "config/early-hooks/empty.lua", "");
    assert(agent_read_early_hook_at(root, "empty", &script, &error) ==
           AgentScriptReadStatus::kLoaded);
    assert(script.empty());
    write_file(root, "config/early-hooks/binary.lua", std::string("a\0b", 3));
    assert(agent_read_early_hook_at(root, "binary", &script, &error) ==
           AgentScriptReadStatus::kError);
    assert(error.find("NUL") != std::string::npos && script.empty());
    write_file(root, "config/early-hooks/oversized.lua",
               std::string(kDejavuMaximumControlScriptSize + 1, 'x'));
    assert(agent_read_early_hook_at(root, "oversized", &script, &error) ==
           AgentScriptReadStatus::kError);
    assert(!error.empty() && script.empty());
    assert(mkdirat(root, "config/early-hooks/directory.lua", 0700) == 0);
    assert(agent_read_early_hook_at(root, "directory", &script, &error) ==
           AgentScriptReadStatus::kError);
    assert(mkfifoat(root, "config/early-hooks/fifo.lua", 0600) == 0);
    assert(agent_read_early_hook_at(root, "fifo", &script, &error) ==
           AgentScriptReadStatus::kError);
    assert(symlinkat("com.example.app.lua", root, "config/early-hooks/link.lua") == 0);
    assert(agent_read_early_hook_at(root, "link", &script, &error) ==
           AgentScriptReadStatus::kError);
    write_file(root, "config/early-hooks/private.lua", "return true");
    assert(fchmodat(root, "config/early-hooks/private.lua", 0000, 0) == 0);
    if (geteuid() != 0) {
        assert(agent_read_early_hook_at(root, "private", &script, &error) ==
               AgentScriptReadStatus::kError);
        assert(!error.empty());
    }
    for (const char *path :
         {"com.example.app.lua", "com.example.app:worker.lua", "empty.lua", "binary.lua",
          "oversized.lua", "fifo.lua", "link.lua", "private.lua"}) {
        const std::string file = std::string("config/early-hooks/") + path;
        assert(unlinkat(root, file.c_str(), 0) == 0);
    }
    assert(unlinkat(root, "config/early-hooks/directory.lua", AT_REMOVEDIR) == 0);
    assert(unlinkat(root, "config/early-hooks", AT_REMOVEDIR) == 0);
    assert(unlinkat(root, "config/init.lua", 0) == 0);
    assert(unlinkat(root, "config", AT_REMOVEDIR) == 0);
    close(root);
    assert(rmdir(directory) == 0);
}

void test_chunks() {
    {
        Session session;
        const auto result = session.initialize(
            "local private_value = 7; shared_value = 41; order = {'init'}; return true",
            "assert(private_value == nil); assert(shared_value == 41); "
            "shared_value = 42; table.insert(order, 'early'); return 'ready'");
        assert(result.initialized && !result.early_hook_failed && result.error.empty());
        assert(session.stages == std::vector<std::string>({"init", "early"}));
        std::string error;
        assert(session.evaluate("assert(shared_value == 42); "
                                "assert(table.concat(order, ',') == 'init,early')",
                                &error));
    }
    {
        Session session;
        const auto result = session.initialize("normal_startup = true; return true", "");
        assert(result.initialized && !result.early_hook_failed && result.error.empty());
        assert(session.stages == std::vector<std::string>({"init"}));
    }
    for (const char *hook :
         {"this is not valid Lua", "partial_state = 9; error('startup failed')"}) {
        Session session;
        const auto result = session.initialize("normal_startup = true; return true", hook);
        assert(result.initialized && result.early_hook_failed && !result.error.empty());
        std::string error;
        assert(session.evaluate("assert(normal_startup); recovery_rpc = true", &error));
        assert(session.evaluate("assert(recovery_rpc)", &error));
        if (std::string(hook).find("partial_state") != std::string::npos) {
            assert(session.evaluate("assert(partial_state == 9)", &error));
        }
    }
    {
        Session session;
        const auto result = session.initialize("error('init failed')", "must_not_run = true");
        assert(!result.initialized && !result.early_hook_failed && !result.error.empty());
        assert(session.stages == std::vector<std::string>({"init"}));
    }
}

} // namespace

int main() {
    test_reads();
    test_chunks();
    puts("OK: startup script selection, bounded reads, isolated Lua chunks and recovery");
}
