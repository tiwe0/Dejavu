#include "art_elf_resolver.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__ANDROID__) || defined(__linux__)
#include <link.h>
#endif

#if defined(__has_include) && __has_include(<elf.h>)
#include <elf.h>
#else
#define EI_NIDENT 16
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define EI_CLASS 4
#define EI_DATA 5
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_DYNSYM 11
#define SHN_UNDEF 0
#define SHN_LORESERVE 0xff00
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2
#define SHN_XINDEX 0xffff
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)

struct Elf64_Ehdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64_Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

struct Elf64_Sym {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};
#endif

#ifndef SHN_LORESERVE
#define SHN_LORESERVE 0xff00
#endif
#ifndef SHN_ABS
#define SHN_ABS 0xfff1
#endif
#ifndef SHN_COMMON
#define SHN_COMMON 0xfff2
#endif
#ifndef SHN_XINDEX
#define SHN_XINDEX 0xffff
#endif

#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_set>

#ifdef DEJAVU_WITH_XZ_EMBEDDED
extern "C" {
#include "xz.h"
}
#endif

namespace {

#ifdef DEJAVU_WITH_XZ_EMBEDDED
constexpr size_t kMaxCompressedDebugData = 16 * 1024 * 1024;
constexpr size_t kMaxDecompressedDebugData = 32 * 1024 * 1024;
constexpr uint32_t kMaxXzDictionarySize = 64 * 1024 * 1024;
constexpr size_t kXzOutputChunkSize = 64 * 1024;
#endif

#if defined(__ANDROID__) || defined(__linux__)
struct LoadedLibrary {
    std::string_view soname;
    std::string path;
    uintptr_t load_bias = 0;
};

bool has_soname(std::string_view path, std::string_view soname) {
    const size_t slash = path.rfind('/');
    const std::string_view basename =
        slash == std::string_view::npos ? path : path.substr(slash + 1);
    return basename == soname;
}

int locate_library(dl_phdr_info *info, size_t, void *opaque) {
    auto *library = static_cast<LoadedLibrary *>(opaque);
    if (info == nullptr || info->dlpi_name == nullptr || info->dlpi_name[0] == '\0') {
        return 0;
    }
    const std::string_view path(info->dlpi_name);
    if (!has_soname(path, library->soname)) {
        return 0;
    }
    library->path.assign(path);
    library->load_bias = static_cast<uintptr_t>(info->dlpi_addr);
    return 1;
}
#endif

bool checked_range(uint64_t offset, uint64_t size, size_t total) {
    const uint64_t total64 = static_cast<uint64_t>(total);
    return offset <= total64 && size <= total64 - offset;
}

template <typename T> T read_unaligned(const unsigned char *ptr) {
    T value;
    memcpy(&value, ptr, sizeof(T));
    return value;
}

bool symbol_is_resolvable(const Elf64_Sym &symbol, uintptr_t load_bias, uintptr_t *address_out) {
    if (symbol.st_shndx == SHN_UNDEF || symbol.st_shndx == SHN_ABS ||
        symbol.st_shndx == SHN_COMMON ||
        (symbol.st_shndx >= SHN_LORESERVE && symbol.st_shndx != SHN_XINDEX) ||
        symbol.st_value == 0) {
        return false;
    }
    if (symbol.st_value > std::numeric_limits<uintptr_t>::max()) {
        return false;
    }
    const uintptr_t sym_val = static_cast<uintptr_t>(symbol.st_value);
    if (sym_val > std::numeric_limits<uintptr_t>::max() - load_bias) {
        return false;
    }
    *address_out = load_bias + sym_val;
    return true;
}

void set_error(std::string *error, const char *message) {
    if (error != nullptr) {
        error->assign(message);
    }
}

#ifdef DEJAVU_WITH_XZ_EMBEDDED
void ensure_xz_crc_initialized() {
    static std::once_flag once;
    std::call_once(once, []() {
        xz_crc32_init();
#ifdef XZ_USE_CRC64
        xz_crc64_init();
#endif
    });
}

bool decompress_xz_bounded(const unsigned char *input, size_t input_size,
                           std::vector<unsigned char> *output) {
    if (input == nullptr || input_size == 0 || input_size > kMaxCompressedDebugData ||
        output == nullptr) {
        return false;
    }
    ensure_xz_crc_initialized();
    std::unique_ptr<xz_dec, decltype(&xz_dec_end)> decoder(
        xz_dec_init(XZ_DYNALLOC, kMaxXzDictionarySize), &xz_dec_end);
    if (!decoder) {
        return false;
    }

    output->clear();
    std::vector<unsigned char> chunk(kXzOutputChunkSize);
    xz_buf buf = {};
    buf.in = input;
    buf.in_pos = 0;
    buf.in_size = input_size;
    buf.out = chunk.data();
    buf.out_pos = 0;
    buf.out_size = chunk.size();

    while (true) {
        const size_t prev_in = buf.in_pos;
#ifdef XZ_DEC_CONCATENATED
        const enum xz_ret status = xz_dec_catrun(decoder.get(), &buf, 1);
#else
        const enum xz_ret status = xz_dec_run(decoder.get(), &buf);
#endif
        const size_t produced = buf.out_pos;
        if (produced > 0) {
            if (output->size() > kMaxDecompressedDebugData - produced) {
                output->clear();
                return false;
            }
            output->insert(output->end(), chunk.data(), chunk.data() + produced);
            buf.out_pos = 0;
        }
        if (status == XZ_STREAM_END) {
            if (buf.in_pos != buf.in_size) {
                output->clear();
                return false;
            }
            return !output->empty();
        }
        if (status == XZ_OK) {
            if (buf.in_pos == prev_in && produced == 0) {
                output->clear();
                return false;
            }
            continue;
        }
        if (status == XZ_UNSUPPORTED_CHECK) {
            continue;
        }
        output->clear();
        return false;
    }
}
#endif

} // namespace

