#pragma once
#include <cstddef>
#include <cstdint>
#define PUBLIC
using addr_t = uintptr_t;
inline void TestDobbyCodePatchErrorLog(const char *, ...) {
}
#define ERROR_LOG(...) TestDobbyCodePatchErrorLog(__VA_ARGS__)
