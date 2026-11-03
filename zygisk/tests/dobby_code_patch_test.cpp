#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <sys/mman.h>
#include <unistd.h>

struct TestPage { uintptr_t address; int protection; };
static std::vector<TestPage> mappings;
static std::vector<TestPage> changes;
static size_t failed_call = 0;
static size_t second_failed_call = 0;
static size_t flushes = 0;
static size_t page_size = 0;
static FILE *TestOpenMaps(const char *, const char *) {
  FILE *file = tmpfile(); assert(file);
  for (const auto &page : mappings) {
    char p[5] = {'-', '-', '-', 'p', 0};
    if (page.protection & PROT_READ) p[0] = 'r';
    if (page.protection & PROT_WRITE) p[1] = 'w';
    if (page.protection & PROT_EXEC) p[2] = 'x';
    fprintf(file, "%llx-%llx %s 0 0:0 0\n", static_cast<unsigned long long>(page.address),
            static_cast<unsigned long long>(page.address + page_size), p);
  }
  rewind(file); return file;
}
static int TestMprotect(void *address, size_t size, int protection) {
  assert(size == page_size);
  changes.push_back({reinterpret_cast<uintptr_t>(address), protection});
  return (failed_call == changes.size() || second_failed_call == changes.size()) ? -1 : 0;
}
static void TestClearCache(void *, void *) { ++flushes; }
#ifdef __APPLE__
#undef __APPLE__
#endif
#ifndef __linux__
#define __linux__ 1
#endif
#define fopen TestOpenMaps
#define mprotect TestMprotect
#define ClearCache TestClearCache
#include DOBBY_CODE_PATCH_SOURCE
#undef fopen
#undef mprotect
#undef ClearCache

static void reset() { changes.clear(); failed_call = 0; second_failed_call = 0; flushes = 0; }
int main() {
  page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  auto *memory = static_cast<uint8_t *>(mmap(nullptr, page_size * 3, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(memory != MAP_FAILED);
  const uintptr_t base = reinterpret_cast<uintptr_t>(memory);
  mappings = {{base, PROT_EXEC}, {base + page_size, PROT_READ | PROT_EXEC},
              {base + page_size * 2, PROT_READ | PROT_WRITE}};
  uint32_t value = 0x14000001;
  assert(DobbyCodePatch(memory + page_size - 4, reinterpret_cast<uint8_t *>(&value), 4) == 0);
  assert(changes.size() == 2 && changes[0].address == base && changes[1].address == base);
  assert(changes[1].protection == PROT_EXEC && flushes == 1);
  assert(std::memcmp(memory + page_size - 4, &value, 4) == 0);

  reset();
  uint8_t bytes[4] = {1,2,3,4};
  assert(DobbyCodePatch(memory + page_size - 2, bytes, 4) == 0);
  assert(changes.size() == 4);
  assert(changes[2].protection == PROT_EXEC && changes[3].protection == (PROT_READ | PROT_EXEC));

  reset(); failed_call = 1;
  uint32_t before = 0; std::memcpy(&before, memory, 4);
  assert(DobbyCodePatch(memory, reinterpret_cast<uint8_t *>(&value), 4) != 0);
  assert(std::memcmp(memory, &before, 4) == 0 && flushes == 0);
  reset(); failed_call = 2;
  uint8_t saved[4]; std::memcpy(saved, memory + page_size - 2, 4);
  assert(DobbyCodePatch(memory + page_size - 2, bytes, 4) != 0);
  assert(std::memcmp(saved, memory + page_size - 2, 4) == 0 && flushes == 0);
  assert(changes.size() == 3 && changes.back().protection == PROT_EXEC);
  reset(); failed_call = 2; second_failed_call = 3;
  assert(DobbyCodePatch(memory + page_size - 2, bytes, 4) == -1);
  assert(std::memcmp(saved, memory + page_size - 2, 4) == 0 && flushes == 0);
  reset(); failed_call = 2;
  assert(DobbyCodePatch(memory, reinterpret_cast<uint8_t *>(&value), 4) == -2);
  assert(std::memcmp(memory, &value, 4) == 0 && flushes == 1);

  reset();
  assert(DobbyCodePatch(reinterpret_cast<void *>(UINTPTR_MAX - 1), bytes, 4) != 0);
  assert(DobbyCodePatch(memory, bytes, 0) != 0 && changes.empty());
  mappings = {{base, PROT_READ | PROT_WRITE}};
  std::atomic<bool> done{false};
  __atomic_store_n(reinterpret_cast<uint32_t *>(memory), value, __ATOMIC_RELEASE);
  std::thread reader([&] {
    while (!done.load()) {
      uint32_t observed = __atomic_load_n(reinterpret_cast<uint32_t *>(memory), __ATOMIC_ACQUIRE);
      assert(observed == value || observed == 0xD503201F);
    }
  });
  for (int n = 0; n < 200; ++n) {
    uint32_t replacement = n % 2 ? value : 0xD503201F;
    assert(DobbyCodePatch(memory, reinterpret_cast<uint8_t *>(&replacement), 4) == 0);
  }
  done.store(true); reader.join();
  assert(changes.empty());
  munmap(memory, page_size * 3);
}
