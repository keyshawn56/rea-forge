#include "romfs.hpp"

#include "../core/io.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace forge {
namespace {

constexpr uint64_t kHeaderSize = 0x50;
constexpr uint64_t kDirEntrySize = 0x18;
constexpr uint64_t kFileEntrySize = 0x20;
constexpr uint64_t kMaxTable = 64ull << 20;
constexpr uint64_t kMaxWalkEntries = 500000;
constexpr uint32_t kNoEntry = 0xFFFFFFFFu;
constexpr int kMaxDepth = 128;

struct DirEntry {
    uint32_t parent = kNoEntry;
    uint32_t sibling = kNoEntry;
    uint32_t child = kNoEntry;
    uint32_t file = kNoEntry;
    uint32_t hash = 0;
    std::string name;
};

struct FileEntry {
    uint32_t parent = kNoEntry;
    uint32_t sibling = kNoEntry;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t hash = 0;
    std::string name;
};

uint64_t align4(uint64_t value) {
    return (value + 3) & ~3ull;
}

bool validIndex(uint32_t index, std::size_t count) {
    return index == kNoEntry || index < count;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string extensionOf(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return "(none)";
    }
    std::string ext = lower(name.substr(dot + 1));
    if (ext.size() > 12) {
        ext.resize(12);
    }
    return ext;
}

bool safeComponent(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

void countName(RomFsInfo& info, const std::string& name) {
    ++info.file_count;
    ++info.extensions[extensionOf(name)];
    const auto base = lower(name);
    if (base == "main" || base == "main.nso") info.has_main = true;
    if (base == "main.npdm") info.has_main_npdm = true;
    if (base == "rtld") info.has_rtld = true;
    if (base == "sdk") info.has_sdk = true;
    if (base.size() > 4 && base.substr(base.size() - 4) == ".kps") ++info.shader_packages;
    if (base.size() > 4 && base.substr(base.size() - 4) == ".nca") ++info.nca_files;
    if (base.size() > 5 && base.substr(base.size() - 5) == ".pack") ++info.pack_files;
    if (base.size() > 6 && base.substr(base.size() - 6) == ".bfres") ++info.bfres_files;
}

std::vector<DirEntry> parseDirEntries(const std::vector<uint8_t>& table) {
    std::vector<DirEntry> entries;
    std::size_t pos = 0;
    while (pos + kDirEntrySize <= table.size()) {
        DirEntry entry;
        entry.parent = rd32(table.data() + pos);
        entry.sibling = rd32(table.data() + pos + 4);
        entry.child = rd32(table.data() + pos + 8);
        entry.file = rd32(table.data() + pos + 0xC);
        entry.hash = rd32(table.data() + pos + 0x10);
        const uint32_t name_size = rd32(table.data() + pos + 0x14);
        const std::size_t name_start = pos + kDirEntrySize;
        if (name_size > table.size() - name_start) {
            throw std::runtime_error("RomFS directory name extends past its metadata table");
        }
        entry.name.assign(reinterpret_cast<const char*>(table.data() + name_start), name_size);
        entries.push_back(std::move(entry));
        pos += align4(kDirEntrySize + name_size);
    }
    if (pos != table.size()) {
        throw std::runtime_error("RomFS directory metadata has trailing bytes");
    }
    return entries;
}

std::vector<FileEntry> parseFileEntries(const std::vector<uint8_t>& table) {
    std::vector<FileEntry> entries;
    std::size_t pos = 0;
    while (pos + kFileEntrySize <= table.size()) {
        FileEntry entry;
        entry.parent = rd32(table.data() + pos);
        entry.sibling = rd32(table.data() + pos + 4);
        entry.offset = rd64(table.data() + pos + 8);
        entry.size = rd64(table.data() + pos + 0x10);
        entry.hash = rd32(table.data() + pos + 0x18);
        const uint32_t name_size = rd32(table.data() + pos + 0x1C);
        const std::size_t name_start = pos + kFileEntrySize;
        if (name_size > table.size() - name_start) {
            throw std::runtime_error("RomFS file name extends past its metadata table");
        }
        entry.name.assign(reinterpret_cast<const char*>(table.data() + name_start), name_size);
        entries.push_back(std::move(entry));
        pos += align4(kFileEntrySize + name_size);
    }
    if (pos != table.size()) {
        throw std::runtime_error("RomFS file metadata has trailing bytes");
    }
    return entries;
}

}  // namespace

RomFsInfo inspectRomFsImage(int fd, uint64_t file_size) {
    RomFsInfo info;
    info.raw_image = true;
    info.file_bytes = file_size;
    if (file_size < kHeaderSize) {
        throw std::runtime_error("RomFS image is smaller than its 0x50-byte header");
    }
    const auto header = readAt(fd, 0, kHeaderSize, "RomFS header");
    const uint64_t header_size = rd64(header.data());
    const uint64_t dir_hash_offset = rd64(header.data() + 0x08);
    const uint64_t dir_hash_size = rd64(header.data() + 0x10);
    const uint64_t dir_meta_offset = rd64(header.data() + 0x18);
    const uint64_t dir_meta_size = rd64(header.data() + 0x20);
    const uint64_t file_hash_offset = rd64(header.data() + 0x28);
    const uint64_t file_hash_size = rd64(header.data() + 0x30);
    const uint64_t file_meta_offset = rd64(header.data() + 0x38);
    const uint64_t file_meta_size = rd64(header.data() + 0x40);
    const uint64_t file_data_offset = rd64(header.data() + 0x48);

    if (header_size < kHeaderSize || header_size > file_size) {
        throw std::runtime_error("RomFS header size is invalid");
    }
    const auto inside = [&](uint64_t offset, uint64_t size, const char* what) {
        if (offset > file_size || size > file_size - offset) {
            throw std::runtime_error(std::string(what) + " is outside the RomFS image");
        }
    };
    inside(dir_hash_offset, dir_hash_size, "RomFS directory hash table");
    inside(dir_meta_offset, dir_meta_size, "RomFS directory metadata");
    inside(file_hash_offset, file_hash_size, "RomFS file hash table");
    inside(file_meta_offset, file_meta_size, "RomFS file metadata");
    if (file_data_offset > file_size) {
        throw std::runtime_error("RomFS file data offset is outside the image");
    }
    if (dir_hash_size % 0x20 != 0 || file_hash_size % 0x20 != 0) {
        throw std::runtime_error("RomFS hash table size is not a multiple of 0x20");
    }
    if (dir_meta_size % kDirEntrySize != 0) {
        info.warnings.push_back("directory metadata size is not a multiple of 0x18");
    }
    if (file_meta_size % kFileEntrySize != 0) {
        info.warnings.push_back("file metadata size is not a multiple of 0x20");
    }
    if (dir_meta_size > kMaxTable || file_meta_size > kMaxTable) {
        throw std::runtime_error("RomFS metadata table is unreasonably large");
    }

    const auto dir_table = readAt(fd, dir_meta_offset, static_cast<std::size_t>(dir_meta_size),
                                 "RomFS directory metadata");
    const auto file_table = readAt(fd, file_meta_offset, static_cast<std::size_t>(file_meta_size),
                                  "RomFS file metadata");
    const auto dirs = parseDirEntries(dir_table);
    const auto files = parseFileEntries(file_table);
    if (dirs.empty()) {
        throw std::runtime_error("RomFS has no root directory entry");
    }
    if (dir_hash_size / 0x20 != dirs.size()) {
        info.warnings.push_back("directory hash count does not match directory entry count");
    }
    if (file_hash_size / 0x20 != files.size()) {
        info.warnings.push_back("file hash count does not match file entry count");
    }
    if (dirs[0].name.size() != 0) {
        info.warnings.push_back("RomFS root directory has a non-empty name");
    }

    struct PendingDir {
        uint32_t index;
        std::string path;
        int depth;
    };
    std::vector<PendingDir> stack;
    stack.push_back({0, {}, 0});
    std::unordered_set<uint32_t> visited_dirs;
    std::unordered_set<uint32_t> visited_files;

    while (!stack.empty()) {
        const PendingDir current = std::move(stack.back());
        stack.pop_back();
        if (current.depth > kMaxDepth) {
            info.warnings.push_back("RomFS tree exceeds the depth limit");
            continue;
        }
        if (current.index >= dirs.size()) {
            info.warnings.push_back("RomFS directory index is out of range");
            continue;
        }
        if (!visited_dirs.insert(current.index).second) {
            info.warnings.push_back("RomFS directory graph contains a cycle");
            continue;
        }
        ++info.directory_count;

        std::string path = current.path;
        if (current.index != 0) {
            const auto& entry = dirs[current.index];
            if (!safeComponent(entry.name)) {
                info.warnings.push_back("RomFS directory name is unsafe and was skipped");
                continue;
            }
            path = path.empty() ? entry.name : path + "/" + entry.name;
            if (current.depth == 1) {
                info.top_level.push_back(entry.name);
            }
        }

        const auto& root = dirs[current.index];
        if (!validIndex(root.child, dirs.size()) || !validIndex(root.sibling, dirs.size()) ||
            !validIndex(root.file, files.size()) || !validIndex(root.parent, dirs.size())) {
            info.warnings.push_back("RomFS directory entry has an out-of-range link");
            continue;
        }

        // Push siblings in reverse so the natural order is preserved on pop.
        std::vector<uint32_t> children;
        uint32_t link = root.child;
        std::set<uint32_t> seen_links;
        while (link != kNoEntry && link < dirs.size() && seen_links.insert(link).second) {
            children.push_back(link);
            link = dirs[link].sibling;
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back({*it, path, current.depth + 1});
        }

        uint32_t file_link = root.file;
        seen_links.clear();
        while (file_link != kNoEntry && file_link < files.size() &&
               seen_links.insert(file_link).second) {
            if (!visited_files.insert(file_link).second) {
                info.warnings.push_back("RomFS file graph contains a cycle");
                break;
            }
            const auto& file = files[file_link];
            if (!safeComponent(file.name)) {
                info.warnings.push_back("RomFS file name is unsafe and was skipped");
            } else {
                if (file.size > file_size - file_data_offset ||
                    file.offset > file_size - file_data_offset - file.size) {
                    info.warnings.push_back("RomFS file payload is outside the image: " + file.name);
                } else {
                    info.total_bytes += file.size;
                }
                countName(info, file.name);
                if (current.index == 0) {
                    info.top_level.push_back(file.name);
                }
            }
            file_link = file.sibling;
        }
    }

    std::sort(info.top_level.begin(), info.top_level.end());
    info.top_level.erase(std::unique(info.top_level.begin(), info.top_level.end()),
                         info.top_level.end());
    info.valid = info.directory_count > 0;
    if (!info.valid) {
        info.diagnostic = "RomFS tree could not be walked";
    }
    return info;
}

RomFsInfo indexRomFsDirectory(const std::string& root) {
    namespace fs = std::filesystem;
    RomFsInfo info;
    info.directory = true;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) {
        throw std::runtime_error("selected RomFS path is not a readable directory");
    }

    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        info.top_level.push_back(it->path().filename().string());
        if (info.top_level.size() >= 128) {
            break;
        }
    }
    std::sort(info.top_level.begin(), info.top_level.end());

    uint64_t entries = 0;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (it != end) {
        if (ec) {
            info.warnings.push_back("directory walk stopped: " + ec.message());
            break;
        }
        if (++entries > kMaxWalkEntries) {
            info.truncated = true;
            info.warnings.push_back("directory walk reached the 500000-entry limit");
            break;
        }
        const fs::directory_entry& entry = *it;
        std::error_code type_ec;
        if (entry.is_symlink(type_ec)) {
            it.disable_recursion_pending();
        } else if (entry.is_directory(type_ec)) {
            ++info.directory_count;
            if (entry.path().filename().string() == ".git") {
                it.disable_recursion_pending();
            }
        } else if (entry.is_regular_file(type_ec)) {
            const std::string name = entry.path().filename().string();
            const uint64_t size = entry.file_size(type_ec);
            if (!type_ec) {
                info.total_bytes += size;
            }
            countName(info, name);
        }
        it.increment(ec);
    }

    info.file_bytes = info.total_bytes;
    info.valid = info.directory_count > 0 || info.file_count > 0;
    if (!info.valid) {
        info.diagnostic = "the selected directory is empty or unreadable";
    }
    return info;
}

