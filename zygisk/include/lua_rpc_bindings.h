#pragma once

#include "dejavu_runtime.h"
#include "rpc_service.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

constexpr uint64_t kLuaRpcOwnerToken = 0x4C75615F52504300ULL;

// Runtime and session outlive the adapter; transport must outlive shutdown().
class LuaRpcBindings {
  public:
    LuaRpcBindings(DejavuRuntime *runtime, dejavu_control_session *session, RpcService *rpc);
    ~LuaRpcBindings();
    bool initialize(const std::string &bootstrap, std::string *error);
    void activate();
    // Call outside a Lua callback, before destroying the control session.
    void shutdown();

  private:
    struct Service;
    static int host_register(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);
    static int host_update(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t);
    static int host_unregister(dejavu_control_session *, void *, const dejavu_value *, size_t,
                               dejavu_value *, char *, size_t);
    static int host_cancelled(dejavu_control_session *, void *, const dejavu_value *, size_t,
                              dejavu_value *, char *, size_t);
    static int dispatch(void *, const DejavuRpcRequest *, DejavuRpcResponse *, char *, size_t);
    DejavuRuntime *runtime_;
    dejavu_control_session *session_;
    RpcService *rpc_;
    std::mutex mutex_;
    std::map<uint64_t, std::shared_ptr<Service>> services_;
    bool initialized_ = false;
    std::atomic<bool> stopped_{false};
};
