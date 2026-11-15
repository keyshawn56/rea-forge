#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace forge {

struct PipelineCacheResult {
    bool valid = false;
    bool vulkan_available = false;
    bool cache_written = false;
    std::string error;
    std::string device_name;
    std::string driver_name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t driver_version = 0;
    uint32_t api_version = 0;
    uint32_t cache_bytes = 0;
    uint32_t shaders_staged = 0;
    uint32_t shaders_skipped = 0;
    uint32_t pipelines_compiled = 0;
    uint32_t pipelines_failed = 0;
    std::vector<std::string> diagnostics;
};

// Creates a Vulkan instance/device on the current Android device, stages the
// supplied SPIR-V modules, attempts pipeline creation, and serializes the
// driver pipeline cache to out_path. It never claims that game shaders were
// translated.
PipelineCacheResult buildPipelineCache(const std::vector<std::string>& spv_paths,
                                       const std::string& out_path);

std::string pipelineCacheToJson(const PipelineCacheResult& result);

}  // namespace forge
