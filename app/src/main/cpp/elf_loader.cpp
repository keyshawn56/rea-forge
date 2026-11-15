#include "elf_loader.hpp"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#ifndef R_AARCH64_COPY
#define R_AARCH64_COPY 1024
#endif
#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif
#ifndef R_AARCH64_RELATIVE
#define R_AARCH64_RELATIVE 1027
#endif
#ifndef R_AARCH64_TLS_TPREL64
#define R_AARCH64_TLS_TPREL64 1030
#endif
#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE 1032
#endif

#ifndef NT_GNU_BUILD_ID
#define NT_GNU_BUILD_ID 3
#endif

namespace rea {
namespace {

constexpr uint64_t kMaxImageSize = 16ull * 1024 * 1024 * 1024;
constexpr uint64_t kMaxStringTable = 64ull * 1024 * 1024;
constexpr size_t kSymbolEntrySize = sizeof(Elf64_Sym);
constexpr size_t kRelaEntrySize = sizeof(Elf64_Rela);

void note(const LoadOptions& options, int level, const std::string& text) {
    if (options.log) {
        options.log(level, text);
    }
}

std::string hex(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%" PRIx64, value);
    return buffer;
}

uint64_t pageSize() {
    const long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<uint64_t>(value) : 4096ull;
}

uint64_t alignDown(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool addOverflows(uint64_t a, uint64_t b) {
    return a > std::numeric_limits<uint64_t>::max() - b;
}

class FileReader {
public:
    FileReader(int fd, uint64_t size) : fd_(fd), size_(size) {}

    void read(void* destination, size_t count, uint64_t offset) const {
        if (addOverflows(offset, count) || offset + count > size_) {
            throw ElfError("read outside file: offset " + hex(offset) + " size " + hex(count));
        }
        auto* out = static_cast<uint8_t*>(destination);
        size_t done = 0;
        while (done < count) {
            const ssize_t got = pread(fd_, out + done, count - done,
                                      static_cast<off_t>(offset + done));
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw ElfError(std::string("pread failed: ") + std::strerror(errno));
            }
            if (got == 0) {
                throw ElfError("unexpected EOF while reading ELF");
            }
            done += static_cast<size_t>(got);
        }
    }

    uint64_t size() const { return size_; }

private:
    int fd_;
    uint64_t size_;
};

struct DynamicInfo {
    uint64_t rela = 0;
    uint64_t rela_size = 0;
    uint64_t rela_entry = 0;
    uint64_t jmprel = 0;
    uint64_t jmprel_size = 0;
    uint64_t relr = 0;
    uint64_t relr_size = 0;
    uint64_t relr_entry = 0;
    uint64_t strtab = 0;
    uint64_t symtab = 0;
    uint64_t strsz = 0;
    uint64_t syment = 0;
    bool textrel = false;
    std::vector<std::string> needed;
    std::string soname;
};

uint8_t protFromFlags(uint32_t flags) {
    uint8_t prot = 0;
    if (flags & PF_R) prot |= PROT_READ;
    if (flags & PF_W) prot |= PROT_WRITE;
    if (flags & PF_X) prot |= PROT_EXEC;
    return prot;
}

}  // namespace

LoadedImage::~LoadedImage() {
    if (mapping_address != 0 && mapping_size != 0) {
        munmap(reinterpret_cast<void*>(mapping_address), static_cast<size_t>(mapping_size));
    }
}

bool LoadedImage::contains(uint64_t address, uint64_t size) const {
    if (addOverflows(address, size)) {
        return false;
    }
    for (const auto& segment : segments) {
        const uint64_t start = load_bias + segment.vaddr;
        if (addOverflows(start, segment.memory_size)) {
            continue;
        }
        const uint64_t end = start + segment.memory_size;
        if (address >= start && address + size <= end) {
            return true;
        }
    }
    return false;
}

uint32_t crc32(const void* data, size_t size, uint32_t seed) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            values[i] = c;
        }
        return values;
    }();

    uint32_t value = ~seed;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        value = table[(value ^ bytes[i]) & 0xFFu] ^ (value >> 8);
    }
    return ~value;
}

