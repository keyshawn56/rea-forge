#include "pfs0.hpp"

#include "../core/io.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace forge {
namespace {

constexpr uint32_t kPfs0HeaderSize = 0x10;
constexpr uint32_t kPfs0EntrySize = 0x18;
constexpr uint32_t kMaxEntries = 4096;
constexpr uint32_t kMaxStringTable = 1u << 20;
constexpr std::size_t kCopyChunk = 1u << 20;

std::string readName(const std::vector<uint8_t>& strings, uint32_t offset) {
    if (offset >= strings.size()) {
        throw std::runtime_error("PFS0 entry name offset is outside the string table");
    }
    const auto begin = strings.begin() + offset;
    const auto end = std::find(begin, strings.end(), 0);
    if (end == strings.end()) {
        throw std::runtime_error("PFS0 entry name is not NUL-terminated");
    }
    return std::string(reinterpret_cast<const char*>(&*begin),
                       static_cast<std::size_t>(std::distance(begin, end)));
}

}  // namespace

Pfs0Archive parsePfs0(int fd, uint64_t base, uint64_t file_size) {
    if (base >= file_size || file_size - base < kPfs0HeaderSize) {
        throw std::runtime_error("PFS0 header is outside the file");
    }
    const auto header = readAt(fd, base, kPfs0HeaderSize, "PFS0 header");
    if (!hasMagic(header.data(), "PFS0", 4)) {
        throw std::runtime_error("PFS0 magic is missing");
    }

    Pfs0Archive archive;
    archive.base = base;
    archive.entry_count = rd32(header.data() + 4);
    archive.string_table_size = rd32(header.data() + 8);
    if (archive.entry_count == 0) {
        throw std::runtime_error("PFS0 contains no entries");
    }
    if (archive.entry_count > kMaxEntries) {
        throw std::runtime_error("PFS0 entry count is unreasonable");
    }
    if (archive.string_table_size > kMaxStringTable) {
        throw std::runtime_error("PFS0 string table is unreasonably large");
    }
    const uint64_t table_size = kPfs0HeaderSize +
                                static_cast<uint64_t>(archive.entry_count) * kPfs0EntrySize +
                                archive.string_table_size;
    if (table_size > file_size - base) {
        throw std::runtime_error("PFS0 table extends past end of file");
    }
    archive.data_offset = table_size;
    archive.total_size = file_size - base;

    const uint64_t entries_bytes =
        static_cast<uint64_t>(archive.entry_count) * kPfs0EntrySize;
    const auto table = readAt(fd, base + kPfs0HeaderSize,
                              static_cast<std::size_t>(entries_bytes), "PFS0 entry table");
    const auto strings = readAt(fd, base + kPfs0HeaderSize + entries_bytes,
                                archive.string_table_size, "PFS0 string table");

    std::set<std::string> names;
    std::vector<std::pair<uint64_t, uint64_t>> extents;
    archive.entries.reserve(archive.entry_count);
    for (uint32_t i = 0; i < archive.entry_count; ++i) {
        const uint8_t* row = table.data() + static_cast<std::size_t>(i) * kPfs0EntrySize;
        Pfs0Entry entry;
        entry.offset = rd64(row);
        entry.size = rd64(row + 8);
        const uint32_t name_offset = rd32(row + 0x10);
        entry.name = readName(strings, name_offset);
        if (entry.name.empty()) {
            throw std::runtime_error("PFS0 entry has an empty name");
        }
        if (!names.insert(entry.name).second) {
            throw std::runtime_error("PFS0 contains duplicate entry name: " + entry.name);
        }
        if (entry.offset > archive.total_size ||
            entry.size > archive.total_size - entry.offset) {
            throw std::runtime_error("PFS0 entry extends past end of file: " + entry.name);
        }
        if (entry.offset < archive.data_offset) {
            throw std::runtime_error("PFS0 entry overlaps its own table: " + entry.name);
        }
        entry.absolute_offset = base + entry.offset;
        if (entry.size != 0) {
            extents.emplace_back(entry.offset, entry.offset + entry.size);
        }
        archive.entries.push_back(std::move(entry));
    }

    std::sort(extents.begin(), extents.end());
    for (std::size_t i = 1; i < extents.size(); ++i) {
        if (extents[i].first < extents[i - 1].second) {
            throw std::runtime_error("PFS0 contains overlapping file extents");
        }
    }
    return archive;
}

