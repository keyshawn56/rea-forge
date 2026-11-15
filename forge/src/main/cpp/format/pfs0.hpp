#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace forge {

struct Pfs0Entry {
    std::string name;
    uint64_t offset = 0;  // relative to the PFS0 data region
    uint64_t size = 0;
    uint64_t absolute_offset = 0;
};

struct Pfs0Archive {
    uint64_t base = 0;
    uint32_t entry_count = 0;
    uint32_t string_table_size = 0;
    uint64_t data_offset = 0;  // relative to base
    uint64_t total_size = 0;
    std::vector<Pfs0Entry> entries;
};

// Parses a PFS0 at fd offset base. Bounds are checked against file_size.
Pfs0Archive parsePfs0(int fd, uint64_t base, uint64_t file_size);

// Streams one PFS0 entry into out_path. The caller has already validated the
// archive. Returns the number of bytes written.
uint64_t extractPfs0Entry(int fd, const Pfs0Archive& archive, const Pfs0Entry& entry,
                          const std::string& out_path);

struct NcaSectionInfo {
    bool present = false;
    uint64_t start_block = 0;
    uint64_t end_block = 0;
    uint32_t fs_version = 0;
    uint32_t fs_type = 0;
    uint32_t hash_type = 0;
    uint32_t encryption_type = 0;
};

struct NcaInfo {
    bool examined = false;
    bool plaintext_header = false;
    std::string magic;
    uint8_t distribution_type = 0;
    uint8_t content_type = 0;
    uint8_t key_generation_old = 0;
    uint8_t key_area_index = 0;
    uint64_t content_size = 0;
    uint64_t program_id = 0;
    uint32_t content_index = 0;
    uint32_t sdk_addon_version = 0;
    uint8_t key_generation = 0;
    std::string rights_id;
    std::vector<NcaSectionInfo> sections;
    std::string diagnostic;
};

// Inspects the (plaintext) NCA header at the start of an in-memory buffer. A
// retail NSP stores the header encrypted; in that case this function returns an
// NcaInfo with plaintext_header=false and does not attempt decryption.
NcaInfo inspectNcaHeader(const uint8_t* data, std::size_t size);

std::string nspToJson(const Pfs0Archive& archive, const std::vector<NcaInfo>& ncas,
                      const std::vector<uint8_t>& entry_kinds);

}  // namespace forge
