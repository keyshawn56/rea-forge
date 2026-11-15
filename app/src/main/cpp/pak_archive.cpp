#include "pak_archive.hpp"

#include "elf_loader.hpp"  // crc32

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

namespace rea {
namespace {

constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr size_t kEntrySize = 96;
constexpr size_t kMaxName = 64;
constexpr uint32_t kCodecRaw = 0;
constexpr uint32_t kCodecLz4 = 1;
constexpr uint32_t kMaxEntries = 65536;

uint32_t readU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t readU64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// LZ4 block format. Returns the number of decoded bytes, or 0 on malformed
// input. The destination must be exactly the advertised decoded size.
size_t lz4DecodeBlock(const uint8_t* source, size_t source_size, uint8_t* destination,
                      size_t destination_capacity) {
    size_t si = 0;
    size_t di = 0;
    while (si < source_size) {
        const uint8_t token = source[si++];
        size_t literals = token >> 4;
        if (literals == 15) {
            uint8_t extra = 0;
            do {
                if (si >= source_size) return 0;
                extra = source[si++];
                literals += extra;
            } while (extra == 255);
        }
        if (si + literals > source_size || di + literals > destination_capacity) {
            return 0;
        }
        std::memcpy(destination + di, source + si, literals);
        si += literals;
        di += literals;

        if (si >= source_size) {
            break;  // a final literal-only sequence is valid
        }
        if (si + 2 > source_size) {
            return 0;
        }
        const size_t offset = static_cast<size_t>(source[si]) |
                              (static_cast<size_t>(source[si + 1]) << 8);
        si += 2;
        if (offset == 0 || offset > di) {
            return 0;
        }
        size_t match = token & 0x0F;
        if (match == 15) {
            uint8_t extra = 0;
            do {
                if (si >= source_size) return 0;
                extra = source[si++];
                match += extra;
            } while (extra == 255);
        }
        match += 4;
        if (di + match > destination_capacity) {
            return 0;
        }
        // Overlapping copies are intentional in LZ4.
        for (size_t i = 0; i < match; ++i) {
            destination[di + i] = destination[di - offset + i];
        }
        di += match;
    }
    return di;
}

}  // namespace

std::unique_ptr<PakArchive> PakArchive::open(int fd) {
    struct stat status {};
    if (fstat(fd, &status) != 0) {
        throw PakError(std::string("fstat failed: ") + std::strerror(errno));
    }
    if (status.st_size < static_cast<off_t>(kHeaderSize)) {
        throw PakError("assets.pak is smaller than the 32-byte header");
    }

    auto archive = std::unique_ptr<PakArchive>(
        new PakArchive(fd, static_cast<uint64_t>(status.st_size)));

    uint8_t header[kHeaderSize];
    archive->readAt(header, sizeof(header), 0);
    if (std::memcmp(header, "RPAK", 4) != 0) {
        throw PakError("assets.pak has bad magic (expected RPAK)");
    }
    const uint32_t version = readU32(header + 4);
    if (version != kVersion) {
        throw PakError("unsupported pak version " + std::to_string(version));
    }
    const uint32_t count = readU32(header + 12);
    const uint64_t index_offset = readU64(header + 16);
    if (count == 0) {
        throw PakError("assets.pak contains no entries");
    }
    if (count > kMaxEntries) {
        throw PakError("assets.pak entry count is implausible");
    }
    if (index_offset < kHeaderSize || index_offset > archive->file_size_ ||
        count > (archive->file_size_ - index_offset) / kEntrySize) {
        throw PakError("pak index is outside the file");
    }

    std::vector<uint8_t> index(static_cast<size_t>(count) * kEntrySize);
    archive->readAt(index.data(), index.size(), index_offset);
    std::unordered_set<std::string> names;
    archive->entries_.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* raw = index.data() + static_cast<size_t>(i) * kEntrySize;
        const size_t name_length = strnlen(reinterpret_cast<const char*>(raw), kMaxName);
        if (name_length == 0 || name_length == kMaxName) {
            throw PakError("pak entry " + std::to_string(i) + " has an invalid name");
        }
        PakEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(raw), name_length);
        entry.offset = readU64(raw + 64);
        entry.stored_size = readU64(raw + 72);
        entry.original_size = readU64(raw + 80);
        entry.codec = readU32(raw + 88);
        entry.crc = readU32(raw + 92);

        if (!names.insert(entry.name).second) {
            throw PakError("duplicate pak entry: " + entry.name);
        }
        if (entry.offset < kHeaderSize || entry.offset > archive->file_size_ ||
            entry.stored_size > archive->file_size_ - entry.offset) {
            throw PakError("pak entry '" + entry.name + "' points outside the file");
        }
        if (entry.codec != kCodecRaw && entry.codec != kCodecLz4) {
            throw PakError("pak entry '" + entry.name + "' uses an unknown codec");
        }
        if (entry.codec == kCodecRaw && entry.stored_size != entry.original_size) {
            throw PakError("raw pak entry '" + entry.name + "' has mismatched sizes");
        }
        if (entry.original_size > 512ull * 1024 * 1024) {
            throw PakError("pak entry '" + entry.name + "' is larger than 512 MiB");
        }
        archive->entries_.push_back(std::move(entry));
    }
    return archive;
}

void PakArchive::readAt(void* destination, size_t size, uint64_t offset) const {
    if (offset > file_size_ || size > file_size_ - offset) {
        throw PakError("pak read outside the file");
    }
    auto* out = static_cast<uint8_t*>(destination);
    size_t done = 0;
    while (done < size) {
        const ssize_t got = pread(fd_, out + done, size - done,
                                  static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            throw PakError(std::string("pak pread failed: ") + std::strerror(errno));
        }
        if (got == 0) {
            throw PakError("unexpected EOF in pak");
        }
        done += static_cast<size_t>(got);
    }
}

const PakEntry* PakArchive::find(const std::string& name) const {
    for (const auto& entry : entries_) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

bool PakArchive::contains(const std::string& name) const {
    return find(name) != nullptr;
}

std::vector<uint8_t> PakArchive::read(const std::string& name) const {
    const PakEntry* entry = find(name);
    if (entry == nullptr) {
        throw PakError("pak entry not found: " + name);
    }
    if (entry->stored_size > 512ull * 1024 * 1024) {
        throw PakError("pak entry is too large to load: " + name);
    }
    std::vector<uint8_t> stored(static_cast<size_t>(entry->stored_size));
    readAt(stored.data(), stored.size(), entry->offset);

    std::vector<uint8_t> decoded;
    if (entry->codec == kCodecRaw) {
        decoded = std::move(stored);
    } else {
        decoded.resize(static_cast<size_t>(entry->original_size));
        const size_t produced = lz4DecodeBlock(stored.data(), stored.size(),
                                               decoded.data(), decoded.size());
        if (produced != decoded.size()) {
            throw PakError("LZ4 decode failed for pak entry: " + name);
        }
    }
    const uint32_t actual = crc32(decoded.data(), decoded.size());
    if (actual != entry->crc) {
        throw PakError("CRC mismatch for pak entry: " + name);
    }
    return decoded;
}

}  // namespace rea