ArtElfResolver::~ArtElfResolver() {
    reset();
}

void ArtElfResolver::reset() {
    if (owns_mapping_ && mapping_ != nullptr) {
        munmap(const_cast<void *>(mapping_), mapping_size_);
    }
    mapping_ = nullptr;
    mapping_size_ = 0;
    owns_mapping_ = false;
    path_.clear();
    load_bias_ = 0;
    dynamic_ = {};
    full_ = {};
    debugdata_elf_.clear();
    debugdata_ = {};
}

bool ArtElfResolver::open_loaded(std::string_view soname, std::string *error) {
#if !defined(__ANDROID__) && !defined(__linux__)
    (void)soname;
    reset();
    set_error(error, "dl_iterate_phdr is unavailable on this host");
    return false;
#else
    LoadedLibrary library{soname, {}, 0};
    struct stat file_stat = {};

    reset();
    if (soname.empty()) {
        set_error(error, "empty library name");
        return false;
    }
    dl_iterate_phdr(locate_library, &library);
    if (library.path.empty()) {
        set_error(error, "loaded library not found");
        return false;
    }

    const int fd = open(library.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_error(error, "unable to open loaded library image");
        return false;
    }
    if (fstat(fd, &file_stat) != 0 || file_stat.st_size <= 0 ||
        static_cast<uintmax_t>(file_stat.st_size) > std::numeric_limits<size_t>::max()) {
        close(fd);
        set_error(error, "invalid loaded library image size");
        return false;
    }
    mapping_size_ = static_cast<size_t>(file_stat.st_size);
    void *mapped = mmap(nullptr, mapping_size_, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) {
        mapping_ = nullptr;
        mapping_size_ = 0;
        set_error(error, "unable to map loaded library image");
        return false;
    }
    mapping_ = mapped;
    owns_mapping_ = true;
    path_ = library.path;
    load_bias_ = library.load_bias;
    if (!initialize_tables(error)) {
        reset();
        return false;
    }
    return true;
#endif
}

bool ArtElfResolver::open_memory(const void *image, size_t size, uintptr_t load_bias,
                                 std::string *error) {
    reset();
    if (image == nullptr || size == 0) {
        set_error(error, "empty ELF image");
        return false;
    }
    mapping_ = image;
    mapping_size_ = size;
    owns_mapping_ = false;
    load_bias_ = load_bias;
    if (!initialize_tables(error)) {
        reset();
        return false;
    }
    return true;
}

bool ArtElfResolver::load_gnu_debugdata(const unsigned char *compressed, size_t compressed_size) {
#ifndef DEJAVU_WITH_XZ_EMBEDDED
    (void)compressed;
    (void)compressed_size;
    return false;
#else
    std::vector<unsigned char> decompressed;
    if (!decompress_xz_bounded(compressed, compressed_size, &decompressed)) {
        return false;
    }
    SymbolTable nested_dynamic = {};
    SymbolTable nested_full = {};
    std::string ignored;
    if (!parse_elf_tables(decompressed.data(), decompressed.size(), &nested_dynamic, &nested_full,
                          false, &ignored)) {
        return false;
    }
    if (nested_full.symbol_count == 0 && nested_dynamic.symbol_count == 0) {
        return false;
    }
    debugdata_elf_ = std::move(decompressed);
    nested_dynamic = {};
    nested_full = {};
    if (!parse_elf_tables(debugdata_elf_.data(), debugdata_elf_.size(), &nested_dynamic,
                          &nested_full, false, &ignored)) {
        debugdata_elf_.clear();
        return false;
    }
    debugdata_ = nested_full.symbol_count > 0 ? nested_full : nested_dynamic;
    return debugdata_.symbol_count > 0;
#endif
}

