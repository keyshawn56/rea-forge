#pragma once
// Reader for the harness's custom asset container.
//
// Layout (all integers little-endian):
//   Header (32 bytes)
//     0  char     magic[4]      "RPAK"
//     4  uint32_t version       1
//     8  uint32_t flags         0
//    12  uint32_t entry_count
//    16  uint64_t index_offset
//    24  uint64_t reserved
//   Index (entry_count * 96 bytes)
//     0  char     name[64]      NUL-terminated, unique
//    64  uint64_t offset        file offset of the blob
//    72  uint64_t stored_size   bytes in the file
//    80  uint64_t original_size bytes after decoding
//    88  uint32_t codec         0 = raw, 1 = LZ4 block
//    92  uint32_t crc32         CRC-32 of the decoded blob
//
// The container is intentionally simple and has no encryption, key material or
// title-specific compression. The reader does not write to the file.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rea {

struct PakError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct PakEntry {
    std::string name;
    uint64_t offset = 0;
    uint64_t stored_size = 0;
    uint64_t original_size = 0;
    uint32_t codec = 0;
    uint32_t crc = 0;
};

class PakArchive {
public:
    static std::unique_ptr<PakArchive> open(int fd);

    const std::vector<PakEntry>& entries() const { return entries_; }
    bool contains(const std::string& name) const;
    std::vector<uint8_t> read(const std::string& name) const;
    uint64_t fileSize() const { return file_size_; }

private:
    PakArchive(int fd, uint64_t file_size) : fd_(fd), file_size_(file_size) {}
    void readAt(void* destination, size_t size, uint64_t offset) const;
    const PakEntry* find(const std::string& name) const;

    int fd_ = -1;
    uint64_t file_size_ = 0;
    std::vector<PakEntry> entries_;
};

}  // namespace rea
