#include <jni.h>
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

namespace {
// XOM must not cover the ELF headers/dynamic tables consulted by dladdr, even
// on 16 KiB devices. This function is deliberately absent from .dynsym.
__attribute__((noinline, aligned(16384))) jlong namespace_target(jlong value) {
    asm volatile("nop");
    return value + 7;
}
} // namespace

extern "C" JNIEXPORT jlong JNICALL Java_io_dejavu_bridge_NativeNamespaceFixture_address(JNIEnv *,
                                                                                        jclass) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(namespace_target));
}

extern "C" JNIEXPORT jlong JNICALL Java_io_dejavu_bridge_NativeNamespaceFixture_call(JNIEnv *,
                                                                                     jclass,
                                                                                     jlong value) {
    return namespace_target(value);
}

extern "C" JNIEXPORT jint JNICALL
Java_io_dejavu_bridge_NativeNamespaceFixture_protect(JNIEnv *, jclass, jint protection) {
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || (protection != PROT_EXEC && protection != (PROT_READ | PROT_EXEC)))
        return -1;
    const auto address = reinterpret_cast<uintptr_t>(namespace_target);
    return mprotect(reinterpret_cast<void *>(address - address % page), static_cast<size_t>(page),
                    protection);
}

extern "C" JNIEXPORT jint JNICALL Java_io_dejavu_bridge_NativeNamespaceFixture_permissions(JNIEnv *,
                                                                                           jclass) {
    FILE *maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr)
        return -1;
    const auto address = reinterpret_cast<uintptr_t>(namespace_target);
    char line[512];
    jint protection = -1;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long start = 0, end = 0;
        char permissions[8]{};
        if (std::sscanf(line, "%lx-%lx %7s", &start, &end, permissions) != 3 || address < start ||
            address >= end)
            continue;
        protection = (permissions[0] == 'r' ? PROT_READ : 0) |
                     (permissions[1] == 'w' ? PROT_WRITE : 0) |
                     (permissions[2] == 'x' ? PROT_EXEC : 0);
        break;
    }
    std::fclose(maps);
    return protection;
}
