#include "art_elf_resolver.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <limits>

namespace {

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

bool checked_range(size_t offset, size_t size, size_t total) {
    return offset <= total && size <= total - offset;
}

void set_error(std::string *error, const char *message) {
    if (error != nullptr) {
        error->assign(message);
    }
}

}  // namespace

ArtElfResolver::~ArtElfResolver() {
    reset();
}

void ArtElfResolver::reset() {
    if (mapping_ != nullptr) {
        munmap(mapping_, mapping_size_);
    }
    mapping_ = nullptr;
    mapping_size_ = 0;
    load_bias_ = 0;
    dynamic_ = {};
    full_ = {};
}

bool ArtElfResolver::open_loaded(std::string_view soname, std::string *error) {
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
    mapping_ = mmap(nullptr, mapping_size_, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping_ == MAP_FAILED) {
        mapping_ = nullptr;
        mapping_size_ = 0;
        set_error(error, "unable to map loaded library image");
        return false;
    }
    load_bias_ = library.load_bias;
    if (!initialize_tables(error)) {
        reset();
        return false;
    }
    return true;
}

bool ArtElfResolver::initialize_tables(std::string *error) {
    if (!checked_range(0, sizeof(Elf64_Ehdr), mapping_size_)) {
        set_error(error, "truncated ELF header");
        return false;
    }
    const auto *header = static_cast<const Elf64_Ehdr *>(mapping_);
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
        header->e_ident[EI_CLASS] != ELFCLASS64 ||
        header->e_ident[EI_DATA] != ELFDATA2LSB ||
        header->e_shentsize != sizeof(Elf64_Shdr) || header->e_shnum == 0) {
        set_error(error, "unsupported ELF image");
        return false;
    }

    const size_t section_bytes = static_cast<size_t>(header->e_shnum) * sizeof(Elf64_Shdr);
    if (!checked_range(static_cast<size_t>(header->e_shoff), section_bytes, mapping_size_)) {
        set_error(error, "truncated ELF section table");
        return false;
    }
    const auto *sections = reinterpret_cast<const Elf64_Shdr *>(
        static_cast<const unsigned char *>(mapping_) + header->e_shoff);

    for (size_t index = 0; index < header->e_shnum; ++index) {
        const Elf64_Shdr &section = sections[index];
        if (section.sh_type != SHT_DYNSYM && section.sh_type != SHT_SYMTAB) {
            continue;
        }
        if (section.sh_link >= header->e_shnum || section.sh_entsize < sizeof(Elf64_Sym) ||
            section.sh_size % section.sh_entsize != 0 ||
            !checked_range(section.sh_offset, section.sh_size, mapping_size_)) {
            continue;
        }
        const Elf64_Shdr &strings = sections[section.sh_link];
        if (strings.sh_type != SHT_STRTAB ||
            !checked_range(strings.sh_offset, strings.sh_size, mapping_size_)) {
            continue;
        }

        SymbolTable table{
            static_cast<const unsigned char *>(mapping_) + section.sh_offset,
            static_cast<size_t>(section.sh_size / section.sh_entsize),
            static_cast<size_t>(section.sh_entsize),
            reinterpret_cast<const char *>(mapping_) + strings.sh_offset,
            static_cast<size_t>(strings.sh_size),
        };
        if (section.sh_type == SHT_DYNSYM) {
            dynamic_ = table;
        } else {
            full_ = table;
        }
    }
    if (dynamic_.symbols == nullptr && full_.symbols == nullptr) {
        set_error(error, "ELF image has no usable symbol table");
        return false;
    }
    return true;
}

void *ArtElfResolver::find_in(
    const SymbolTable &table,
    std::string_view name,
    bool prefix) const {
    if (table.symbols == nullptr || name.empty()) {
        return nullptr;
    }
    const auto *bytes = static_cast<const unsigned char *>(table.symbols);
    for (size_t index = 0; index < table.symbol_count; ++index) {
        const auto *symbol = reinterpret_cast<const Elf64_Sym *>(
            bytes + index * table.symbol_entry_size);
        if (symbol->st_name >= table.strings_size || symbol->st_shndx == SHN_UNDEF ||
            symbol->st_value == 0) {
            continue;
        }
        const char *candidate = table.strings + symbol->st_name;
        const size_t available = table.strings_size - symbol->st_name;
        const void *terminator = memchr(candidate, '\0', available);
        if (terminator == nullptr) {
            continue;
        }
        const size_t candidate_size =
            static_cast<const char *>(terminator) - candidate;
        const bool matches = prefix
            ? candidate_size >= name.size() && memcmp(candidate, name.data(), name.size()) == 0
            : candidate_size == name.size() && memcmp(candidate, name.data(), name.size()) == 0;
        if (matches) {
            return reinterpret_cast<void *>(load_bias_ + symbol->st_value);
        }
    }
    return nullptr;
}

void *ArtElfResolver::find(std::string_view name) const {
    if (void *address = find_in(dynamic_, name, false); address != nullptr) {
        return address;
    }
    return find_in(full_, name, false);
}

void *ArtElfResolver::find_prefix(std::string_view prefix) const {
    if (void *address = find_in(dynamic_, prefix, true); address != nullptr) {
        return address;
    }
    return find_in(full_, prefix, true);
}
