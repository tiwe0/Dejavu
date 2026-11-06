#include "lsplant_backend.h"

#include <android/log.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <dobby.h>
#include <lsplant.hpp>

#include "aarch64_adrp_add.h"
#include "art_elf_resolver.h"
#include "root_helper_client.h"

namespace {

constexpr char kLogTag[] = "DejavuLSPlant";
constexpr uint32_t kBackendAbiVersion = 1;

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

std::mutex g_initialize_mutex;
bool g_initialize_finished = false;
DejavuLsplantInitResult g_initialize_result = {
    DejavuLsplantInitStatus::kInvalidArgument,
    false,
    nullptr,
};

void *resolve_interpreter_bridge(const ArtElfResolver &resolver) {
    constexpr std::string_view kOracleSymbol =
        "_ZNK3art11ClassLinker26IsQuickToInterpreterBridgeEPKv";
    constexpr size_t kOracleInstructionCount = 8;
    void *oracle = resolver.find(kOracleSymbol);
    if (oracle == nullptr) {
        return nullptr;
    }

    const uintptr_t target = dejavu_decode_aarch64_adrp_add_target(
        static_cast<const uint32_t *>(oracle), kOracleInstructionCount,
        reinterpret_cast<uintptr_t>(oracle));
    const uintptr_t oracle_address = reinterpret_cast<uintptr_t>(oracle);
    const uintptr_t distance =
        target > oracle_address ? target - oracle_address : oracle_address - target;
    if (target == 0 || (target & 0x3U) != 0 || distance > uintptr_t{128} * 1024 * 1024) {
        return nullptr;
    }
    return reinterpret_cast<void *>(target);
}

struct InlineHookState {
    RootHelperProtectionSnapshot protection;
    void *original = nullptr;
    bool installed = false;
    bool permissions_dirty = true;
};

std::recursive_mutex g_inline_mutex;
std::unordered_map<void *, InlineHookState> g_inline_hooks;

bool restore_inline_permissions(InlineHookState &state, std::string *error) {
    if (!state.permissions_dirty)
        return true;
    state.permissions_dirty = !root_helper_restore(state.protection, error);
    return !state.permissions_dirty;
}

bool remove_inline_hook(void *target, InlineHookState &state, std::string *error) {
    if (state.installed) {
        state.permissions_dirty = true;
        if (!root_helper_apply_read_write(state.protection, error)) {
            std::string cleanup_error;
            if (!restore_inline_permissions(state, &cleanup_error))
                *error += "; permission restoration pending: " + cleanup_error;
            return false;
        }
        const int status = DobbyDestroy(target);
        if (status == 0)
            state.installed = false;
        std::string restore_error;
        const bool restored = restore_inline_permissions(state, &restore_error);
        if (status != 0 || !restored) {
            *error = status != 0 ? "DobbyDestroy failed: " + std::to_string(status) : "";
            if (!restored)
                *error += "; permission restoration pending: " + restore_error;
            return false;
        }
    }
    return restore_inline_permissions(state, error);
}

void *inline_hook(void *target, void *replacement) {
    std::lock_guard<std::recursive_mutex> lock(g_inline_mutex);
    if (target == nullptr || replacement == nullptr) {
        return nullptr;
    }
    auto existing = g_inline_hooks.find(target);
    if (existing != g_inline_hooks.end()) {
        if (existing->second.installed) {
            LOGE("ART hook target already owned: %p", target);
            return nullptr;
        }
        std::string error;
        if (!restore_inline_permissions(existing->second, &error)) {
            LOGE("ART hook cleanup pending: %p: %s", target, error.c_str());
            return nullptr;
        }
        g_inline_hooks.erase(existing);
    }
    if (DobbyIsHooked(target) > 0) {
        LOGE("ART hook target already patched by another owner: %p", target);
        return nullptr;
    }
    InlineHookState pending;
    std::string error;
    if (!root_helper_capture(target, 16, &pending.protection, &error)) {
        LOGE("ART hook protection capture failed: %p: %s", target, error.c_str());
        return nullptr;
    }
    auto &state = g_inline_hooks.emplace(target, std::move(pending)).first->second;
    if (!root_helper_apply_read_write(state.protection, &error)) {
        std::string restore_error;
        if (restore_inline_permissions(state, &restore_error))
            g_inline_hooks.erase(target);
        else
            error += "; permission restoration pending: " + restore_error;
        LOGE("ART hook permission change failed: %p: %s", target, error.c_str());
        return nullptr;
    }
    const int status = DobbyHook(target, replacement, &state.original);
    // A competing installation must not become ours merely because it exists.
    state.installed = status == 0 || (state.original != nullptr && DobbyIsHooked(target) > 0);
    const bool restored = restore_inline_permissions(state, &error);
    if (status != 0 || !restored) {
        std::string cleanup_error;
        if (remove_inline_hook(target, state, &cleanup_error)) {
            g_inline_hooks.erase(target);
            LOGE("ART hook installation rolled back: %p status=%d: %s", target, status,
                 error.c_str());
            return nullptr;
        }
        LOGE("ART hook cleanup deferred: %p status=%d: %s", target, status, cleanup_error.c_str());
        // LSPlant must receive the valid original while its replacement is live.
        // Keep the saved snapshot so its unhook callback can retry cleanup.
        return state.installed ? state.original : nullptr;
    }
    return state.original;
}

bool inline_unhook(void *target) {
    std::lock_guard<std::recursive_mutex> lock(g_inline_mutex);
    const auto iterator = g_inline_hooks.find(target);
    if (target == nullptr || iterator == g_inline_hooks.end())
        return false;
    std::string error;
    if (!remove_inline_hook(target, iterator->second, &error)) {
        LOGE("ART hook cleanup deferred: %p: %s", target, error.c_str());
        return false;
    }
    g_inline_hooks.erase(iterator);
    return true;
}

jobject hook(JNIEnv *env, jobject target, jobject bridge, jobject callback) {
    return lsplant::Hook(env, target, bridge, callback);
}

bool is_hooked(JNIEnv *env, jobject target) {
    return lsplant::IsHooked(env, target);
}

bool unhook(JNIEnv *env, jobject target) {
    return lsplant::UnHook(env, target);
}

bool deoptimize(JNIEnv *env, jobject method) {
    return lsplant::Deoptimize(env, method);
}

const DejavuLsplantBackend kBackend = {
    kBackendAbiVersion, hook, is_hooked, unhook, deoptimize,
};

} // namespace

