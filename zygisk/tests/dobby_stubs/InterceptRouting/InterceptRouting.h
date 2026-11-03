#pragma once
#include <Interceptor.h>
extern size_t test_patch_size;
extern bool test_prepare_fails;
extern bool test_activate_fails;
extern bool test_was_activated;
extern void *test_published_original;
extern void test_activation_observer();
struct InterceptRouting {
  Interceptor::Entry *entry;
  int error = 0;
  bool require_atomic_patch = false;
  explicit InterceptRouting(Interceptor::Entry *entry):entry(entry) {}
  virtual ~InterceptRouting() = default;
  virtual addr_t TrampolineTarget() { return 0; }
  bool GenerateTrampoline() { return !test_prepare_fails; }
  void GenerateRelocatedCode() {
    entry->patched = MemBlock(entry->addr, test_patch_size);
    entry->relocated = MemBlock(0x10000, 16);
  }
  void BackupOriginCode() {
    entry->origin_code_ = static_cast<uint8_t *>(::operator new(test_patch_size));
  }
  size_t trampoline_size() { return test_patch_size; }
  addr_t trampoline_addr() { return 0x30000; }
#include <dobby-active.h>
};