std::unique_ptr<LoadedImage> ElfLoader::load(int fd, const LoadOptions& options) {
    struct stat status {};
    if (fstat(fd, &status) != 0) {
        throw ElfError(std::string("fstat failed: ") + std::strerror(errno));
    }
    if (status.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr))) {
        throw ElfError("file is smaller than an ELF64 header");
    }
    const FileReader file(fd, static_cast<uint64_t>(status.st_size));

    Elf64_Ehdr header{};
    file.read(&header, sizeof(header), 0);

    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0) {
        throw ElfError("bad ELF magic");
    }
    if (header.e_ident[EI_CLASS] != ELFCLASS64) {
        throw ElfError("not ELF64");
    }
    if (header.e_ident[EI_DATA] != ELFDATA2LSB) {
        throw ElfError("not little-endian");
    }
    if (header.e_ident[EI_VERSION] != EV_CURRENT || header.e_version != EV_CURRENT) {
        throw ElfError("unsupported ELF version");
    }
    if (header.e_machine != EM_AARCH64) {
        throw ElfError("ELF machine is not AArch64 (EM_AARCH64)");
    }
    if (header.e_type != ET_EXEC && header.e_type != ET_DYN) {
        throw ElfError("only ET_EXEC and ET_DYN are supported");
    }
    if (header.e_ehsize < sizeof(Elf64_Ehdr) ||
        header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 ||
        header.e_phnum > 4096) {
        throw ElfError("invalid program header table");
    }

    std::vector<Elf64_Phdr> phdrs(header.e_phnum);
    file.read(phdrs.data(), phdrs.size() * sizeof(Elf64_Phdr), header.e_phoff);

    auto image = std::make_unique<LoadedImage>();
    image->fd = fd;
    image->pie = header.e_type == ET_DYN;
    image->program_header_count = header.e_phnum;
    image->program_header_size = header.e_phentsize;

    uint64_t min_vaddr = std::numeric_limits<uint64_t>::max();
    uint64_t max_vaddr = 0;

    for (const auto& phdr : phdrs) {
        if (phdr.p_type == PT_INTERP && phdr.p_filesz > 0) {
            throw ElfError("PT_INTERP is not supported; supply a self-contained image");
        }
        if (phdr.p_type != PT_LOAD) {
            continue;
        }
        if (phdr.p_filesz > phdr.p_memsz) {
            throw ElfError("PT_LOAD file size exceeds memory size");
        }
        if (addOverflows(phdr.p_offset, phdr.p_filesz) ||
            phdr.p_offset + phdr.p_filesz > file.size()) {
            throw ElfError("PT_LOAD file range is outside the file");
        }
        if (phdr.p_memsz == 0) {
            continue;
        }
        if (phdr.p_vaddr + phdr.p_memsz < phdr.p_vaddr) {
            throw ElfError("PT_LOAD virtual range overflows");
        }
        min_vaddr = std::min<uint64_t>(min_vaddr, phdr.p_vaddr);
        max_vaddr = std::max<uint64_t>(max_vaddr, phdr.p_vaddr + phdr.p_memsz);
        image->segments.push_back(ElfSegment{
            phdr.p_vaddr, phdr.p_memsz, phdr.p_filesz, phdr.p_offset, phdr.p_flags});
        note(options, 0, "PT_LOAD vaddr=" + hex(phdr.p_vaddr) +
                             " filesz=" + hex(phdr.p_filesz) +
                             " memsz=" + hex(phdr.p_memsz) +
                             " flags=" + std::to_string(phdr.p_flags));
    }

    if (image->segments.empty() || min_vaddr == std::numeric_limits<uint64_t>::max()) {
        throw ElfError("ELF has no loadable segments");
    }

    const uint64_t page = pageSize();
    if (max_vaddr > std::numeric_limits<uint64_t>::max() - page) {
        throw ElfError("image span overflows the address space");
    }
    const uint64_t map_start = alignDown(min_vaddr, page);
    const uint64_t map_end = alignUp(max_vaddr, page);
    const uint64_t map_size = map_end - map_start;
    if (map_size == 0 || map_size > kMaxImageSize) {
        throw ElfError("image span is implausible: " + hex(map_size));
    }

    int map_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    void* mapping = MAP_FAILED;
    if (image->pie) {
        mapping = mmap(nullptr, static_cast<size_t>(map_size), PROT_NONE, map_flags, -1, 0);
        if (mapping == MAP_FAILED) {
            throw ElfError(std::string("mmap(image) failed: ") + std::strerror(errno));
        }
        image->mapping_address = reinterpret_cast<uint64_t>(mapping);
    } else {
        if (map_start < 0x10000ull) {
            throw ElfError("ET_EXEC image maps too close to address zero");
        }
        mapping = mmap(reinterpret_cast<void*>(map_start), static_cast<size_t>(map_size),
                       PROT_NONE, map_flags | MAP_FIXED_NOREPLACE, -1, 0);
        if (mapping == MAP_FAILED) {
            throw ElfError("cannot reserve ET_EXEC range " + hex(map_start) + "-" +
                           hex(map_start + map_size) + ": " + std::strerror(errno));
        }
        image->mapping_address = reinterpret_cast<uint64_t>(mapping);
    }
    image->mapping_size = map_size;
    // The bias makes load_bias + min_vaddr equal the first reserved page. For
    // ET_EXEC this is exactly zero, because the mapping was placed at the
    // link-time address. For ET_DYN it accounts for a non-zero p_vaddr base.
    image->load_bias = image->mapping_address - map_start;
    image->entry = image->load_bias + header.e_entry;

    // Make every segment writable long enough to copy, zero BSS and relocate.
    for (const auto& segment : image->segments) {
        const uint64_t start = alignDown(image->load_bias + segment.vaddr, page);
        const uint64_t end = alignUp(image->load_bias + segment.vaddr + segment.memory_size, page);
        if (mprotect(reinterpret_cast<void*>(start), static_cast<size_t>(end - start),
                     PROT_READ | PROT_WRITE) != 0) {
            throw ElfError(std::string("mprotect(copy) failed: ") + std::strerror(errno));
        }
    }

    for (const auto& segment : image->segments) {
        if (segment.file_size > 0) {
            file.read(reinterpret_cast<void*>(image->load_bias + segment.vaddr),
                      static_cast<size_t>(segment.file_size), segment.file_offset);
        }
        if (segment.memory_size > segment.file_size) {
            void* bss = reinterpret_cast<void*>(image->load_bias + segment.vaddr +
                                                segment.file_size);
            std::memset(bss, 0, static_cast<size_t>(segment.memory_size - segment.file_size));
        }
        if (segment.flags & PF_X) {
            image->executable_ranges.emplace_back(image->load_bias + segment.vaddr,
                                                  segment.memory_size);
        }
    }

    // AT_PHDR: locate the PT_LOAD that contains the program header table.
    const uint64_t phdr_bytes = static_cast<uint64_t>(header.e_phnum) * header.e_phentsize;
    for (const auto& segment : image->segments) {
        if (header.e_phoff >= segment.file_offset &&
            header.e_phoff + phdr_bytes <= segment.file_offset + segment.file_size) {
            image->program_headers = image->load_bias + segment.vaddr +
                                     (header.e_phoff - segment.file_offset);
            break;
        }
    }

    if (image->program_headers == 0) {
        note(options, 1, "AT_PHDR could not be derived; libc auxv initialisation may fail");
    }

    for (const auto& phdr : phdrs) {
        if (phdr.p_type == PT_TLS) {
            image->tls_address = image->load_bias + phdr.p_vaddr;
            image->tls_file_size = phdr.p_filesz;
            image->tls_memory_size = phdr.p_memsz;
            image->tls_alignment = phdr.p_align;
        } else if (phdr.p_type == PT_GNU_STACK && (phdr.p_flags & PF_X)) {
            note(options, 1, "PT_GNU_STACK requests an executable stack");
        } else if (phdr.p_type == PT_NOTE && phdr.p_filesz > 0 &&
                   phdr.p_filesz <= 1024 * 1024) {
            // Build ID notes are read from the file; the note segment need not
            // be part of a PT_LOAD.
            std::vector<uint8_t> notes(static_cast<size_t>(phdr.p_filesz));
            file.read(notes.data(), notes.size(), phdr.p_offset);
            size_t at = 0;
            while (at + 12 <= notes.size()) {
                uint32_t namesz = 0;
                uint32_t descsz = 0;
                uint32_t type = 0;
                std::memcpy(&namesz, notes.data() + at, 4);
                std::memcpy(&descsz, notes.data() + at + 4, 4);
                std::memcpy(&type, notes.data() + at + 8, 4);
                const size_t name_at = at + 12;
                const size_t desc_at = alignUp(name_at + namesz, 4);
                const size_t next = alignUp(desc_at + descsz, 4);
                if (next > notes.size() || desc_at + descsz > notes.size()) {
                    break;
                }
                if (type == NT_GNU_BUILD_ID && namesz >= 4 &&
                    std::memcmp(notes.data() + name_at, "GNU", 4) == 0) {
                    image->build_id.assign(notes.begin() + static_cast<ptrdiff_t>(desc_at),
                                           notes.begin() + static_cast<ptrdiff_t>(desc_at + descsz));
                }
                at = next;
            }
        }
    }

    // Section headers are optional (fully stripped images still load), but they
    // give us names for the log and a reliable .dynsym bound.
    uint64_t dynsym_count = 0;
    if (header.e_shoff != 0 && header.e_shentsize == sizeof(Elf64_Shdr)) {
        uint64_t shnum = header.e_shnum;
        if (shnum == 0) {
            Elf64_Shdr first{};
            file.read(&first, sizeof(first), header.e_shoff);
            shnum = first.sh_size;
        }
        if (shnum > 0 && shnum < 65536) {
            std::vector<Elf64_Shdr> shdrs(shnum);
            file.read(shdrs.data(), shdrs.size() * sizeof(Elf64_Shdr), header.e_shoff);
            uint64_t shstrndx = header.e_shstrndx;
            if (shstrndx == SHN_XINDEX) {
                shstrndx = shdrs[0].sh_link;
            }
            std::vector<char> shstr;
            if (shstrndx < shdrs.size() && shdrs[shstrndx].sh_size > 0 &&
                shdrs[shstrndx].sh_size < kMaxStringTable) {
                shstr.resize(static_cast<size_t>(shdrs[shstrndx].sh_size));
                file.read(shstr.data(), shstr.size(), shdrs[shstrndx].sh_offset);
            }
            for (size_t i = 0; i < shdrs.size(); ++i) {
                const auto& sh = shdrs[i];
                std::string name;
                if (!shstr.empty() && sh.sh_name < shstr.size()) {
                    name = shstr.data() + sh.sh_name;
                }
                image->sections.push_back(ElfSection{
                    name, sh.sh_type, sh.sh_flags, image->load_bias + sh.sh_addr,
                    sh.sh_offset, sh.sh_size, sh.sh_entsize, sh.sh_link, sh.sh_info});
                if (sh.sh_type == SHT_DYNSYM && sh.sh_entsize >= kSymbolEntrySize) {
                    dynsym_count = sh.sh_size / sh.sh_entsize;
                }
            }
        }
    }

    const auto findSection = [&](const std::string& name) -> const ElfSection* {
        for (const auto& section : image->sections) {
            if (section.name == name) return &section;
        }
        return nullptr;
    };

    // Symbol tables: keep both .symtab and .dynsym in the diagnostic vector,
    // but only defined globals are exported for HLE binding.
    for (const auto& section : image->sections) {
        if (section.type != SHT_SYMTAB && section.type != SHT_DYNSYM) {
            continue;
        }
        if (section.entry_size < kSymbolEntrySize || section.link >= image->sections.size()) {
            continue;
        }
        const auto& strings = image->sections[section.link];
        if (strings.size == 0 || strings.size > kMaxStringTable) {
            continue;
        }
        std::vector<char> strtab(static_cast<size_t>(strings.size));
        file.read(strtab.data(), strtab.size(), strings.offset);
        const uint64_t count = section.size / section.entry_size;
        if (count > 2'000'000) {
            continue;
        }
        for (uint64_t i = 0; i < count; ++i) {
            Elf64_Sym raw{};
            file.read(&raw, sizeof(raw), section.offset + i * section.entry_size);
            if (raw.st_name >= strtab.size()) {
                continue;
            }
            const char* raw_name = strtab.data() + raw.st_name;
            std::string name(raw_name, strnlen(raw_name, strtab.size() - raw.st_name));
            const bool defined = raw.st_shndx != SHN_UNDEF;
            ElfSymbol symbol;
            symbol.name = name;
            symbol.size = raw.st_size;
            symbol.bind = ELF64_ST_BIND(raw.st_info);
            symbol.type = ELF64_ST_TYPE(raw.st_info);
            symbol.visibility = raw.st_other & 0x3;
            symbol.section_index = raw.st_shndx;
            symbol.defined = defined;
            symbol.weak = symbol.bind == STB_WEAK;
            symbol.value = defined
                               ? (raw.st_shndx == SHN_ABS ? raw.st_value
                                                          : image->load_bias + raw.st_value)
                               : 0;
            image->symbols.push_back(symbol);
            if (defined && !name.empty() &&
                (symbol.bind == STB_GLOBAL || symbol.bind == STB_WEAK)) {
                auto it = image->exports.find(name);
                if (it == image->exports.end() || symbol.bind == STB_GLOBAL) {
                    image->exports[name] = symbol.value;
                }
            }
        }
    }

    DynamicInfo dynamic;
    for (const auto& phdr : phdrs) {
        if (phdr.p_type != PT_DYNAMIC || phdr.p_memsz == 0) {
            continue;
        }
        const uint64_t base = image->load_bias + phdr.p_vaddr;
        if (!image->contains(base, phdr.p_memsz)) {
            throw ElfError("PT_DYNAMIC is not inside a loaded segment");
        }
        const auto* entries = reinterpret_cast<const Elf64_Dyn*>(base);
        const uint64_t count = phdr.p_memsz / sizeof(Elf64_Dyn);
        for (uint64_t i = 0; i < count; ++i) {
            const auto& entry = entries[i];
            if (entry.d_tag == DT_NULL) break;
            switch (entry.d_tag) {
                case DT_NEEDED:
                case DT_SONAME:
                case DT_RPATH:
                case DT_RUNPATH:
                    // String-valued tags are resolved after DT_STRTAB is known.
                    break;
                case DT_RELA: dynamic.rela = entry.d_un.d_ptr; break;
                case DT_RELASZ: dynamic.rela_size = entry.d_un.d_val; break;
                case DT_RELAENT: dynamic.rela_entry = entry.d_un.d_val; break;
                case DT_JMPREL: dynamic.jmprel = entry.d_un.d_ptr; break;
                case DT_PLTRELSZ: dynamic.jmprel_size = entry.d_un.d_val; break;
                case DT_PLTREL:
                    if (entry.d_un.d_val != DT_RELA) {
                        throw ElfError("DT_PLTREL is not DT_RELA");
                    }
                    break;
                case DT_RELR: dynamic.relr = entry.d_un.d_ptr; break;
                case DT_RELRSZ: dynamic.relr_size = entry.d_un.d_val; break;
                case DT_RELRENT: dynamic.relr_entry = entry.d_un.d_val; break;
                case DT_STRTAB: dynamic.strtab = entry.d_un.d_ptr; break;
                case DT_SYMTAB: dynamic.symtab = entry.d_un.d_ptr; break;
                case DT_STRSZ: dynamic.strsz = entry.d_un.d_val; break;
                case DT_SYMENT: dynamic.syment = entry.d_un.d_val; break;
                case DT_TEXTREL: dynamic.textrel = true; break;
                case DT_HASH: {
                    const uint64_t hash = image->load_bias + entry.d_un.d_ptr;
                    if (image->contains(hash, 8) && dynsym_count == 0) {
                        uint32_t nchain = 0;
                        std::memcpy(&nchain, reinterpret_cast<const void*>(hash + 4), 4);
                        dynsym_count = nchain;
                    }
                    break;
                }
                default: break;
            }
        }
    }

    if (dynamic.strtab != 0) {
        dynamic.strtab += image->load_bias;
    }
    if (dynamic.symtab != 0) {
        dynamic.symtab += image->load_bias;
    }

    // Resolve string-valued dynamic tags now that DT_STRTAB is available.
    if (dynamic.strtab != 0 && dynamic.strsz > 0 && dynamic.strsz < kMaxStringTable) {
        for (const auto& phdr : phdrs) {
            if (phdr.p_type != PT_DYNAMIC || phdr.p_memsz == 0) continue;
            const auto* entries = reinterpret_cast<const Elf64_Dyn*>(
                image->load_bias + phdr.p_vaddr);
            for (uint64_t i = 0; i < phdr.p_memsz / sizeof(Elf64_Dyn); ++i) {
                const auto& entry = entries[i];
                if (entry.d_tag == DT_NULL) break;
                if (entry.d_tag != DT_NEEDED && entry.d_tag != DT_SONAME) continue;
                if (entry.d_un.d_val >= dynamic.strsz) continue;
                const char* text = reinterpret_cast<const char*>(dynamic.strtab +
                                                                 entry.d_un.d_val);
                const size_t length = strnlen(text, dynamic.strsz - entry.d_un.d_val);
                if (entry.d_tag == DT_NEEDED) {
                    dynamic.needed.emplace_back(text, length);
                } else {
                    dynamic.soname.assign(text, length);
                }
            }
        }
    }
    image->soname = dynamic.soname;

    if (!dynamic.needed.empty()) {
        std::string joined;
        for (const auto& name : dynamic.needed) {
            if (!joined.empty()) joined += ", ";
            joined += name;
        }
        throw ElfError("dynamic dependencies are not supported: " + joined);
    }

    if (dynsym_count == 0 && dynamic.strtab > dynamic.symtab && dynamic.symtab != 0 &&
        dynamic.strtab - dynamic.symtab < 64ull * 1024 * 1024) {
        dynsym_count = (dynamic.strtab - dynamic.symtab) / kSymbolEntrySize;
    }
    if (dynamic.syment != 0 && dynamic.syment != kSymbolEntrySize) {
        throw ElfError("unsupported DT_SYMENT");
    }
    if (dynamic.textrel) {
        note(options, 1, "DT_TEXTREL present; text pages are writable during relocation only");
    }

    const auto symbolAt = [&](uint32_t index, std::string* name,
                              bool* weak) -> uint64_t {
        *name = {};
        *weak = false;
        if (index == 0) return 0;
        if (dynamic.symtab == 0 || index >= dynsym_count) {
            throw ElfError("relocation references invalid dynamic symbol " +
                           std::to_string(index));
        }
        Elf64_Sym raw{};
        std::memcpy(&raw, reinterpret_cast<const void*>(dynamic.symtab +
                                                        index * kSymbolEntrySize),
                    sizeof(raw));
        if (dynamic.strtab != 0 && raw.st_name < dynamic.strsz) {
            const char* text = reinterpret_cast<const char*>(dynamic.strtab + raw.st_name);
            *name = std::string(text, strnlen(text, dynamic.strsz - raw.st_name));
        }
        *weak = ELF64_ST_BIND(raw.st_info) == STB_WEAK;
        if (raw.st_shndx == SHN_UNDEF) {
            return 0;
        }
        return raw.st_shndx == SHN_ABS ? raw.st_value
                                        : image->load_bias + raw.st_value;
    };

    const auto writeRelocation = [&](uint64_t target, uint64_t value, uint32_t type) {
        if (!image->contains(target, sizeof(uint64_t))) {
            throw ElfError("relocation target " + hex(target) + " is outside the image");
        }
        std::memcpy(reinterpret_cast<void*>(target), &value, sizeof(value));
        (void)type;
    };

    size_t relocation_count = 0;
    const auto processRela = [&](uint64_t address, uint64_t byte_size) {
        if (byte_size == 0) return;
        if (!image->contains(address, byte_size)) {
            throw ElfError("RELA table is outside the image");
        }
        if (byte_size % kRelaEntrySize != 0) {
            throw ElfError("RELA table has a partial entry");
        }
        const auto* entries = reinterpret_cast<const Elf64_Rela*>(address);
        const size_t count = static_cast<size_t>(byte_size / kRelaEntrySize);
        for (size_t i = 0; i < count; ++i) {
            const auto& rela = entries[i];
            const uint32_t type = static_cast<uint32_t>(ELF64_R_TYPE(rela.r_info));
            const uint32_t sym_index = static_cast<uint32_t>(ELF64_R_SYM(rela.r_info));
            const uint64_t target = image->load_bias + rela.r_offset;

            if (type == R_AARCH64_NONE) {
                ++relocation_count;
                continue;
            }
            if (type == R_AARCH64_RELATIVE) {
                writeRelocation(target, image->load_bias + rela.r_addend, type);
                ++relocation_count;
                continue;
            }

            std::string symbol_name;
            bool weak = false;
            const uint64_t symbol_value = symbolAt(sym_index, &symbol_name, &weak);
            switch (type) {
                case R_AARCH64_ABS64:
                    writeRelocation(target, symbol_value + rela.r_addend, type);
                    break;
                case R_AARCH64_GLOB_DAT:
                case R_AARCH64_JUMP_SLOT: {
                    uint64_t value = symbol_value;
                    if (value == 0 && sym_index != 0) {
                        if (options.resolve_hle_symbol && !symbol_name.empty()) {
                            value = options.resolve_hle_symbol(symbol_name);
                        }
                        if (value == 0 && !weak) {
                            throw ElfError("unresolved symbol '" + symbol_name +
                                           "' at relocation " + hex(rela.r_offset));
                        }
                    }
                    writeRelocation(target, value + rela.r_addend, type);
                    break;
                }
                default:
                    throw ElfError("unsupported AArch64 relocation type " +
                                   std::to_string(type) + " for '" + symbol_name + "'");
            }
            ++relocation_count;
        }
    };

    if (dynamic.rela_entry != 0 && dynamic.rela_entry != kRelaEntrySize) {
        throw ElfError("unsupported DT_RELAENT");
    }
    if (dynamic.rela != 0 && dynamic.rela_size != 0) {
        processRela(image->load_bias + dynamic.rela, dynamic.rela_size);
    }
    if (dynamic.jmprel != 0 && dynamic.jmprel_size != 0) {
        processRela(image->load_bias + dynamic.jmprel, dynamic.jmprel_size);
    }

    if (dynamic.relr != 0 && dynamic.relr_size != 0) {
        if (dynamic.relr_entry != sizeof(uint64_t)) {
            throw ElfError("unsupported DT_RELRENT");
        }
        const uint64_t table = image->load_bias + dynamic.relr;
        if (!image->contains(table, dynamic.relr_size)) {
            throw ElfError("RELR table is outside the image");
        }
        const auto* entries = reinterpret_cast<const uint64_t*>(table);
        const size_t count = static_cast<size_t>(dynamic.relr_size / 8);
        uint64_t where = 0;
        for (size_t i = 0; i < count; ++i) {
            const uint64_t entry = entries[i];
            if ((entry & 1ull) == 0) {
                where = entry;
                const uint64_t target = image->load_bias + where;
                uint64_t addend = 0;
                if (!image->contains(target, 8)) {
                    throw ElfError("RELR target is outside the image");
                }
                std::memcpy(&addend, reinterpret_cast<const void*>(target), 8);
                writeRelocation(target, image->load_bias + addend, 0);
                where += 8;
            } else {
                uint64_t bits = entry >> 1;
                for (int bit = 0; bit < 63; ++bit) {
                    if ((bits & (1ull << bit)) == 0) continue;
                    const uint64_t target = image->load_bias + where + bit * 8;
                    uint64_t addend = 0;
                    if (!image->contains(target, 8)) {
                        throw ElfError("RELR bitmap target is outside the image");
                    }
                    std::memcpy(&addend, reinterpret_cast<const void*>(target), 8);
                    writeRelocation(target, image->load_bias + addend, 0);
                }
                where += 63 * 8;
            }
        }
    }

    // Final W^X protection, computed per host page because 16 KB-page devices
    // may back several 4 KB-aligned ELF segments with one host page.
    const uint64_t page_count = map_size / page;
    std::vector<uint8_t> page_prot(static_cast<size_t>(page_count), 0);
    for (const auto& segment : image->segments) {
        const uint64_t start = alignDown(image->load_bias + segment.vaddr, page);
        const uint64_t end = alignUp(image->load_bias + segment.vaddr + segment.memory_size,
                                     page);
        const uint8_t prot = protFromFlags(segment.flags);
        for (uint64_t at = start; at < end; at += page) {
            if (at < image->mapping_address ||
                at >= image->mapping_address + map_size) {
                throw ElfError("segment falls outside the reserved mapping");
            }
            page_prot[static_cast<size_t>((at - image->mapping_address) / page)] |= prot;
        }
    }
    for (size_t i = 0; i < page_prot.size(); ++i) {
        uint8_t prot = page_prot[i];
        if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
            if (!options.allow_writable_executable) {
                prot &= ~PROT_WRITE;
                note(options, 1, "W^X: dropping write permission on a shared code page");
            }
        }
        page_prot[i] = prot;
    }
    for (size_t i = 0; i < page_prot.size();) {
        size_t j = i + 1;
        while (j < page_prot.size() && page_prot[j] == page_prot[i]) ++j;
        void* address = reinterpret_cast<void*>(image->mapping_address + i * page);
        const size_t length = (j - i) * page;
        if (mprotect(address, length, page_prot[i]) != 0) {
            throw ElfError(std::string("mprotect(final) failed: ") + std::strerror(errno));
        }
        i = j;
    }

    note(options, 0, std::string("ELF ") + (image->pie ? "ET_DYN" : "ET_EXEC") +
                         " base=" + hex(image->load_bias) + " entry=" + hex(image->entry) +
                         " symbols=" + std::to_string(image->symbols.size()) +
                         " relocations=" + std::to_string(relocation_count));
    if (!image->build_id.empty()) {
        std::string id;
        char byte[3];
        for (uint8_t value : image->build_id) {
            std::snprintf(byte, sizeof(byte), "%02x", value);
            id += byte;
        }
        note(options, 0, "build-id " + id);
    }
    if (const auto* text = findSection(".text")) {
        note(options, 0, ".text addr=" + hex(text->address) + " size=" + hex(text->size));
    }
    if (const auto* rodata = findSection(".rodata")) {
        note(options, 0, ".rodata addr=" + hex(rodata->address) +
                             " size=" + hex(rodata->size));
    }
    if (const auto* data = findSection(".data")) {
        note(options, 0, ".data addr=" + hex(data->address) + " size=" + hex(data->size));
    }
    if (const auto* bss = findSection(".bss")) {
        note(options, 0, ".bss addr=" + hex(bss->address) + " size=" + hex(bss->size));
    }
    return image;
}

}  // namespace rea
