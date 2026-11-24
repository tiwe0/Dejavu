#include "art_elf_resolver.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<unsigned char> read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.is_open());
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(input)),
                                      std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char **argv) {
    assert(argc == 9);
    const auto crc32_elf = read_file(argv[1]);
    const auto crc64_elf = read_file(argv[2]);
    const auto sha256_elf = read_file(argv[3]);
    const auto multichunk_elf = read_file(argv[4]);
    const auto debugonly_elf = read_file(argv[5]);
    const auto symtab_elf = read_file(argv[6]);
    const auto corrupt_elf = read_file(argv[7]);
    const auto trailing_elf = read_file(argv[8]);

    constexpr uintptr_t kBias = 0x7000000000ULL;

    auto verify_valid = [&](const std::vector<unsigned char> &elf) {
        // Deliberately test unaligned input buffer (offset +1) to verify UB-free memcpy reads.
        std::vector<unsigned char> unaligned(elf.size() + 1, 0xaa);
        std::copy(elf.begin(), elf.end(), unaligned.begin() + 1);

        ArtElfResolver resolver;
        std::string error;
        assert(resolver.open_memory(unaligned.data() + 1, elf.size(), kBias, &error));
        assert(error.empty());

        assert(reinterpret_cast<uintptr_t>(resolver.find("exported_art_fn")) == kBias + 0x1000);
        assert(reinterpret_cast<uintptr_t>(resolver.find("_ZN3art11ClassLinker10FixupStaticEv")) ==
               kBias + 0x2400);
        assert(reinterpret_cast<uintptr_t>(resolver.find_prefix("_ZN3art11ClassLinker")) ==
               kBias + 0x2400);
        assert(reinterpret_cast<uintptr_t>(resolver.find("shared_symbol")) == kBias + 0x1100);
        // SHN_ABS symbols must not be resolved as load_bias + st_value.
        assert(resolver.find("abs_constant_symbol") == nullptr);
        assert(resolver.find("does_not_exist") == nullptr);

        std::vector<ArtElfSymbol> symbols;
        assert(resolver.enumerate_symbols(&symbols, &error));
        bool saw_exported = false;
        bool saw_hidden = false;
        size_t shared_count = 0;
        for (const auto &sym : symbols) {
            assert(sym.name != "abs_constant_symbol");
            if (sym.name == "exported_art_fn") {
                saw_exported = true;
                assert(sym.dynamic && sym.address == kBias + 0x1000);
            } else if (sym.name == "_ZN3art11ClassLinker10FixupStaticEv") {
                saw_hidden = true;
                assert(!sym.dynamic && sym.address == kBias + 0x2400);
            } else if (sym.name == "shared_symbol") {
                ++shared_count;
                assert(sym.dynamic && sym.address == kBias + 0x1100);
            }
        }
        assert(saw_exported && saw_hidden && shared_count == 1);
    };

    verify_valid(crc32_elf);
    verify_valid(crc64_elf);
    verify_valid(sha256_elf);

    // Multi-chunk (> 128 KiB decompressed) XZ stream verifies progress loop across > 64 KiB chunks.
    {
        ArtElfResolver resolver;
        std::string error;
        assert(resolver.open_memory(multichunk_elf.data(), multichunk_elf.size(), kBias, &error));
        assert(reinterpret_cast<uintptr_t>(resolver.find("_ZN3art11ClassLinker10FixupStaticEv")) ==
               kBias + 0x2400);
        assert(reinterpret_cast<uintptr_t>(resolver.find("tail_symbol_after_128k")) ==
               kBias + 0x4800);
    }

    // .gnu_debugdata-only ELF (no .dynsym or .symtab in outer image) still opens and resolves!
    {
        ArtElfResolver resolver;
        std::string error;
        assert(resolver.open_memory(debugonly_elf.data(), debugonly_elf.size(), kBias, &error));
        assert(reinterpret_cast<uintptr_t>(resolver.find("_ZN3art11ClassLinker10FixupStaticEv")) ==
               kBias + 0x2400);

        // Reuse the same ArtElfResolver on an outer .symtab-only ELF: stale debugdata_ must clear!
        assert(resolver.open_memory(symtab_elf.data(), symtab_elf.size(), kBias, &error));
        assert(reinterpret_cast<uintptr_t>(resolver.find("outer_symtab_fn")) == kBias + 0x3200);
        assert(resolver.find("_ZN3art11ClassLinker10FixupStaticEv") == nullptr);
    }

    // Corrupt and trailing-garbage .gnu_debugdata degrade gracefully when .dynsym is valid.
    for (const auto *bad : {&corrupt_elf, &trailing_elf}) {
        ArtElfResolver resolver;
        std::string error;
        assert(resolver.open_memory(bad->data(), bad->size(), kBias, &error));
        assert(reinterpret_cast<uintptr_t>(resolver.find("exported_art_fn")) == kBias + 0x1000);
        assert(resolver.find("_ZN3art11ClassLinker10FixupStaticEv") == nullptr);
    }

    // Reject truncated/empty ELF images.
    {
        ArtElfResolver resolver;
        std::string error;
        const unsigned char tiny[4] = {0x7f, 'E', 'L', 'F'};
        assert(!resolver.open_memory(tiny, sizeof(tiny), kBias, &error));
        assert(!error.empty());
    }

    puts("OK: ArtElfResolver resolves .dynsym, .symtab, and multi-chunk XZ .gnu_debugdata");
    return 0;
}
