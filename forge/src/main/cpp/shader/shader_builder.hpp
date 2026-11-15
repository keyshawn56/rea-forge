#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace forge {

struct ShaderPackage {
    std::string path;
    std::string name;
    std::string kind;
    std::string magic;
    std::string sha256;
    uint64_t size = 0;
    bool valid_spirv = false;
    std::string diagnostic;
};

struct ShaderInventory {
    bool valid = false;
    uint64_t files = 0;
    uint64_t spirv_files = 0;
    uint64_t kps_files = 0;
    uint64_t bfres_files = 0;
    uint64_t other_files = 0;
    uint64_t total_bytes = 0;
    bool truncated = false;
    std::vector<ShaderPackage> packages;
    std::vector<std::string> warnings;
    std::string diagnostic;
};

// Checks the SPIR-V magic, version, bound and instruction-stream length.
bool validateSpirv(const std::vector<uint8_t>& bytes, std::string& error);

// Inventories shader-looking files in an extracted directory. KPS containers
// are identified by their shipped magic but their Maxwell bytecode is not
// translated.
ShaderInventory scanShaderDirectory(const std::string& root);

std::string shaderInventoryToJson(const ShaderInventory& inventory);

}  // namespace forge