bool ArtElfResolver::parse_elf_tables(const void *image, size_t size, SymbolTable *dynamic_out,
                                      SymbolTable *full_out, bool allow_debugdata,
                                      std::string *error) {
    if (!checked_range(0, sizeof(Elf64_Ehdr), size)) {
        set_error(error, "truncated ELF header");
        return false;
    }
    const auto *base = static_cast<const unsigned char *>(image);
    const Elf64_Ehdr header = read_unaligned<Elf64_Ehdr>(base);
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_shentsize != sizeof(Elf64_Shdr) ||
        header.e_shoff == 0) {
        set_error(error, "unsupported ELF image");
        return false;
    }

    uint64_t shnum = header.e_shnum;
    uint64_t shstrndx = header.e_shstrndx;
    if (shnum == 0 || shstrndx == SHN_XINDEX) {
        if (!checked_range(header.e_shoff, sizeof(Elf64_Shdr), size)) {
            set_error(error, "truncated ELF section table");
            return false;
        }
        const Elf64_Shdr section0 = read_unaligned<Elf64_Shdr>(base + header.e_shoff);
        if (shnum == 0) {
            shnum = section0.sh_size;
        }
        if (shstrndx == SHN_XINDEX) {
            shstrndx = section0.sh_link;
        }
    }
    if (shnum == 0 || shnum > static_cast<uint64_t>(size) / sizeof(Elf64_Shdr)) {
        set_error(error, "truncated ELF section table");
        return false;
    }
    const uint64_t section_bytes = shnum * sizeof(Elf64_Shdr);
    if (!checked_range(header.e_shoff, section_bytes, size)) {
        set_error(error, "truncated ELF section table");
        return false;
    }

    auto read_shdr = [&](uint64_t idx) -> Elf64_Shdr {
        return read_unaligned<Elf64_Shdr>(base + header.e_shoff + idx * sizeof(Elf64_Shdr));
    };

    const char *shstrtab = nullptr;
    size_t shstrtab_size = 0;
    if (allow_debugdata && shstrndx != SHN_UNDEF && shstrndx < shnum) {
        const Elf64_Shdr shstr = read_shdr(shstrndx);
        if (shstr.sh_type == SHT_STRTAB && shstr.sh_size > 0 &&
            checked_range(shstr.sh_offset, shstr.sh_size, size)) {
            shstrtab = reinterpret_cast<const char *>(base + shstr.sh_offset);
            shstrtab_size = static_cast<size_t>(shstr.sh_size);
        }
    }

    const unsigned char *debugdata_ptr = nullptr;
    size_t debugdata_size = 0;

    for (uint64_t index = 0; index < shnum; ++index) {
        const Elf64_Shdr section = read_shdr(index);
        if (allow_debugdata && shstrtab != nullptr && section.sh_type == SHT_PROGBITS &&
            section.sh_name < shstrtab_size && section.sh_size > 0 &&
            checked_range(section.sh_offset, section.sh_size, size)) {
            const char *sname = shstrtab + section.sh_name;
            const size_t avail = shstrtab_size - section.sh_name;
            const void *nul = memchr(sname, '\0', avail);
            if (nul != nullptr) {
                const size_t slen = static_cast<const char *>(nul) - sname;
                constexpr std::string_view kGnuDebugData = ".gnu_debugdata";
                if (slen == kGnuDebugData.size() &&
                    memcmp(sname, kGnuDebugData.data(), kGnuDebugData.size()) == 0) {
                    debugdata_ptr = base + section.sh_offset;
                    debugdata_size = static_cast<size_t>(section.sh_size);
                }
            }
        }
        if (section.sh_type != SHT_DYNSYM && section.sh_type != SHT_SYMTAB) {
            continue;
        }
        if (section.sh_link >= shnum || section.sh_entsize < sizeof(Elf64_Sym) ||
            section.sh_size == 0 || section.sh_size % section.sh_entsize != 0 ||
            !checked_range(section.sh_offset, section.sh_size, size)) {
            continue;
        }
        const Elf64_Shdr strings = read_shdr(section.sh_link);
        if (strings.sh_type != SHT_STRTAB || strings.sh_size == 0 ||
            !checked_range(strings.sh_offset, strings.sh_size, size)) {
            continue;
        }

        SymbolTable table{
            base + section.sh_offset,
            static_cast<size_t>(section.sh_size / section.sh_entsize),
            static_cast<size_t>(section.sh_entsize),
            reinterpret_cast<const char *>(base + strings.sh_offset),
            static_cast<size_t>(strings.sh_size),
        };
        if (section.sh_type == SHT_DYNSYM) {
            if (dynamic_out != nullptr && dynamic_out->symbol_count == 0) {
                *dynamic_out = table;
            }
        } else if (full_out != nullptr && full_out->symbol_count == 0) {
            *full_out = table;
        }
    }

    if (allow_debugdata && debugdata_ptr != nullptr && debugdata_size > 0) {
        load_gnu_debugdata(debugdata_ptr, debugdata_size);
    }

    return true;
}

