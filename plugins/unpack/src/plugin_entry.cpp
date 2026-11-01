#include "dejavu_plugin.h"

#include <cstddef>
#include <cstdio>
#include <string>

#include "unpack_manager.h"

namespace {

int plugin_initialize(const DejavuPluginHost *host, void **plugin_state, char *error,
                      size_t error_size) {
    if (host == nullptr || plugin_state == nullptr) {
        return -1;
    }
    if (host->abi_version != DEJAVU_PLUGIN_ABI_VERSION ||
        host->struct_size <
            offsetof(DejavuPluginHost, get_interface) + sizeof(host->get_interface)) {
        std::snprintf(error, error_size, "incompatible plugin host ABI");
        return -1;
    }
    auto *manager = new UnpackManager();
    std::string message;
    if (!manager->initialize(host, &message)) {
        std::snprintf(error, error_size, "%s", message.c_str());
        delete manager;
        return -1;
    }
    if (!manager->register_plugin_functions(host, &message)) {
        std::snprintf(error, error_size, "%s", message.c_str());
        manager->shutdown();
        delete manager;
        return -1;
    }
    manager->enable_from_system_property();
    *plugin_state = manager;
    return 0;
}

void plugin_shutdown(void *plugin_state) {
    auto *manager = static_cast<UnpackManager *>(plugin_state);
    if (manager != nullptr) {
        manager->shutdown();
        delete manager;
    }
}

} // namespace

extern "C" __attribute__((visibility("default"))) const DejavuPluginDescriptor *
dejavu_plugin_entry(void) {
    static DejavuPluginDescriptor descriptor = {
        DEJAVU_PLUGIN_ABI_VERSION,
        sizeof(DejavuPluginDescriptor),
        "unpack",
        "1.0.0",
        0, // not unload-safe: this example does not remove its native hooks
        plugin_initialize,
        nullptr,
        plugin_shutdown,
    };
    return &descriptor;
}
