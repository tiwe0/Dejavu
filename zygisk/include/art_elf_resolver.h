#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

struct ArtElfSymbol {
    std::string name;
    unsigned char type = 0;
    unsigned char bind = 0;
    uintptr_t address = 0;
    bool dynamic = false;
};

class ArtElfResolver {
  public:
    ArtElfResolver() = default;
    ~ArtElfResolver();

    ArtElfResolver(const ArtElfResolver &) = delete;
    ArtElfResolver &operator=(const ArtElfResolver &) = delete;

    bool open_loaded(std::string_view soname, std::string *error);
    bool open_memory(const void *image, size_t size, uintptr_t load_bias, std::string *error);
    void *find(std::string_view name) const;
    void *find_prefix(std::string_view prefix) const;
    bool enumerate_symbols(std::vector<ArtElfSymbol> *symbols, std::string *error) const;
    const std::string &path() const {
        return path_;
    }
    uintptr_t load_bias() const {
        return load_bias_;
    }

  private:
    struct SymbolTable {
        const void *symbols = nullptr;
        size_t symbol_count = 0;
        size_t symbol_entry_size = 0;
        const char *strings = nullptr;
        size_t strings_size = 0;
    };

    const void *mapping_ = nullptr;
    size_t mapping_size_ = 0;
    bool owns_mapping_ = false;
    std::string path_;
    uintptr_t load_bias_ = 0;
    SymbolTable dynamic_ = {};
    SymbolTable full_ = {};
    std::vector<unsigned char> debugdata_elf_;
    SymbolTable debugdata_ = {};

    void reset();
    bool initialize_tables(std::string *error);
    bool parse_elf_tables(const void *image, size_t size, SymbolTable *dynamic_out,
                          SymbolTable *full_out, bool allow_debugdata, std::string *error);
    bool load_gnu_debugdata(const unsigned char *compressed, size_t compressed_size);
    void *find_in(const SymbolTable &table, std::string_view name, bool prefix) const;
};
