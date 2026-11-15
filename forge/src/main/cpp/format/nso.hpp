#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace forge {

struct NsoSection {
    std::string name;
    uint32_t file_offset = 0;
    uint32_t memory_offset = 0;
    uint32_t memory_size = 0;
    uint32_t file_size = 0;
    bool compressed = false;
    bool hash_verified = false;
    std::string stored_hash;
    std::string actual_hash;
    std::vector<uint8_t> bytes;
};

struct NsoImage {
    std::string source_name;
    uint32_t version = 0;
    uint32_t flags = 0;
    uint32_t bss_size = 0;
    uint32_t module_name_size = 0;
    std::string module_name;
    std::array<uint8_t, 32> module_id{};
    uint32_t embedded_offset = 0;  // relative to .rodata
    uint32_t embedded_size = 0;
    uint32_t dynstr_offset = 0;
    uint32_t dynstr_size = 0;
    uint32_t dynsym_offset = 0;
    uint32_t dynsym_size = 0;
    std::vector<NsoSection> sections;
    uint64_t file_bytes = 0;
};

// Parses and decompresses an NSO0 image. Compressed sections are raw LZ4
// blocks. ZBIC (flag 0x80) is rejected because its frame format is not LZ4.
// Hashes are checked when the corresponding header flag is present.
NsoImage loadNso(int fd, const std::string& source_name, bool keep_bytes = true);

// LZ4 block decoder exposed for tests and for the asset pipeline.
std::vector<uint8_t> lz4BlockDecompress(const uint8_t* src, std::size_t src_size,
                                        std::size_t expected_size);

std::string nsoToJson(const NsoImage& image, bool include_sites = false);

}  // namespace forge
