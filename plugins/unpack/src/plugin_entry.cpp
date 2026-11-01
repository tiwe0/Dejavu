#include "dejavu_plugin.h"

#include <string>

#include "unpack_manager.h"

namespace {

int plugin_initialize(const DejavuPluginHost *host, void **plugin_state, char *error,
                      size_t error_size) {
    if (host == nullptr || plugin_state == nullptr) {
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
        DEJAVU_PLUGIN_ABI_VERSION, "unpack", "1.0.0", plugin_initialize, plugin_shutdown,
    };
    return &descriptor;
}