uint64_t extractPfs0Entry(int fd, const Pfs0Archive& archive, const Pfs0Entry& entry,
                          const std::string& out_path) {
    if (entry.offset > archive.total_size || entry.size > archive.total_size - entry.offset) {
        throw std::runtime_error("PFS0 entry is outside the archive");
    }
    std::error_code ec;
    const auto parent = std::filesystem::path(out_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw std::runtime_error("cannot create staging directory: " + ec.message());
        }
    }
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot open staging output " + out_path);
    }
    std::vector<uint8_t> buffer(kCopyChunk);
    uint64_t done = 0;
    while (done < entry.size) {
        const std::size_t take = static_cast<std::size_t>(
            std::min<uint64_t>(buffer.size(), entry.size - done));
        preadExact(fd, entry.absolute_offset + done, buffer.data(), take, "PFS0 extraction");
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(take));
        if (!out) {
            throw std::runtime_error("short write while extracting " + entry.name);
        }
        done += take;
    }
    return done;
}

NcaInfo inspectNcaHeader(const uint8_t* data, std::size_t size) {
    NcaInfo info;
    if (size < 0x300) {
        info.diagnostic = "entry is too small to contain an NCA header";
        return info;
    }
    info.examined = true;
    if (!hasMagic(data + 0x200, "NCA", 3)) {
        info.diagnostic =
            "NCA header is not plaintext (retail NSPs are encrypted and require title keys)";
        return info;
    }
    const char* magic = reinterpret_cast<const char*>(data + 0x200);
    info.magic.assign(magic, 4);
    info.plaintext_header = true;
    info.distribution_type = data[0x204];
    info.content_type = data[0x205];
    info.key_generation_old = data[0x206];
    info.key_area_index = data[0x207];
    info.content_size = rd64(data + 0x208);
    info.program_id = rd64(data + 0x210);
    info.content_index = rd32(data + 0x218);
    info.sdk_addon_version = rd32(data + 0x21C);
    info.key_generation = data[0x220];
    info.rights_id = hex(data + 0x230, 16);

    if (size < 0xC00) {
        info.diagnostic = "NCA header is truncated before the section-header area";
        return info;
    }
    if (info.content_size != 0 && info.content_size < 0xC00) {
        info.diagnostic = "NCA content size is smaller than its header";
    }

    for (int i = 0; i < 4; ++i) {
        const uint8_t* fs_entry = data + 0x240 + i * 0x10;
        const uint64_t start = rd32(fs_entry);
        const uint64_t end = rd32(fs_entry + 4);
        NcaSectionInfo section;
        section.start_block = start;
        section.end_block = end;
        section.present = end > start;
        if (section.present) {
            if (end < start) {
                info.diagnostic = "NCA section has end < start";
            }
            // Each block is 0x200 bytes. Do not assume the entry size is exact:
            // content_size is authoritative when present.
            const uint64_t byte_end = end * 0x200ull;
            if (byte_end > size && (info.content_size == 0 || byte_end > info.content_size)) {
                info.diagnostic = "NCA section extends past the entry payload";
            }
        }
        const uint8_t* fs_header = data + 0x400 + i * 0x200;
        section.fs_version = rd16(fs_header);
        section.fs_type = fs_header[2];
        section.hash_type = fs_header[3];
        section.encryption_type = fs_header[4];
        if (section.present) {
            if (section.fs_version != 2) {
                info.diagnostic = "NCA section header version is not 2";
            }
            if (section.fs_type > 1) {
                info.diagnostic = "NCA section type is not RomFS/PartitionFS";
            }
            if (section.encryption_type > 6) {
                info.diagnostic = "NCA section uses an unknown encryption type";
            }
        }
        info.sections.push_back(section);
    }
    return info;
}