extern "C" DejavuLsplantInitResult dejavu_lsplant_backend_initialize(JNIEnv *env) {
    std::lock_guard<std::mutex> lock(g_initialize_mutex);
    if (g_initialize_finished) {
        return g_initialize_result;
    }
    if (env == nullptr) {
        return g_initialize_result;
    }

    ArtElfResolver resolver;
    std::string resolver_error;
    if (!resolver.open_loaded("libart.so", &resolver_error)) {
        LOGE("libart resolver initialization failed: %s", resolver_error.c_str());
        g_initialize_result = {
            DejavuLsplantInitStatus::kResolverFailed,
            false,
            nullptr,
        };
        g_initialize_finished = true;
        return g_initialize_result;
    }

    lsplant::InitInfo info{
        .inline_hooker = inline_hook,
        .inline_unhooker = inline_unhook,
        .art_symbol_resolver =
            [&resolver](std::string_view name) {
                void *address = resolver.find(name);
                if (address == nullptr && name == "art_quick_to_interpreter_bridge") {
                    address = resolve_interpreter_bridge(resolver);
                    if (address != nullptr) {
                        LOGE("derived ART interpreter bridge: %p", address);
                    }
                }
                if (address == nullptr) {
                    LOGE("ART symbol not found: %.*s", static_cast<int>(name.size()), name.data());
                }
                return address;
            },
        .art_symbol_prefix_resolver =
            [&resolver](std::string_view prefix) {
                void *address = resolver.find_prefix(prefix);
                if (address == nullptr) {
                    LOGE("ART symbol prefix not found: %.*s", static_cast<int>(prefix.size()),
                         prefix.data());
                }
                return address;
            },
        .generated_class_name = "DejavuHooker_",
        .generated_source_name = "Dejavu",
        .generated_field_name = "hooker",
        .generated_method_name = "{target}",
        .executable_memory_allocator = {},
        .executable_memory_recycler = {},
    };

    const bool initialized = lsplant::Init(env, info);
    g_initialize_result =
        initialized
            ? DejavuLsplantInitResult{DejavuLsplantInitStatus::kReady, true, &kBackend}
            : DejavuLsplantInitResult{DejavuLsplantInitStatus::kLsplantFailed, true, nullptr};
    g_initialize_finished = true;
    return g_initialize_result;
}
