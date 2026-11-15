#include "a64_scanner.hpp"

#include "../core/io.hpp"
#include "../core/sha256.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <set>
#include <sstream>

namespace forge {
namespace {

constexpr uint32_t kSvcMask = 0xFFE0001Fu;
constexpr uint32_t kSvcValue = 0xD4000001u;
constexpr uint32_t kMrsMask = 0xFFF00000u;
constexpr uint32_t kMrsBase = 0xD5300000u;
constexpr uint32_t kMsrMask = 0xFFF00000u;
constexpr uint32_t kMsrBase = 0xD5100000u;
constexpr uint32_t kBrMask = 0xFFFFFC1Fu;
constexpr uint32_t kBrValue = 0xD61F0000u;
constexpr uint32_t kBlrValue = 0xD63F0000u;
constexpr uint32_t kRetValue = 0xD65F0000u;

std::string sysregName(uint32_t insn) {
    const uint32_t encoded = insn & 0xFFFFF;
    const unsigned op0 = 2 + ((insn >> 19) & 1u);
    const unsigned op1 = (insn >> 16) & 0x7u;
    const unsigned crn = (insn >> 12) & 0xFu;
    const unsigned crm = (insn >> 8) & 0xFu;
    const unsigned op2 = (insn >> 5) & 0x7u;

    struct Known {
        unsigned op0, op1, crn, crm, op2;
        const char* name;
    };
    static constexpr std::array<Known, 18> known = {{
        {3, 3, 13, 0, 2, "TPIDR_EL0"},
        {3, 3, 13, 0, 3, "TPIDRRO_EL0"},
        {3, 3, 14, 0, 2, "CNTVCT_EL0"},
        {3, 3, 14, 0, 0, "CNTFRQ_EL0"},
        {3, 3, 4, 0, 0, "FPCR"},
        {3, 3, 4, 0, 1, "FPSR"},
        {3, 3, 4, 2, 0, "DAIF"},
        {3, 0, 4, 2, 0, "CurrentEL"},
        {3, 0, 4, 1, 0, "SP_EL0"},
        {3, 0, 0, 0, 5, "MPIDR_EL1"},
        {3, 0, 0, 0, 0, "MIDR_EL1"},
        {3, 0, 0, 0, 1, "CTR_EL0"},
        {3, 3, 0, 0, 7, "DCZID_EL0"},
        {3, 0, 1, 0, 0, "SCTLR_EL1"},
        {3, 0, 2, 0, 0, "TTBR0_EL1"},
        {3, 0, 2, 0, 2, "TCR_EL1"},
        {3, 0, 5, 0, 0, "ESR_EL1"},
        {3, 0, 5, 2, 0, "FAR_EL1"},
    }};
    for (const auto& item : known) {
        if (item.op0 == op0 && item.op1 == op1 && item.crn == crn && item.crm == crm &&
            item.op2 == op2) {
            return item.name;
        }
    }
    std::ostringstream name;
    name << "S" << op0 << '_' << op1 << '_' << crn << '_' << crm << '_' << op2;
    (void)encoded;
    return name.str();
}

}  // namespace

A64Scan scanA64Text(const std::vector<uint8_t>& text, bool is_64_bit) {
    A64Scan scan;
    scan.instruction_count = text.size() / 4;
    std::set<std::string> registers;

    Sha256 hash;
    hash.update(text.data(), text.size());
    const auto digest = hash.finish();
    scan.text_sha256 = forge::hex(digest.data(), digest.size());

    for (std::size_t offset = 0; offset + 4 <= text.size(); offset += 4) {
        const uint32_t insn = rd32(text.data() + offset);

        if ((insn & kSvcMask) == kSvcValue) {
            ++scan.svc_sites;
        }
        if ((insn & kMrsMask) == kMrsBase) {
            const std::string name = sysregName(insn);
            ++scan.system_register_accesses;
            registers.insert(name);
            if (name == "TPIDRRO_EL0") {
                ++scan.tpidrro_reads;
            }
        }
        if ((insn & kMsrMask) == kMsrBase) {
            const std::string name = sysregName(insn);
            ++scan.system_register_accesses;
            registers.insert(name);
            if (name == "TPIDRRO_EL0") {
                ++scan.tpidrro_writes;
            }
        }

        if ((insn & kBrMask) == kBrValue) {
            ++scan.branch_register;
        } else if ((insn & kBrMask) == kBlrValue) {
            ++scan.call_register;
        } else if ((insn & kBrMask) == kRetValue) {
            ++scan.returns;
        }

        if ((insn & 0xFC000000u) == 0x14000000u) {
            ++scan.direct_branches;
        } else if ((insn & 0xFC000000u) == 0x94000000u) {
            ++scan.direct_calls;
        }

        // DMB/DSB/ISB share the 0xD5033xxx hint/system space. The option field
        // lives in bits 11:8, so it is masked out before comparison.
        if ((insn & 0xFFFFF0FFu) == 0xD50330BFu || (insn & 0xFFFFF0FFu) == 0xD503309Fu ||
            insn == 0xD5033FDFu) {
            ++scan.barriers;
        }
        if ((insn & 0xFFFFF01Fu) == 0xD503201Fu) {
            ++scan.hint_instructions;
        }
    }

    scan.system_registers.assign(registers.begin(), registers.end());
    if (!is_64_bit) {
        scan.notes.push_back("NPDM does not advertise a 64-bit address space; A64 assumptions may be wrong");
    }
    if (scan.svc_sites != 0) {
        scan.notes.push_back(std::to_string(scan.svc_sites) +
                             " SVC sites require a Horizon kernel service layer");
    }
    if (scan.tpidrro_reads + scan.tpidrro_writes != 0) {
        scan.notes.push_back("TPIDRRO_EL0 accesses require per-thread TLS remapping");
    }
    scan.notes.push_back("static analysis only: no AArch64 image or shared object is produced");
    return scan;
}

std::string a64ReportToJson(const NsoImage& image, const A64Scan& scan) {
    std::ostringstream json;
    json << '{';
    json << "\"valid\":true";
    json << ",\"mode\":\"analysis-only\"";
    json << ",\"recompiled\":false";
    json << ",\"executable\":false";
    json << ",\"nso\":" << nsoToJson(image, false);
    json << ",\"scan\":{";
    json << "\"instructions\":" << scan.instruction_count;
    json << ",\"textSha256\":\"" << scan.text_sha256 << '"';
    json << ",\"svcSites\":" << scan.svc_sites;
    json << ",\"tpidrroReads\":" << scan.tpidrro_reads;
    json << ",\"tpidrroWrites\":" << scan.tpidrro_writes;
    json << ",\"branchRegister\":" << scan.branch_register;
    json << ",\"callRegister\":" << scan.call_register;
    json << ",\"returns\":" << scan.returns;
    json << ",\"directBranches\":" << scan.direct_branches;
    json << ",\"directCalls\":" << scan.direct_calls;
    json << ",\"barriers\":" << scan.barriers;
    json << ",\"hints\":" << scan.hint_instructions;
    json << ",\"systemRegisterAccesses\":" << scan.system_register_accesses;
    json << ",\"systemRegisters\":[";
    for (std::size_t i = 0; i < scan.system_registers.size(); ++i) {
        if (i != 0) json << ',';
        json << '"' << jsonEscape(scan.system_registers[i]) << '"';
    }
    json << "]}";

    json << ",\"blockers\":[";
    json << "\"Horizon OS kernel services (SVC) are not implemented\"";
    json << ",\"Horizon IPC and applet services are not implemented\"";
    json << ",\"NVN graphics/command submission is not translated to Vulkan\"";
    json << ",\"AArch64 code from the NSO is not recompiled or emitted\"";
    json << ",\"Title-specific memory layout and relocation handling is not implemented\"";
    json << "],\"notes\":[";
    for (std::size_t i = 0; i < scan.notes.size(); ++i) {
        if (i != 0) json << ',';
        json << '"' << jsonEscape(scan.notes[i]) << '"';
    }
    json << "]}";
    return json.str();
}

}  // namespace forge