std::string romFsToJson(const RomFsInfo& info) {
    std::ostringstream json;
    json << '{';
    json << "\"valid\":" << (info.valid ? "true" : "false");
    json << ",\"mode\":\"" << (info.raw_image ? "image" : "directory") << '"';
    json << ",\"fileBytes\":" << info.file_bytes;
    json << ",\"directoryCount\":" << info.directory_count;
    json << ",\"fileCount\":" << info.file_count;
    json << ",\"contentBytes\":" << info.total_bytes;
    json << ",\"truncated\":" << (info.truncated ? "true" : "false");
    json << ",\"hasMain\":" << (info.has_main ? "true" : "false");
    json << ",\"hasMainNpdm\":" << (info.has_main_npdm ? "true" : "false");
    json << ",\"hasRtld\":" << (info.has_rtld ? "true" : "false");
    json << ",\"hasSdk\":" << (info.has_sdk ? "true" : "false");
    json << ",\"shaderPackages\":" << info.shader_packages;
    json << ",\"ncaFiles\":" << info.nca_files;
    json << ",\"packFiles\":" << info.pack_files;
    json << ",\"bfresFiles\":" << info.bfres_files;
    json << ",\"topLevel\":[";
    for (std::size_t i = 0; i < info.top_level.size(); ++i) {
        if (i != 0) json << ',';
        json << '"' << jsonEscape(info.top_level[i]) << '"';
    }
    json << "],\"extensions\":{";
    bool first = true;
    for (const auto& [ext, count] : info.extensions) {
        if (!first) json << ',';
        first = false;
        json << '"' << jsonEscape(ext) << "\":" << count;
    }
    json << "},\"warnings\":[";
    for (std::size_t i = 0; i < info.warnings.size(); ++i) {
        if (i != 0) json << ',';
        json << '"' << jsonEscape(info.warnings[i]) << '"';
    }
    json << ']';
    if (!info.diagnostic.empty()) {
        json << ",\"diagnostic\":\"" << jsonEscape(info.diagnostic) << '"';
    }
    json << '}';
    return json.str();
}

}  // namespace forge
