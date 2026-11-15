#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace forge {

// Result of validating a raw RomFS image or indexing an already-extracted
// RomFS directory. Neither operation copies game content into app storage.
struct RomFsInfo {
    bool valid = false;
    bool raw_image = false;
    bool directory = false;
    uint64_t file_bytes = 0;
    uint64_t directory_count = 0;
    uint64_t file_count = 0;
    uint64_t total_bytes = 0;
    bool truncated = false;
    std::vector<std::string> top_level;
    std::map<std::string, uint64_t> extensions;
    bool has_main = false;
    bool has_main_npdm = false;
    bool has_rtld = false;
    bool has_sdk = false;
    uint64_t shader_packages = 0;
    uint64_t nca_files = 0;
    uint64_t pack_files = 0;
    uint64_t bfres_files = 0;
    std::vector<std::string> warnings;
    std::string diagnostic;
};

// Parses the RomFS header, directory/file metadata tables and walks the tree.
// Only metadata is read from fd; file payloads are never loaded.
RomFsInfo inspectRomFsImage(int fd, uint64_t file_size);

// Recursively indexes an extracted RomFS tree on the app-private filesystem.
// Symlinks are not followed and the walk is capped to avoid unbounded work.
RomFsInfo indexRomFsDirectory(const std::string& root);

std::string romFsToJson(const RomFsInfo& info);

}  // namespace forge
