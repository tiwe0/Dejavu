#include <sys/mman.h>
#include <cstdarg>
#include <cstdlib>
#include <iostream>
#include <string>
#include "root_helper_client.h"

// Exercise the real callbacks, including their saved-state and cleanup paths.
#include "../src/lsplant_backend.cpp"

namespace {
struct Scenario {
    int original_protection = PROT_READ | PROT_EXEC;
    int current_protection = original_protection;
    int captures = 0;
    int applies = 0;
    int restores = 0;
    int installs = 0;
    int destroys = 0;
    int restore_failures = 0;
    bool capture_failure = false;
    bool apply_failure = false;
    bool hook_failure = false;
    bool hook_failure_after_patch = false;
    bool destroy_failure = false;
    bool foreign_race = false;
    bool installed = false;
} state;
void *const kTarget = reinterpret_cast<void *>(0x1020);
void *const kReplacement = reinterpret_cast<void *>(0x2000);
void *const kOriginal = reinterpret_cast<void *>(0x3000);

void check(bool value, const char *message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
void reset(int protection = PROT_READ | PROT_EXEC) {
    g_inline_hooks.clear();
    state = {};
    state.original_protection = state.current_protection = protection;
}
}

extern "C" int __android_log_print(int, const char *, const char *, ...) { return 0; }
extern "C" int __android_log_write(int, const char *, const char *) { return 0; }

bool root_helper_capture(void *target, size_t size, RootHelperProtectionSnapshot *snapshot,
                         std::string *error) {
    ++state.captures;
    check(target == kTarget && size == 16, "capture the complete ordinary ARM64 patch window");
    if (state.capture_failure) { *error = "capture failure"; return false; }
    snapshot->ranges = {{0x1000, 4096, state.current_protection}};
    return true;
}
bool root_helper_apply_read_write(const RootHelperProtectionSnapshot &snapshot, std::string *error) {
    ++state.applies;
    check(snapshot.ranges[0].start == 0x1000 && snapshot.ranges[0].length == 4096,
          "modify only the immutable captured range");
    state.current_protection = PROT_READ | PROT_WRITE | PROT_EXEC;
    if (state.apply_failure) { *error = "lost apply response"; return false; }
    return true;
}
bool root_helper_restore(const RootHelperProtectionSnapshot &snapshot, std::string *error) {
    ++state.restores;
    check(snapshot.ranges[0].start == 0x1000 && snapshot.ranges[0].length == 4096 &&
          snapshot.ranges[0].protection == state.original_protection,
          "restore the original range and permissions, never the current RWX mapping");
    if (state.restore_failures > 0) {
        --state.restore_failures;
        *error = "injected restore failure";
        return false;
    }
    state.current_protection = snapshot.ranges[0].protection;
    return true;
}
extern "C" int DobbyIsHooked(void *) { return state.installed ? 1 : 0; }
extern "C" int DobbyHook(void *, void *, void **original) {
    ++state.installs;
    check((state.current_protection & PROT_WRITE) != 0, "patching requires writable target pages");
    if (state.foreign_race) { state.installed = true; return -1; }
    if (state.hook_failure) return -1;
    *original = kOriginal;
    state.installed = true;
    return state.hook_failure_after_patch ? -1 : 0;
}
extern "C" int DobbyDestroy(void *) {
    ++state.destroys;
    check((state.current_protection & PROT_WRITE) != 0, "unpatching requires writable pages");
    if (state.destroy_failure) return -1;
    state.installed = false;
    return 0;
}
uintptr_t dejavu_decode_aarch64_adrp_add_target(const uint32_t *, size_t, uintptr_t) { return 0; }

int main() {
    for (int protection : {PROT_READ | PROT_EXEC, PROT_EXEC}) {
        reset(protection);
        check(inline_hook(kTarget, kReplacement) == kOriginal, "normal installation returns original");
        check(state.current_protection == protection, "normal installation does not leave RWX");
        check(inline_unhook(kTarget), "normal unhook succeeds");
        check(state.current_protection == protection && !state.installed,
              "unhook restores the original RX/XOM permissions");
    }
    std::cout << "OK: LSPlant RX and XOM permissions restored after install and uninstall\n";

    reset(); state.capture_failure = true;
    check(inline_hook(kTarget, kReplacement) == nullptr && state.applies == 0 && state.installs == 0,
          "capture failure does not change memory");
    reset(); state.apply_failure = true;
    check(inline_hook(kTarget, kReplacement) == nullptr && state.installs == 0 &&
          state.current_protection == state.original_protection,
          "failed apply still restores an uncertain changed range");
    reset(); state.hook_failure = true;
    check(inline_hook(kTarget, kReplacement) == nullptr && !state.installed &&
          state.current_protection == state.original_protection && g_inline_hooks.empty(),
          "failed Dobby prepare restores permissions and removes unexposed state");
    std::cout << "OK: LSPlant capture/apply/prepare failures preserve permissions\n";

    reset(); state.hook_failure = true; state.restore_failures = 2; state.destroy_failure = true;
    check(inline_hook(kTarget, kReplacement) == nullptr && !state.installed &&
          state.destroys == 0 && g_inline_hooks.count(kTarget) == 1,
          "unwritten entry with deferred permissions must not report a live ART hook");
    state.restore_failures = 0;
    check(inline_unhook(kTarget) && state.destroys == 0 && g_inline_hooks.empty(),
          "unwritten entry retries permissions only, without destroying a foreign hook");
    std::cout << "OK: LSPlant unwritten entry is never reported as a live replacement\n";

    reset(); state.restore_failures = 1;
    check(inline_hook(kTarget, kReplacement) == nullptr && !state.installed &&
          state.destroys == 1 && state.current_protection == state.original_protection,
          "restore failure rolls back a hook when cleanup is available");
    reset(); state.restore_failures = 3; state.destroy_failure = true;
    check(inline_hook(kTarget, kReplacement) == kOriginal && state.installed &&
          g_inline_hooks.count(kTarget) == 1,
          "live deferred cleanup must return original, not an unusable null pointer");
    state.restore_failures = 0; state.destroy_failure = false;
    check(inline_unhook(kTarget) && !state.installed && g_inline_hooks.empty(),
          "live failed-install cleanup can be retried");
    std::cout << "OK: LSPlant failed install retains a valid original when rollback is blocked\n";

    reset(); check(inline_hook(kTarget, kReplacement) == kOriginal, "setup installed hook");
    state.destroy_failure = true;
    check(!inline_unhook(kTarget) && state.installed && g_inline_hooks.count(kTarget) == 1 &&
          state.current_protection == state.original_protection,
          "failed destroy retains ownership and restores protection");
    state.destroy_failure = false;
    check(inline_unhook(kTarget), "destroy failure is retryable");

    reset(); check(inline_hook(kTarget, kReplacement) == kOriginal, "setup installed hook");
    state.restore_failures = 1;
    check(!inline_unhook(kTarget) && !state.installed && g_inline_hooks.count(kTarget) == 1,
          "destroyed hook with failed restore remains tracked");
    check(inline_unhook(kTarget) && state.destroys == 1 && state.applies == 2,
          "retry a restore-only state without destroying the hook twice");
    std::cout << "OK: LSPlant unhook failures retain precise retry state\n";

    reset(); state.installed = true;
    check(inline_hook(kTarget, kReplacement) == nullptr && state.captures == 0,
          "foreign entry rejected before permissions change");
    check(!inline_unhook(kTarget) && state.destroys == 0, "foreign hook cannot be uninstalled");
    reset(); state.foreign_race = true;
    check(inline_hook(kTarget, kReplacement) == nullptr && state.installed && state.destroys == 0 &&
          g_inline_hooks.empty() && state.current_protection == state.original_protection,
          "foreign race does not acquire ownership or destroy the competing hook");
    std::cout << "OK: LSPlant foreign hook ownership remains protected\n";
}
