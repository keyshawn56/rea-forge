#include "nso.hpp"

#include "../core/io.hpp"
#include "../core/sha256.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace forge {
namespace {

constexpr uint32_t kHeaderSize = 0x100;
constexpr uint32_t kFlagTextCompressed = 1u << 0;
constexpr uint32_t kFlagRodataCompressed = 1u << 1;
constexpr uint32_t kFlagDataCompressed = 1u << 2;
constexpr uint32_t kFlagTextHash = 1u << 3;
constexpr uint32_t kFlagRodataHash = 1u << 4;
constexpr uint32_t kFlagDataHash = 1u << 5;
constexpr uint32_t kFlagZbic = 1u << 7;
constexpr uint64_t kMaxSegment = 1ull << 30;

struct SectionSpec {
    const char* name;
    uint32_t file_offset_field;
    uint32_t memory_offset_field;
    uint32_t memory_size_field;
    uint32_t file_size_field;
    uint32_t hash_field;
    uint32_t compressed_flag;
    uint32_t hash_flag;
};

const SectionSpec kSections[] = {
    {"text", 0x10, 0x14, 0x18, 0x60, 0xA0, kFlagTextCompressed, kFlagTextHash},
    {"rodata", 0x20, 0x24, 0x28, 0x64, 0xC0, kFlagRodataCompressed, kFlagRodataHash},
    {"data", 0x30, 0x34, 0x38, 0x68, 0xE0, kFlagDataCompressed, kFlagDataHash},
};

std::string readModuleName(int fd, uint32_t offset, uint32_t size, uint64_t file_size) {
    if (size == 0) {
        return {};
    }
    if (offset >= file_size || size > file_size - offset || size > 4096) {
        throw std::runtime_error("module name range is outside the file");
    }
    auto bytes = readAt(fd, offset, size, "NSO module name");
    const auto nul = std::find(bytes.begin(), bytes.end(), 0);
    bytes.erase(nul, bytes.end());
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void verifyHash(const NsoSection& section, bool required) {
    if (!required) {
        return;
    }
    Sha256 hash;
    hash.update(section.bytes.data(), section.bytes.size());
    const auto digest = hash.finish();
    const std::string actual = forge::hex(digest.data(), digest.size());
    if (actual != section.stored_hash) {
        throw std::runtime_error(section.name + " SHA-256 mismatch: header " +
                                 section.stored_hash + ", decoded section " + actual);
    }
}

}  // namespace

std::vector<uint8_t> lz4BlockDecompress(const uint8_t* src, std::size_t src_size,
                                        std::size_t expected_size) {
    std::vector<uint8_t> out;
    out.reserve(expected_size);
    std::size_t ip = 0;
    while (ip < src_size) {
        const uint8_t token = src[ip++];
        std::size_t literal_length = token >> 4;
        if (literal_length == 15) {
            for (;;) {
                if (ip >= src_size) {
                    throw std::runtime_error("LZ4: truncated literal length");
                }
                const uint8_t extra = src[ip++];
                literal_length += extra;
                if (extra != 255) {
                    break;
                }
            }
        }
        if (literal_length > src_size - ip) {
            throw std::runtime_error("LZ4: literal run exceeds input");
        }
        if (out.size() + literal_length > expected_size) {
            throw std::runtime_error("LZ4: literal run exceeds expected output");
        }
        out.insert(out.end(), src + ip, src + ip + literal_length);
        ip += literal_length;

        if (ip == src_size) {
            break;
        }
        if (src_size - ip < 2) {
            throw std::runtime_error("LZ4: truncated match offset");
        }
        const std::size_t match_offset =
            static_cast<std::size_t>(src[ip]) | (static_cast<std::size_t>(src[ip + 1]) << 8);
        ip += 2;
        if (match_offset == 0 || match_offset > out.size()) {
            throw std::runtime_error("LZ4: invalid match offset");
        }

        std::size_t match_length = token & 0x0f;
        if (match_length == 15) {
            for (;;) {
                if (ip >= src_size) {
                    throw std::runtime_error("LZ4: truncated match length");
                }
                const uint8_t extra = src[ip++];
                match_length += extra;
                if (extra != 255) {
                    break;
                }
            }
        }
        match_length += 4;
        if (out.size() + match_length > expected_size) {
            throw std::runtime_error("LZ4: match exceeds expected output");
        }
        const std::size_t match_start = out.size() - match_offset;
        for (std::size_t i = 0; i < match_length; ++i) {
            out.push_back(out[match_start + i]);
        }
    }

    if (out.size() != expected_size) {
        throw std::runtime_error("LZ4: decoded " + std::to_string(out.size()) +
                                 " bytes, expected " + std::to_string(expected_size));
    }
    return out;
}

NsoImage loadNso(int fd, const std::string& source_name, bool keep_bytes) {
    const uint64_t file_size = fileSize(fd);
    if (file_size < kHeaderSize) {
        throw std::runtime_error("NSO is smaller than the 0x100-byte header");
    }
    const auto header = readAt(fd, 0, kHeaderSize, "NSO header");
    if (!hasMagic(header.data(), "NSO0", 4)) {
        throw std::runtime_error("NSO magic is not NSO0");
    }

    NsoImage image;
    image.source_name = source_name;
    image.file_bytes = file_size;
    image.version = rd32(header.data() + 0x04);
    image.flags = rd32(header.data() + 0x0C);
    image.bss_size = rd32(header.data() + 0x3C);
    std::memcpy(image.module_id.data(), header.data() + 0x40, image.module_id.size());
    image.embedded_offset = rd32(header.data() + 0x88);
    image.embedded_size = rd32(header.data() + 0x8C);
    image.dynstr_offset = rd32(header.data() + 0x90);
    image.dynstr_size = rd32(header.data() + 0x94);
    image.dynsym_offset = rd32(header.data() + 0x98);
    image.dynsym_size = rd32(header.data() + 0x9C);

    if (image.version != 0) {
        throw std::runtime_error("unsupported NSO version " + std::to_string(image.version));
    }
    if ((image.flags & kFlagZbic) != 0) {
        throw std::runtime_error(
            "NSO uses ZBIC compression (flag 0x80); this build supports LZ4 NSOs only");
    }
    if ((image.flags & ~0xFFu) != 0) {
        throw std::runtime_error("NSO flags contain unknown high bits");
    }

    const uint32_t name_offset = rd32(header.data() + 0x1C);
    image.module_name_size = rd32(header.data() + 0x2C);
    image.module_name = readModuleName(fd, name_offset, image.module_name_size, file_size);

    for (const auto& spec : kSections) {
        NsoSection section;
        section.name = spec.name;
        section.file_offset = rd32(header.data() + spec.file_offset_field);
        section.memory_offset = rd32(header.data() + spec.memory_offset_field);
        section.memory_size = rd32(header.data() + spec.memory_size_field);
        section.file_size = rd32(header.data() + spec.file_size_field);
        section.compressed = (image.flags & spec.compressed_flag) != 0;
        const bool hash_required = (image.flags & spec.hash_flag) != 0;
        const uint8_t* hash_ptr = header.data() + spec.hash_field;
        section.stored_hash = forge::hex(hash_ptr, 32);

        if (section.memory_size > kMaxSegment) {
            throw std::runtime_error(section.name + " is unreasonably large");
        }
        if (section.file_size > kMaxSegment) {
            throw std::runtime_error(section.name + " stored size is unreasonably large");
        }
        if (section.file_size != 0) {
            if (section.file_offset < kHeaderSize) {
                throw std::runtime_error(section.name + " overlaps the NSO header");
            }
            if (section.file_offset > file_size ||
                section.file_size > file_size - section.file_offset) {
                throw std::runtime_error(section.name + " extends past end of file");
            }
        }
        if (!section.compressed && section.file_size != section.memory_size) {
            throw std::runtime_error(section.name + " is uncompressed but file and memory sizes differ");
        }
        if (section.memory_size == 0 && section.file_size != 0) {
            throw std::runtime_error(section.name + " has data but a zero memory size");
        }

        auto stored = readAt(fd, section.file_offset, section.file_size, "NSO section");
        if (section.compressed) {
            section.bytes = lz4BlockDecompress(stored.data(), stored.size(), section.memory_size);
        } else {
            section.bytes.assign(section.memory_size, 0);
            if (!stored.empty()) {
                std::memcpy(section.bytes.data(), stored.data(), stored.size());
            }
        }

        Sha256 hash;
        hash.update(section.bytes.data(), section.bytes.size());
        const auto digest = hash.finish();
        section.actual_hash = forge::hex(digest.data(), digest.size());
        if (hash_required) {
            verifyHash(section, true);
            section.hash_verified = true;
        }

        if (!keep_bytes) {
            section.bytes.clear();
            section.bytes.shrink_to_fit();
        }
        image.sections.push_back(std::move(section));
    }

    // The embedded module metadata and dynamic tables live in .rodata. Verify
    // their ranges here so later stages can trust the offsets.
    const auto& rodata = image.sections[1];
    const auto check_range = [&](uint32_t offset, uint32_t length, const char* what) {
        if (length == 0) {
            return;
        }
        if (offset > rodata.memory_size || length > rodata.memory_size - offset) {
            throw std::runtime_error(std::string(what) + " range is outside .rodata");
        }
    };
    check_range(image.embedded_offset, image.embedded_size, "embedded metadata");
    check_range(image.dynstr_offset, image.dynstr_size, ".dynstr");
    check_range(image.dynsym_offset, image.dynsym_size, ".dynsym");

    return image;
}

std::string nsoToJson(const NsoImage& image, bool include_sites) {
    (void)include_sites;
    std::ostringstream json;
    json << '{';
    json << "\"valid\":true";
    json << ",\"name\":\"" << jsonEscape(image.source_name) << '"';
    json << ",\"moduleName\":\"" << jsonEscape(image.module_name) << '"';
    json << ",\"version\":\"0x" << std::hex << image.version << std::dec << '"';
    json << ",\"flags\":\"0x" << std::hex << image.flags << std::dec << '"';
    json << ",\"zbic\":" << ((image.flags & 0x80u) != 0 ? "true" : "false");
    json << ",\"fileBytes\":" << image.file_bytes;
    json << ",\"bssBytes\":" << image.bss_size;
    json << ",\"moduleId\":\"" << hex(image.module_id.data(), image.module_id.size()) << '"';
    json << ",\"embeddedOffset\":" << image.embedded_offset;
    json << ",\"embeddedSize\":" << image.embedded_size;
    json << ",\"dynStrSize\":" << image.dynstr_size;
    json << ",\"dynSymSize\":" << image.dynsym_size;
    json << ",\"sections\":[";
    for (std::size_t i = 0; i < image.sections.size(); ++i) {
        const auto& section = image.sections[i];
        if (i != 0) {
            json << ',';
        }
        json << '{';
        json << "\"name\":\"" << section.name << '"';
        json << ",\"fileOffset\":" << section.file_offset;
        json << ",\"memoryOffset\":" << section.memory_offset;
        json << ",\"memorySize\":" << section.memory_size;
        json << ",\"fileSize\":" << section.file_size;
        json << ",\"compressed\":" << (section.compressed ? "true" : "false");
        json << ",\"hashRequired\":" << (section.hash_verified ? "true" : "false");
        json << ",\"sha256\":\"" << section.actual_hash << '"';
        json << '}';
    }
    json << "]}";
    return json.str();
}

}  // namespace forge