std::string nspToJson(const Pfs0Archive& archive, const std::vector<NcaInfo>& ncas,
                      const std::vector<uint8_t>& entry_kinds) {
    int nca_total = 0;
    int plaintext = 0;
    int encrypted = 0;
    int meta = 0;
    for (std::size_t i = 0; i < archive.entries.size(); ++i) {
        if (entry_kinds[i] == 0) {
            ++nca_total;
            if (i < ncas.size() && ncas[i].plaintext_header) {
                ++plaintext;
            } else {
                ++encrypted;
            }
        } else if (entry_kinds[i] == 1) {
            ++meta;
        }
    }

    std::ostringstream json;
    json << '{';
    json << "\"valid\":true";
    json << ",\"format\":\"NSP/PFS0\"";
    json << ",\"entryCount\":" << archive.entry_count;
    json << ",\"stringTableBytes\":" << archive.string_table_size;
    json << ",\"dataOffset\":" << archive.data_offset;
    json << ",\"ncaCount\":" << nca_total;
    json << ",\"ncaPlaintextHeaders\":" << plaintext;
    json << ",\"ncaEncryptedHeaders\":" << encrypted;
    json << ",\"ticketLikeEntries\":" << meta;
    json << ",\"containerExtractable\":true";
    json << ",\"contentExtractable\":" << (plaintext > 0 ? "true" : "false");
    json << ",\"notice\":\"PFS0 container validated. ";
    if (encrypted > 0) {
        json << "Encrypted NCA payloads are deliberately not decrypted; select an already-extracted romfs+exefs pair for build stages.";
    } else {
        json << "Plaintext NCA headers were found; section data may still be encrypted.";
    }
    json << "\"";
    json << ",\"entries\":[";
    for (std::size_t i = 0; i < archive.entries.size(); ++i) {
        if (i != 0) {
            json << ',';
        }
        const auto& entry = archive.entries[i];
        json << '{';
        json << "\"name\":\"" << jsonEscape(entry.name) << '"';
        json << ",\"offset\":" << entry.offset;
        json << ",\"size\":" << entry.size;
        json << ",\"kind\":";
        switch (entry_kinds[i]) {
            case 0: json << "\"nca\""; break;
            case 1: json << "\"ticket\""; break;
            case 2: json << "\"certificate\""; break;
            default: json << "\"other\""; break;
        }
        if (i < ncas.size() && ncas[i].examined) {
            const auto& nca = ncas[i];
            json << ",\"nca\":{";
            json << "\"plaintextHeader\":" << (nca.plaintext_header ? "true" : "false");
            json << ",\"magic\":\"" << jsonEscape(nca.magic) << '"';
            json << ",\"contentType\":" << static_cast<int>(nca.content_type);
            json << ",\"contentSize\":" << nca.content_size;
            std::ostringstream program_id;
            program_id << std::hex << std::setw(16) << std::setfill('0') << nca.program_id;
            json << ",\"programId\":\"" << program_id.str() << '"';
            json << ",\"rightsId\":\"" << nca.rights_id << '"';
            json << ",\"sections\":[";
            for (std::size_t s = 0; s < nca.sections.size(); ++s) {
                if (s != 0) json << ',';
                const auto& section = nca.sections[s];
                json << '{';
                json << "\"present\":" << (section.present ? "true" : "false");
                json << ",\"startBlock\":" << section.start_block;
                json << ",\"endBlock\":" << section.end_block;
                json << ",\"fsType\":" << section.fs_type;
                json << ",\"hashType\":" << section.hash_type;
                json << ",\"encryptionType\":" << section.encryption_type;
                json << '}';
            }
            json << ']';
            if (!nca.diagnostic.empty()) {
                json << ",\"diagnostic\":\"" << jsonEscape(nca.diagnostic) << '"';
            }
            json << '}';
        }
        json << '}';
    }
    json << "]}";
    return json.str();
}

}  // namespace forge
