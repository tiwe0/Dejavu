#include "lsplant_backend.h"

#include <android/log.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <mutex>
#include <string>

#include <dobby.h>
#include <lsplant.hpp>

#include "aarch64_adrp_add.h"
#include "art_elf_resolver.h"

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
        static_cast<const uint32_t *>(oracle),
        kOracleInstructionCount,
        reinterpret_cast<uintptr_t>(oracle));
    const uintptr_t oracle_address = reinterpret_cast<uintptr_t>(oracle);
    const uintptr_t distance = target > oracle_address
        ? target - oracle_address
        : oracle_address - target;
    if (target == 0 || (target & 0x3U) != 0 || distance > uintptr_t{128} * 1024 * 1024) {
        return nullptr;
    }
    return reinterpret_cast<void *>(target);
}

bool make_writable_executable(void *address) {
    const long raw_page_size = sysconf(_SC_PAGESIZE);
    if (address == nullptr || raw_page_size <= 0) {
        return false;
    }
    const uintptr_t page_size = static_cast<uintptr_t>(raw_page_size);
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    const uintptr_t page = value - value % page_size;
    return mprotect(
               reinterpret_cast<void *>(page),
               page_size,
               PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

void *inline_hook(void *target, void *replacement) {
    if (target == nullptr || replacement == nullptr) {
        return nullptr;
    }
    if (!make_writable_executable(target)) {
        LOGE("mprotect failed for inline hook target %p: errno=%d", target, errno);
        return nullptr;
    }
    void *original = nullptr;
    const int status = DobbyHook(target, replacement, &original);
    if (status != RS_SUCCESS) {
        LOGE(
            "DobbyHook failed: target=%p replacement=%p status=%d page_offset=%zu",
            target,
            replacement,
            status,
            reinterpret_cast<uintptr_t>(target) % static_cast<uintptr_t>(sysconf(_SC_PAGESIZE)));
        return nullptr;
    }
    return original;
}

bool inline_unhook(void *target) {
    return target != nullptr && DobbyDestroy(target) == 0;
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
    kBackendAbiVersion,
    hook,
    is_hooked,
    unhook,
    deoptimize,
};

}  // namespace

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
        .art_symbol_resolver = [&resolver](std::string_view name) {
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
        .art_symbol_prefix_resolver = [&resolver](std::string_view prefix) {
            void *address = resolver.find_prefix(prefix);
            if (address == nullptr) {
                LOGE(
                    "ART symbol prefix not found: %.*s",
                    static_cast<int>(prefix.size()),
                    prefix.data());
            }
            return address;
        },
        .generated_class_name = "DejavuHooker_",
        .generated_source_name = "Dejavu",
    };

    const bool initialized = lsplant::Init(env, info);
    g_initialize_result = initialized
        ? DejavuLsplantInitResult{DejavuLsplantInitStatus::kReady, true, &kBackend}
        : DejavuLsplantInitResult{DejavuLsplantInitStatus::kLsplantFailed, true, nullptr};
    g_initialize_finished = true;
    return g_initialize_result;
}
