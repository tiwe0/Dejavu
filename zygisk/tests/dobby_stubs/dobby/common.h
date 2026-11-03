#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <dobby.h>
#define PUBLIC
#define TARGET_ARCH_ARM64 1
#define __FUNC_CALL_TRACE__()
#define ERROR_LOG(...)
namespace stl { template<class T> using vector = std::vector<T>; }
namespace features {
namespace apple {
template<class T> void arm64e_pac_strip(T &) {}
template<class T> void arm64e_pac_strip_and_sign(T &) {}
}
namespace android { inline void make_memory_readable(void *, size_t) {} }
template<class T> void arm_thumb_fix_addr(T &) {}
}
