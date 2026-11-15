#include "npdm.hpp"

#include "../core/io.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace forge {
namespace {

constexpr uint32_t kMetaSize = 0x80;

std::string fixedString(const uint8_t* data, std::size_t size) {
    const auto end = std::find(data, data + size, 0);
    return std::string(reinterpret_cast<const char*>(data),
                       static_cast<std::size_t>(std::distance(data, end)));
}

}  // namespace

NpdmInfo inspectNpdm(const std::vector<uint8_t>& bytes) {
    NpdmInfo info;
    if (bytes.size() < kMetaSize) {
        info.diagnostic = "NPDM is smaller than the 0x80-byte Meta block";
        return info;
    }
    if (!hasMagic(bytes.data(), "META", 4)) {
        info.diagnostic = "NPDM Meta magic is not META";
        return info;
    }
    info.name = fixedString(bytes.data() + 0x20, 0x10);
    info.flags0 = bytes[0x0C];
    info.main_thread_priority = bytes[0x0E];
    info.main_thread_core = bytes[0x0F];
    info.system_resource_size = rd32(bytes.data() + 0x14);
    info.version = rd32(bytes.data() + 0x18);
    info.main_thread_stack_size = rd32(bytes.data() + 0x1C);
    info.aci_offset = rd32(bytes.data() + 0x70);
    info.aci_size = rd32(bytes.data() + 0x74);
    info.acid_offset = rd32(bytes.data() + 0x78);
    info.acid_size = rd32(bytes.data() + 0x7C);
    info.is_64_bit = (info.flags0 & 1u) != 0;
    info.address_space = static_cast<uint8_t>((info.flags0 >> 1) & 0x7u);

    const auto in_range = [&](uint32_t offset, uint32_t size) {
        return size != 0 && offset <= bytes.size() && size <= bytes.size() - offset;
    };
    const auto parse_service_table = [&](const uint8_t* table, uint32_t table_size) {
        // Each record is one prefix byte (low 3 bits = name length - 1) followed
        // by the service name. Records are packed without alignment padding.
        uint32_t pos = 0;
        while (pos < table_size && info.services.size() < 128) {
            const uint8_t prefix = table[pos];
            const uint32_t name_len = static_cast<uint32_t>(prefix & 0x7u) + 1;
            pos += 1;
            if (name_len > table_size - pos) {
                info.diagnostic = "NPDM service access table has a truncated name";
                break;
            }
            std::string service(reinterpret_cast<const char*>(table + pos), name_len);
            info.services.push_back(std::move(service));
            pos += name_len;
        }
    };

    if (!in_range(info.acid_offset, info.acid_size)) {
        info.diagnostic = "NPDM Acid block is outside the file";
    } else {
        const uint8_t* acid = bytes.data() + info.acid_offset;
        // On NX the signature is 0x100 and the key is 0x100, followed by ACID.
        if (info.acid_size >= 0x200 && hasMagic(acid + 0x200, "ACID", 4)) {
            info.acid_magic = true;
            const uint32_t body_size = info.acid_size - 0x200;
            if (body_size >= 0x38) {
                const uint8_t* body = acid + 0x200;
                const uint32_t flags = rd32(body + 0x0C);
                info.production = (flags & 1u) != 0;
                info.program_id_min = rd64(body + 0x10);
                info.program_id_max = rd64(body + 0x18);
            } else {
                info.diagnostic = "NPDM Acid body is smaller than its 0x38-byte header";
            }
        } else if (info.diagnostic.empty()) {
            info.diagnostic = "NPDM Acid signature block does not contain ACID at +0x200";
        }
    }

    if (!in_range(info.aci_offset, info.aci_size)) {
        if (info.diagnostic.empty()) {
            info.diagnostic = "NPDM ACI block is outside the file";
        }
    } else {
        const uint8_t* aci = bytes.data() + info.aci_offset;
        if (info.aci_size >= 0x30 && hasMagic(aci, "ACI0", 4)) {
            info.aci_magic = true;
            const uint32_t fac_offset = rd32(aci + 0x20);
            const uint32_t fac_size = rd32(aci + 0x24);
            const uint32_t sac_offset = rd32(aci + 0x28);
            const uint32_t sac_size = rd32(aci + 0x2C);
            if (fac_size != 0 && (fac_offset > info.aci_size || fac_size > info.aci_size - fac_offset)) {
                info.diagnostic = "NPDM FsAccessControl table is outside the ACI block";
            }
            if (sac_size != 0) {
                if (sac_offset > info.aci_size || sac_size > info.aci_size - sac_offset) {
                    info.diagnostic = "NPDM ServiceAccessControl table is outside the ACI block";
                } else {
                    parse_service_table(aci + sac_offset, sac_size);
                }
            }
        } else if (info.diagnostic.empty()) {
            info.diagnostic = "NPDM ACI block does not start with ACI0";
        }
    }

    if (info.system_resource_size > 0x1FE00000u) {
        info.diagnostic = "NPDM system resource size exceeds the documented maximum";
    }
    info.valid = info.acid_magic && info.aci_magic;
    if (!info.valid && info.diagnostic.empty()) {
        info.diagnostic = "NPDM is missing its Acid or ACI block";
    }
    return info;
}

std::string npdmToJson(const NpdmInfo& info) {
    std::ostringstream json;
    json << '{';
    json << "\"valid\":" << (info.valid ? "true" : "false");
    if (!info.valid) {
        json << ",\"diagnostic\":\"" << jsonEscape(info.diagnostic) << '"';
        json << '}';
        return json.str();
    }
    json << ",\"name\":\"" << jsonEscape(info.name) << '"';
    json << ",\"version\":" << info.version;
    json << ",\"is64Bit\":" << (info.is_64_bit ? "true" : "false");
    json << ",\"addressSpace\":" << static_cast<int>(info.address_space);
    json << ",\"mainThreadPriority\":" << static_cast<int>(info.main_thread_priority);
    json << ",\"mainThreadCore\":" << static_cast<int>(info.main_thread_core);
    json << ",\"mainThreadStackSize\":" << info.main_thread_stack_size;
    json << ",\"systemResourceSize\":" << info.system_resource_size;
    json << ",\"aciMagic\":" << (info.aci_magic ? "true" : "false");
    json << ",\"acidMagic\":" << (info.acid_magic ? "true" : "false");
    json << ",\"production\":" << (info.production ? "true" : "false");
    json << ",\"programIdMin\":\"" << std::hex << info.program_id_min << std::dec << '"';
    json << ",\"programIdMax\":\"" << std::hex << info.program_id_max << std::dec << '"';
    json << ",\"services\":[";
    for (std::size_t i = 0; i < info.services.size(); ++i) {
        if (i != 0) json << ',';
        json << '"' << jsonEscape(info.services[i]) << '"';
    }
    json << ']';
    if (!info.diagnostic.empty()) {
        json << ",\"diagnostic\":\"" << jsonEscape(info.diagnostic) << '"';
    }
    json << '}';
    return json.str();
}

}  // namespace forge
