#include "vk_cache_builder.hpp"

#include "../core/io.hpp"
#include "../shader/shader_builder.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace forge {
namespace {

constexpr uint32_t kOpEntryPoint = 15;
constexpr uint32_t kOpTypeImage = 25;
constexpr uint32_t kOpTypeSampler = 26;
constexpr uint32_t kOpTypeSampledImage = 27;
constexpr uint32_t kOpTypeArray = 28;
constexpr uint32_t kOpTypeRuntimeArray = 29;
constexpr uint32_t kOpTypeStruct = 30;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpDecorate = 71;

constexpr uint32_t kStorageClassUniformConstant = 0;
constexpr uint32_t kStorageClassUniform = 2;
constexpr uint32_t kStorageClassPushConstant = 9;
constexpr uint32_t kStorageClassStorageBuffer = 12;

constexpr uint32_t kDecorationBinding = 33;
constexpr uint32_t kDecorationDescriptorSet = 34;

constexpr uint32_t kExecutionModelVertex = 0;
constexpr uint32_t kExecutionModelFragment = 4;
constexpr uint32_t kExecutionModelGLCompute = 5;

struct PointerType {
    uint32_t storage_class = 0;
    uint32_t pointee = 0;
};

struct ReflectBinding {
    uint32_t set = 0;
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
};

struct Reflection {
    uint32_t execution_model = UINT32_MAX;
    std::vector<ReflectBinding> bindings;
    bool has_push_constants = false;
    std::string error;
};

std::string executionModelName(uint32_t model) {
    switch (model) {
        case kExecutionModelVertex: return "vertex";
        case kExecutionModelFragment: return "fragment";
        case kExecutionModelGLCompute: return "compute";
        default: return "execution model " + std::to_string(model);
    }
}

Reflection reflectSpirv(const std::vector<uint8_t>& bytes) {
    Reflection reflection;
    if (bytes.size() < 20 || bytes.size() % 4 != 0) {
        reflection.error = "SPIR-V length is not a valid word stream";
        return reflection;
    }
    const uint32_t word_count = static_cast<uint32_t>(bytes.size() / 4);
    std::map<uint32_t, uint32_t> binding_of;
    std::map<uint32_t, uint32_t> set_of;
    std::map<uint32_t, PointerType> pointer_types;
    std::map<uint32_t, uint32_t> image_sampled;
    std::map<uint32_t, uint32_t> sampled_image_of;
    std::set<uint32_t> samplers;
    std::set<uint32_t> structs;
    std::map<uint32_t, uint32_t> array_element;
    std::map<uint32_t, uint32_t> variable_type;
    std::map<uint32_t, uint32_t> variable_storage;

    uint32_t i = 5;
    while (i < word_count) {
        const uint32_t instruction = rd32(bytes.data() + static_cast<std::size_t>(i) * 4);
        const uint32_t word_length = instruction >> 16;
        const uint32_t opcode = instruction & 0xFFFFu;
        if (word_length == 0 || i + word_length > word_count) {
            reflection.error = "SPIR-V instruction stream is truncated";
            return reflection;
        }
        const uint32_t* operands = reinterpret_cast<const uint32_t*>(
            bytes.data() + static_cast<std::size_t>(i) * 4);

        switch (opcode) {
            case kOpEntryPoint:
                if (word_length >= 3) {
                    reflection.execution_model = operands[1];
                }
                break;
            case kOpDecorate:
                if (word_length >= 4) {
                    const uint32_t target = operands[1];
                    const uint32_t decoration = operands[2];
                    if (decoration == kDecorationBinding) {
                        binding_of[target] = operands[3];
                    } else if (decoration == kDecorationDescriptorSet) {
                        set_of[target] = operands[3];
                    }
                }
                break;
            case kOpTypeImage:
                if (word_length >= 9) {
                    image_sampled[operands[1]] = operands[7];
                }
                break;
            case kOpTypeSampler:
                if (word_length >= 2) {
                    samplers.insert(operands[1]);
                }
                break;
            case kOpTypeSampledImage:
                if (word_length >= 3) {
                    sampled_image_of[operands[1]] = operands[2];
                }
                break;
            case kOpTypeArray:
            case kOpTypeRuntimeArray:
                if (word_length >= 3) {
                    array_element[operands[1]] = operands[2];
                }
                break;
            case kOpTypeStruct:
                if (word_length >= 2) {
                    structs.insert(operands[1]);
                }
                break;
            case kOpTypePointer:
                if (word_length >= 4) {
                    pointer_types[operands[1]] = PointerType{operands[2], operands[3]};
                }
                break;
            case kOpVariable:
                if (word_length >= 4) {
                    variable_type[operands[2]] = operands[1];
                    variable_storage[operands[2]] = operands[3];
                }
                break;
            default:
                break;
        }
        i += word_length;
    }

    if (reflection.execution_model == UINT32_MAX) {
        reflection.error = "SPIR-V has no OpEntryPoint";
        return reflection;
    }

    const auto descriptor_type = [&](uint32_t type) -> VkDescriptorType {
        uint32_t pointee = type;
        const auto pointer = pointer_types.find(type);
        if (pointer != pointer_types.end()) {
            pointee = pointer->second.pointee;
        }
        if (sampled_image_of.count(pointee) != 0) {
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        const auto image = image_sampled.find(pointee);
        if (image != image_sampled.end()) {
            return image->second == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                      : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        if (samplers.count(pointee) != 0) {
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        }
        if (structs.count(pointee) != 0) {
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        }
        return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    };

    for (const auto& [id, type] : variable_type) {
        const uint32_t storage = variable_storage[id];
        if (storage == kStorageClassPushConstant) {
            reflection.has_push_constants = true;
            continue;
        }
        if (storage != kStorageClassUniformConstant && storage != kStorageClassUniform &&
            storage != kStorageClassStorageBuffer) {
            continue;
        }
        const auto binding = binding_of.find(id);
        if (binding == binding_of.end()) {
            continue;
        }
        ReflectBinding entry;
        entry.set = set_of.count(id) != 0 ? set_of[id] : 0;
        entry.binding = binding->second;

        uint32_t pointee = type;
        const auto pointer = pointer_types.find(type);
        if (pointer != pointer_types.end()) {
            pointee = pointer->second.pointee;
        }
        if (storage == kStorageClassStorageBuffer) {
            entry.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        } else if (storage == kStorageClassUniform && structs.count(pointee) != 0) {
            entry.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        } else {
            entry.type = descriptor_type(type);
        }
        if (entry.type != VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            reflection.bindings.push_back(entry);
        }
    }

    std::sort(reflection.bindings.begin(), reflection.bindings.end(),
              [](const ReflectBinding& a, const ReflectBinding& b) {
                  if (a.set != b.set) return a.set < b.set;
                  return a.binding < b.binding;
              });
    return reflection;
}

void destroyLayouts(VkDevice device, std::vector<VkDescriptorSetLayout>& layouts) {
    for (VkDescriptorSetLayout layout : layouts) {
        if (layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
        }
    }
    layouts.clear();
}

bool createPipelineLayout(VkDevice device, const Reflection& reflection,
                          uint32_t max_push_constants,
                          VkPipelineLayout* out_layout,
                          std::vector<VkDescriptorSetLayout>* out_set_layouts,
                          std::string& error) {
    uint32_t max_set = 0;
    bool any = false;
    for (const auto& binding : reflection.bindings) {
        max_set = std::max(max_set, binding.set);
        any = true;
    }
    const uint32_t set_count = any ? max_set + 1 : 0;
    std::vector<std::vector<VkDescriptorSetLayoutBinding>> grouped(set_count);
    for (const auto& binding : reflection.bindings) {
        if (binding.set >= set_count) {
            continue;
        }
        VkDescriptorSetLayoutBinding item{};
        item.binding = binding.binding;
        item.descriptorType = binding.type;
        item.descriptorCount = 1;
        item.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        grouped[binding.set].push_back(item);
    }

    std::vector<VkDescriptorSetLayout> set_layouts(set_count, VK_NULL_HANDLE);
    for (uint32_t set = 0; set < set_count; ++set) {
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(grouped[set].size());
        info.pBindings = grouped[set].empty() ? nullptr : grouped[set].data();
        const VkResult result =
            vkCreateDescriptorSetLayout(device, &info, nullptr, &set_layouts[set]);
        if (result != VK_SUCCESS) {
            error = "vkCreateDescriptorSetLayout failed with VkResult " +
                    std::to_string(result);
            destroyLayouts(device, set_layouts);
            return false;
        }
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = std::min(128u, max_push_constants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.empty() ? nullptr : set_layouts.data();
    if (reflection.has_push_constants && push_range.size != 0) {
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;
    }
    const VkResult result = vkCreatePipelineLayout(device, &layout_info, nullptr, out_layout);
    if (result != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed with VkResult " + std::to_string(result);
        destroyLayouts(device, set_layouts);
        return false;
    }
    *out_set_layouts = std::move(set_layouts);
    return true;
}

VkShaderModule makeShaderModule(VkDevice device, const std::vector<uint8_t>& bytes,
                                std::string& error) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes.size();
    info.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(device, &info, nullptr, &module);
    if (result != VK_SUCCESS) {
        error = "vkCreateShaderModule failed with VkResult " + std::to_string(result);
        return VK_NULL_HANDLE;
    }
    return module;
}

}  // namespace

PipelineCacheResult buildPipelineCache(const std::vector<std::string>& spv_paths,
                                       const std::string& out_path) {
    PipelineCacheResult result;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "REA Forge";
    app.applicationVersion = 1;
    app.pEngineName = "REA Forge Pipeline Builder";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult vk = vkCreateInstance(&instance_info, nullptr, &instance);
    if (vk != VK_SUCCESS) {
        result.error = "vkCreateInstance failed with VkResult " + std::to_string(vk);
        return result;
    }
    result.vulkan_available = true;

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count == 0) {
        result.error = "no Vulkan physical device is available";
        vkDestroyInstance(instance, nullptr);
        return result;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
    VkPhysicalDevice physical = devices[0];
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    result.device_name = properties.deviceName;
    result.vendor_id = properties.vendorID;
    result.device_id = properties.deviceID;
    result.driver_version = properties.driverVersion;
    result.api_version = properties.apiVersion;

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            family = i;
            break;
        }
    }
    if (family == UINT32_MAX) {
        result.error = "the Vulkan device exposes no compute queue family";
        vkDestroyInstance(instance, nullptr);
        return result;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice device = VK_NULL_HANDLE;
    vk = vkCreateDevice(physical, &device_info, nullptr, &device);
    if (vk != VK_SUCCESS) {
        result.error = "vkCreateDevice failed with VkResult " + std::to_string(vk);
        vkDestroyInstance(instance, nullptr);
        return result;
    }

    VkPipelineCacheCreateInfo cache_info{};
    cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    VkPipelineCache cache = VK_NULL_HANDLE;
    vk = vkCreatePipelineCache(device, &cache_info, nullptr, &cache);
    if (vk != VK_SUCCESS) {
        result.error = "vkCreatePipelineCache failed with VkResult " + std::to_string(vk);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return result;
    }

    std::vector<std::vector<uint8_t>> bytes;
    std::vector<std::string> names;
    bytes.reserve(spv_paths.size());
    names.reserve(spv_paths.size());
    for (const auto& path : spv_paths) {
        try {
            auto data = readFile(path, 64u << 20);
            std::string error;
            if (!validateSpirv(data, error)) {
                result.diagnostics.push_back(std::filesystem::path(path).filename().string() +
                                             ": " + error);
                continue;
            }
            names.push_back(path);
            bytes.push_back(std::move(data));
            ++result.shaders_staged;
        } catch (const std::exception& error) {
            result.diagnostics.push_back(path + ": " + error.what());
        }
    }

    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const std::string filename = std::filesystem::path(names[i]).filename().string();
        const Reflection reflection = reflectSpirv(bytes[i]);
        if (!reflection.error.empty()) {
            result.diagnostics.push_back(filename + ": " + reflection.error);
            ++result.pipelines_failed;
            continue;
        }
        if (reflection.execution_model != kExecutionModelGLCompute) {
            ++result.shaders_skipped;
            result.diagnostics.push_back(
                filename + ": skipped because it is a " +
                executionModelName(reflection.execution_model) +
                " shader; the cache builder compiles compute pipelines");
            continue;
        }

        std::string error;
        VkShaderModule module = makeShaderModule(device, bytes[i], error);
        if (module == VK_NULL_HANDLE) {
            result.diagnostics.push_back(filename + ": " + error);
            ++result.pipelines_failed;
            continue;
        }

        VkPipelineLayout layout = VK_NULL_HANDLE;
        std::vector<VkDescriptorSetLayout> set_layouts;
        if (!createPipelineLayout(device, reflection, properties.limits.maxPushConstantsSize,
                                  &layout, &set_layouts, error)) {
            result.diagnostics.push_back(filename + ": " + error);
            ++result.pipelines_failed;
            vkDestroyShaderModule(device, module, nullptr);
            continue;
        }

        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult created =
            vkCreateComputePipelines(device, cache, 1, &pipeline_info, nullptr, &pipeline);
        if (created == VK_SUCCESS) {
            ++result.pipelines_compiled;
            vkDestroyPipeline(device, pipeline, nullptr);
        } else {
            ++result.pipelines_failed;
            result.diagnostics.push_back(
                filename + ": vkCreateComputePipelines returned " + std::to_string(created));
        }
        vkDestroyPipelineLayout(device, layout, nullptr);
        destroyLayouts(device, set_layouts);
        vkDestroyShaderModule(device, module, nullptr);
    }

    std::size_t cache_size = 0;
    vk = vkGetPipelineCacheData(device, cache, &cache_size, nullptr);
    if (vk == VK_SUCCESS && cache_size != 0) {
        std::vector<uint8_t> cache_bytes(cache_size);
        vk = vkGetPipelineCacheData(device, cache, &cache_size, cache_bytes.data());
        if (vk == VK_SUCCESS) {
            try {
                writeFile(out_path, cache_bytes.data(), cache_size);
                result.cache_bytes = static_cast<uint32_t>(cache_size);
                result.cache_written = true;
            } catch (const std::exception& error) {
                result.diagnostics.push_back(std::string("cache write failed: ") + error.what());
            }
        }
    }
    if (!result.cache_written) {
        result.diagnostics.push_back("the driver returned no serializable pipeline cache data");
    }

    result.valid = result.cache_written && result.pipelines_compiled > 0;
    result.diagnostics.push_back(
        "Only bundled post-process SPIR-V was staged; no game Maxwell shader was translated.");

    vkDestroyPipelineCache(device, cache, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return result;
}

std::string pipelineCacheToJson(const PipelineCacheResult& result) {
    std::ostringstream json;
    json << '{';
    json << "\"valid\":" << (result.valid ? "true" : "false");
    json << ",\"vulkanAvailable\":" << (result.vulkan_available ? "true" : "false");
    json << ",\"cacheWritten\":" << (result.cache_written ? "true" : "false");
    json << ",\"deviceName\":\"" << jsonEscape(result.device_name) << '"';
    json << ",\"driverName\":\"" << jsonEscape(result.driver_name) << '"';
    json << ",\"vendorId\":" << result.vendor_id;
    json << ",\"deviceId\":" << result.device_id;
    json << ",\"driverVersion\":" << result.driver_version;
    json << ",\"apiVersion\":" << result.api_version;
    json << ",\"cacheBytes\":" << result.cache_bytes;
    json << ",\"shadersStaged\":" << result.shaders_staged;
    json << ",\"shadersSkipped\":" << result.shaders_skipped;
    json << ",\"pipelinesCompiled\":" << result.pipelines_compiled;
    json << ",\"pipelinesFailed\":" << result.pipelines_failed;
    json << ",\"gameShadersIncluded\":false";
    if (!result.error.empty()) {
        json << ",\"error\":\"" << jsonEscape(result.error) << '"';
    }
    json << ",\"diagnostics\":[";
    for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
        if (i != 0) json << ',';
        json << '"' << jsonEscape(result.diagnostics[i]) << '"';
    }
    json << "]}";
    return json.str();
}

}  // namespace forge
