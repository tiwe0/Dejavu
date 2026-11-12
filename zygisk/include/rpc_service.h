#pragma once

#include "dejavu_plugin.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

// Owns Unix transport and scheduling; callbacks only implement plugin business logic.
class RpcService {
  public:
    using Invoke = std::function<int(uint64_t, DejavuRpcHandler, void *,
                                     const DejavuRpcRequest *, DejavuRpcResponse *,
                                     char *, size_t)>;
    explicit RpcService(Invoke invoke = {});
    ~RpcService();
    RpcService(const RpcService &) = delete;
    RpcService &operator=(const RpcService &) = delete;

    int register_service(uint64_t owner, const DejavuRpcServiceDescriptor *descriptor,
                         uint64_t *handle, std::string *error);
    int update_routes(uint64_t owner, uint64_t handle, const char *const *routes,
                      size_t route_count, std::string *error);
    int unregister_service(uint64_t owner, uint64_t handle, std::string *error);
    void activate_owner(uint64_t owner);
    // Closes admission and cancels work. False keeps the owner's code mapped for a retry.
    bool retire_owner(uint64_t owner);
    // Final teardown from the owning thread, never from a business callback.
    // Keeps this object valid until all callbacks have returned; idempotent.
    void stop_and_join();

  private:
    struct Service;
    std::shared_ptr<Service> find(uint64_t owner, uint64_t handle);
    Invoke invoke_;
    std::mutex mutex_;
    std::mutex teardown_mutex_;
    bool shutting_down_ = false;
    std::map<uint64_t, std::shared_ptr<Service>> services_;
    std::set<uint64_t> active_owners_;
    std::set<uint64_t> retired_owners_;
    uint64_t next_handle_ = 1;
};