bool ArtElfResolver::initialize_tables(std::string *error) {
    if (!parse_elf_tables(mapping_, mapping_size_, &dynamic_, &full_, true, error)) {
        return false;
    }
    if (dynamic_.symbol_count == 0 && full_.symbol_count == 0 && debugdata_.symbol_count == 0) {
        set_error(error, "ELF image has no usable symbol table");
        return false;
    }
    return true;
}

void *ArtElfResolver::find_in(const SymbolTable &table, std::string_view name, bool prefix) const {
    if (table.symbols == nullptr || table.symbol_count == 0 || name.empty()) {
        return nullptr;
    }
    const auto *bytes = static_cast<const unsigned char *>(table.symbols);
    for (size_t index = 0; index < table.symbol_count; ++index) {
        const Elf64_Sym symbol = read_unaligned<Elf64_Sym>(bytes + index * table.symbol_entry_size);
        uintptr_t address = 0;
        if (symbol.st_name >= table.strings_size ||
            !symbol_is_resolvable(symbol, load_bias_, &address)) {
            continue;
        }
        const char *candidate = table.strings + symbol.st_name;
        const size_t available = table.strings_size - symbol.st_name;
        const void *terminator = memchr(candidate, '\0', available);
        if (terminator == nullptr) {
            continue;
        }
        const size_t candidate_size = static_cast<const char *>(terminator) - candidate;
        const bool matches =
            prefix
                ? candidate_size >= name.size() && memcmp(candidate, name.data(), name.size()) == 0
                : candidate_size == name.size() && memcmp(candidate, name.data(), name.size()) == 0;
        if (matches) {
            return reinterpret_cast<void *>(address);
        }
    }
    return nullptr;
}

void *ArtElfResolver::find(std::string_view name) const {
    if (void *address = find_in(dynamic_, name, false); address != nullptr) {
        return address;
    }
    if (void *address = find_in(full_, name, false); address != nullptr) {
        return address;
    }
    return find_in(debugdata_, name, false);
}

void *ArtElfResolver::find_prefix(std::string_view prefix) const {
    if (void *address = find_in(dynamic_, prefix, true); address != nullptr) {
        return address;
    }
    if (void *address = find_in(full_, prefix, true); address != nullptr) {
        return address;
    }
    return find_in(debugdata_, prefix, true);
}

bool ArtElfResolver::enumerate_symbols(std::vector<ArtElfSymbol> *symbols,
                                       std::string *error) const {
    if (symbols == nullptr) {
        set_error(error, "invalid symbol output");
        return false;
    }
    symbols->clear();
    if (dynamic_.symbol_count == 0 && full_.symbol_count == 0 && debugdata_.symbol_count == 0) {
        set_error(error, "ELF image has no usable symbol table");
        return false;
    }
    std::unordered_set<std::string> seen;
    const auto append = [&](const SymbolTable &table, bool dynamic) {
        if (table.symbols == nullptr || table.symbol_count == 0) {
            return;
        }
        const auto *bytes = static_cast<const unsigned char *>(table.symbols);
        for (size_t index = 0; index < table.symbol_count; ++index) {
            const Elf64_Sym symbol =
                read_unaligned<Elf64_Sym>(bytes + index * table.symbol_entry_size);
            uintptr_t address = 0;
            if (symbol.st_name >= table.strings_size ||
                !symbol_is_resolvable(symbol, load_bias_, &address)) {
                continue;
            }
            const char *candidate = table.strings + symbol.st_name;
            const size_t available = table.strings_size - symbol.st_name;
            const void *terminator = memchr(candidate, '\0', available);
            if (terminator == nullptr) {
                continue;
            }
            std::string name(candidate, static_cast<const char *>(terminator) - candidate);
            if (name.empty()) {
                continue;
            }
            if (!seen.insert(name).second) {
                continue;
            }
            ArtElfSymbol entry;
            entry.name = std::move(name);
            entry.type = ELF64_ST_TYPE(symbol.st_info);
            entry.bind = ELF64_ST_BIND(symbol.st_info);
            entry.address = address;
            entry.dynamic = dynamic;
            symbols->push_back(std::move(entry));
        }
    };
    append(dynamic_, true);
    append(full_, false);
    append(debugdata_, false);
    return true;
}
