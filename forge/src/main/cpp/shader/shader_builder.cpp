#include "shader_builder.hpp"

#include "../core/io.hpp"
#include "../core/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace forge {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint64_t kMaxShaderFile = 64ull << 20;
constexpr uint64_t kMaxFiles = 100000;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string suffixOf(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    return lower(name.substr(dot));
}

std::string readMagic(const std::string& path, std::size_t wanted) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string bytes(wanted, '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(wanted));
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    std::ostringstream hex_text;
    for (unsigned char c : bytes) {
        static const char* digits = "0123456789abcdef";
        hex_text << digits[c >> 4] << digits[c & 0xF];
    }
    return hex_text.str();
}

}  // namespace

bool validateSpirv(const std::vector<uint8_t>& bytes, std::string& error) {
    if (bytes.size() < 20) {
        error = "shader is smaller than the 5-word SPIR-V header";
        return false;
    }
    if (bytes.size() % 4 != 0) {
        error = "shader length is not a multiple of 4 bytes";
        return false;
    }
    const uint32_t magic = rd32(bytes.data());
    if (magic != kSpirvMagic) {
        std::ostringstream message;
        message << "SPIR-V magic is 0x" << std::hex << magic << " (expected 0x07230203)";
        error = message.str();
        return false;
    }
    const uint32_t version = rd32(bytes.data() + 4);
    const uint32_t major = (version >> 16) & 0xFFu;
    const uint32_t minor = (version >> 8) & 0xFFu;
    if (major != 1 || minor > 6) {
        error = "SPIR-V version " + std::to_string(major) + "." + std::to_string(minor) +
                " is not recognized";
        return false;
    }
    const uint32_t bound = rd32(bytes.data() + 8);
    if (bound == 0 || bound > (1u << 22)) {
        error = "SPIR-V id bound is invalid";
        return false;
    }
    const uint32_t schema = rd32(bytes.data() + 16);
    if (schema != 0) {
        error = "SPIR-V schema is not zero";
        return false;
    }
    return true;
}

ShaderInventory scanShaderDirectory(const std::string& root) {
    namespace fs = std::filesystem;
    ShaderInventory inventory;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) {
        inventory.diagnostic = "selected shader path is not a readable directory";
        return inventory;
    }
    inventory.valid = true;

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (it != end) {
        if (ec) {
            inventory.warnings.push_back("shader walk stopped: " + ec.message());
            break;
        }
        if (inventory.files >= kMaxFiles) {
            inventory.truncated = true;
            inventory.warnings.push_back("shader walk reached the 100000-file limit");
            break;
        }
        const fs::directory_entry& entry = *it;
        std::error_code type_ec;
        if (entry.is_symlink(type_ec)) {
            it.disable_recursion_pending();
        } else if (entry.is_regular_file(type_ec)) {
            ++inventory.files;
            const std::string path = entry.path().string();
            const std::string name = entry.path().filename().string();
            const std::string suffix = suffixOf(name);
            const uint64_t size = entry.file_size(type_ec);
            if (!type_ec) {
                inventory.total_bytes += size;
            }

            ShaderPackage package;
            package.path = path;
            package.name = name;
            package.size = type_ec ? 0 : size;
            package.magic = readMagic(path, 8);

            if (suffix == ".spv") {
                ++inventory.spirv_files;
                package.kind = "spirv";
                if (size <= kMaxShaderFile) {
                    try {
                        const auto bytes = readFile(path, static_cast<std::size_t>(kMaxShaderFile));
                        std::string error;
                        package.valid_spirv = validateSpirv(bytes, error);
                        if (!package.valid_spirv) {
                            package.diagnostic = error;
                        }
                        Sha256 hash;
                        hash.update(bytes.data(), bytes.size());
                        const auto digest = hash.finish();
                        package.sha256 = forge::hex(digest.data(), digest.size());
                    } catch (const std::exception& error) {
                        package.diagnostic = error.what();
                    }
                } else {
                    package.diagnostic = "shader exceeds the 64 MiB scan limit";
                }
            } else if (package.magic.rfind("5f53504b34313030", 0) == 0) {
                // "_SPK4100" in hex; this is a KTGL shader package, not SPIR-V.
                ++inventory.kps_files;
                package.kind = "kps";
                package.diagnostic =
                    "KTGL shader package detected; Maxwell bytecode translation is not implemented";
            } else if (suffix == ".bfres" || suffix == ".bfsha" || suffix == ".sharc") {
                ++inventory.bfres_files;
                package.kind = "bfres";
                package.diagnostic = "Nintendo resource container; shader extraction is not implemented";
            } else {
                ++inventory.other_files;
                package.kind = "other";
            }
            if (inventory.packages.size() < 512 &&
                (package.kind != "other" || suffix == ".spv")) {
                inventory.packages.push_back(std::move(package));
            }
        }
        it.increment(ec);
    }

    if (inventory.kps_files != 0) {
        inventory.warnings.push_back(
            "KPS shader packages contain Maxwell-era GPU programs. The bundled SPIR-V post-process "
            "shaders can be cached, but game shader bytecode is not translated.");
    }
    return inventory;
}

std::string shaderInventoryToJson(const ShaderInventory& inventory) {
    std::ostringstream json;
    json << '{';
    json << "\"valid\":" << (inventory.valid ? "true" : "false");
    json << ",\"files\":" << inventory.files;
    json << ",\"spirvFiles\":" << inventory.spirv_files;
    json << ",\"kpsFiles\":" << inventory.kps_files;
    json << ",\"bfresFiles\":" << inventory.bfres_files;
    json << ",\"otherFiles\":" << inventory.other_files;
    json << ",\"totalBytes\":" << inventory.total_bytes;
    json << ",\"truncated\":" << (inventory.truncated ? "true" : "false");
    json << ",\"maxwellTranslationImplemented\":false";
    json << ",\"gameSpirvEmitted\":0";
    json << ",\"packages\":[";
    for (std::size_t i = 0; i < inventory.packages.size(); ++i) {
        if (i != 0) json << ',';
        const auto& package = inventory.packages[i];
        json << '{';
        json << "\"name\":\"" << jsonEscape(package.name) << '"';
        json << ",\"kind\":\"" << package.kind << '"';
        json << ",\"size\":" << package.size;
        json << ",\"magic\":\"" << package.magic << '"';
        json << ",\"sha256\":\"" << package.sha256 << '"';
        json << ",\"validSpirv\":" << (package.valid_spirv ? "true" : "false");
        if (!package.diagnostic.empty()) {
            json << ",\"diagnostic\":\"" << jsonEscape(package.diagnostic) << '"';
        }
        json << '}';
    }
    json << "],\"warnings\":[";
    for (std::size_t i = 0; i < inventory.warnings.size(); ++i) {
        if (i != 0) json << ',';
        json << '"' << jsonEscape(inventory.warnings[i]) << '"';
    }
    json << ']';
    if (!inventory.diagnostic.empty()) {
        json << ",\"diagnostic\":\"" << jsonEscape(inventory.diagnostic) << '"';
    }
    json << '}';
    return json.str();
}

}  // namespace forge
