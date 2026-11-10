#pragma once

#include <string>

enum class AgentScriptReadStatus { kLoaded, kMissing, kError };

AgentScriptReadStatus agent_read_script_at(int directory_fd, const char *path, std::string *script,
                                           std::string *error);
AgentScriptReadStatus agent_read_early_hook_at(int directory_fd, const std::string &process,
                                               std::string *script, std::string *error);

struct AgentStartupResult {
    bool initialized = false;
    bool early_hook_failed = false;
    std::string error;
};

// Both callbacks use the same persistent session, but each source is a separate Lua chunk.
// A configured hook cannot prevent an initialized session from serving recovery RPC requests.
template <typename StartControl, typename EvalControl>
AgentStartupResult
agent_start_control_scripts(const std::string &init_script, const std::string &early_script,
                            StartControl start_control, EvalControl eval_control) {
    AgentStartupResult result;
    if (!start_control(init_script, &result.error)) {
        return result;
    }
    result.initialized = true;
    if (!early_script.empty() && !eval_control(early_script, &result.error)) {
        result.early_hook_failed = true;
    }
    return result;
}
