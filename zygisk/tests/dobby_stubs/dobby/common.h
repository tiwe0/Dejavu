#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <dobby.h>
#define PUBLIC
#define TARGET_ARCH_ARM64 1
#define __FUNC_CALL_TRACE__()
inline void TestDobbyErrorLog(const char *, ...) {
#ifdef DOBBY_TEST_LOG_OBSERVER
    DOBBY_TEST_LOG_OBSERVER();
#endif
}
#define ERROR_LOG(...) TestDobbyErrorLog(__VA_ARGS__)
#define DEBUG_LOG(...) TestDobbyErrorLog(__VA_ARGS__)
namespace stl {
template <class T> using vector = std::vector<T>;
}
namespace features {
namespace apple {
template <class T> void arm64e_pac_strip(T &) {
}
template <class T> void arm64e_pac_strip_and_sign(T &) {
}
} // namespace apple
namespace android {
inline void make_memory_readable(void *, size_t) {
}
} // namespace android
template <class T> void arm_thumb_fix_addr(T &) {
}
} // namespace features
