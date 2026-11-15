#pragma once
// AArch64 ELF64 loader for the REA harness.
//
// This loader maps a statically linked AArch64 Linux ELF (ET_EXEC or ET_DYN)
// into anonymous memory at its link-time addresses, applies dynamic
// relocations, and exposes sections/symbols to the runtime. It never executes
// file-backed mappings: text is copied into anonymous pages, written once, then
// made read+execute. That keeps W^X at steady state and avoids Android's
// file-exec restriction.
//
// It is deliberately not a Horizon/NVN loader. PT_INTERP and DT_NEEDED are
// rejected: this harness hosts self-contained images, with host services
// reached through the HLE resolver below.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace rea {

struct ElfError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ElfSection {
    std::string name;
    uint32_t type = 0;
    uint64_t flags = 0;
    uint64_t address = 0;   // already biased for ET_DYN
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t entry_size = 0;
    uint32_t link = 0;
    uint32_t info = 0;
};

struct ElfSymbol {
    std::string name;
    uint64_t value = 0;     // already biased for ET_DYN, 0 when undefined
    uint64_t size = 0;
    uint8_t bind = 0;
    uint8_t type = 0;
    uint8_t visibility = 0;
    uint16_t section_index = 0;
    bool defined = false;
    bool weak = false;
};

struct ElfSegment {
    uint64_t vaddr = 0;     // link-time virtual address (unbiased)
    uint64_t memory_size = 0;
    uint64_t file_size = 0;
    uint64_t file_offset = 0;
    uint32_t flags = 0;     // PF_X=1 PF_W=2 PF_R=4
};

struct LoadOptions {
    // level: 0 info, 1 warning, 2 error
    std::function<void(int, const std::string&)> log;
    // Resolves an undefined GLOB_DAT/JUMP_SLOT symbol to a host function pointer.
    // This is the HLE ABI thunk table hook. Return 0 when unresolved.
    std::function<uint64_t(const std::string&)> resolve_hle_symbol;
    bool allow_writable_executable = false;
};

struct LoadedImage {
    int fd = -1;                    // borrowed; the owner keeps it open
    bool pie = false;
    uint64_t mapping_address = 0;   // mmap base, for munmap
    uint64_t mapping_size = 0;
    uint64_t load_bias = 0;         // 0 for ET_EXEC, mmap base for ET_DYN
    uint64_t entry = 0;
    uint64_t program_headers = 0;   // AT_PHDR
    uint16_t program_header_count = 0;
    uint16_t program_header_size = 0;
    uint64_t tls_address = 0;
    uint64_t tls_file_size = 0;
    uint64_t tls_memory_size = 0;
    uint64_t tls_alignment = 0;
    std::vector<uint8_t> build_id;
    std::vector<ElfSegment> segments;
    std::vector<ElfSection> sections;
    std::vector<ElfSymbol> symbols;
    std::unordered_map<std::string, uint64_t> exports;
    std::vector<std::pair<uint64_t, uint64_t>> executable_ranges;
    std::string soname;

    LoadedImage() = default;
    LoadedImage(const LoadedImage&) = delete;
    LoadedImage& operator=(const LoadedImage&) = delete;
    ~LoadedImage();

    // True when [address, address+size) is inside a PT_LOAD memory range.
    bool contains(uint64_t address, uint64_t size) const;
};

class ElfLoader {
public:
    static std::unique_ptr<LoadedImage> load(int fd, const LoadOptions& options);
};

// Utility shared with the pak reader and the boot manifest.
uint32_t crc32(const void* data, size_t size, uint32_t seed = 0);

}  // namespace rea
