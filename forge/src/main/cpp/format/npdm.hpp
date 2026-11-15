#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace forge {

struct NpdmInfo {
    bool valid = false;
    std::string name;
    uint64_t program_id_min = 0;
    uint64_t program_id_max = 0;
    uint32_t version = 0;
    uint32_t main_thread_stack_size = 0;
    uint32_t system_resource_size = 0;
    uint8_t flags0 = 0;
    uint8_t main_thread_priority = 0;
    uint8_t main_thread_core = 0;
    bool is_64_bit = false;
    uint8_t address_space = 0;
    bool production = false;
    uint32_t aci_offset = 0;
    uint32_t aci_size = 0;
    uint32_t acid_offset = 0;
    uint32_t acid_size = 0;
    bool aci_magic = false;
    bool acid_magic = false;
    std::vector<std::string> services;
    std::string diagnostic;
};

NpdmInfo inspectNpdm(const std::vector<uint8_t>& bytes);
std::string npdmToJson(const NpdmInfo& info);

}  // namespace forge
