#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "format/nso.hpp"

namespace forge {

struct A64Scan {
    uint64_t instruction_count = 0;
    uint64_t svc_sites = 0;
    uint64_t tpidrro_reads = 0;
    uint64_t tpidrro_writes = 0;
    uint64_t branch_register = 0;
    uint64_t call_register = 0;
    uint64_t returns = 0;
    uint64_t direct_branches = 0;
    uint64_t direct_calls = 0;
    uint64_t barriers = 0;
    uint64_t hint_instructions = 0;
    uint64_t system_register_accesses = 0;
    std::vector<std::string> system_registers;
    std::vector<std::string> notes;
    std::string text_sha256;
};

// Statically scans a decompressed NSO .text section. This is diagnostics only:
// the scanner does not emit AArch64 code, an ELF, a shared object, or a
// runnable image, and it cannot make Horizon OS services exist on Android.
A64Scan scanA64Text(const std::vector<uint8_t>& text, bool is_64_bit);

// Emits the scan plus an explicit recompilation/runtime feasibility report.
std::string a64ReportToJson(const NsoImage& image, const A64Scan& scan);

}  // namespace forge
