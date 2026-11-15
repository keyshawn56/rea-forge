#include <jni.h>

#include <android/api-level.h>
#include <unistd.h>

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/io.hpp"
#include "format/npdm.hpp"
#include "format/nso.hpp"
#include "format/pfs0.hpp"
#include "format/romfs.hpp"
#include "gfx/vk_cache_builder.hpp"
#include "recomp/a64_scanner.hpp"
#include "shader/shader_builder.hpp"

namespace {

using forge::UniqueFd;

std::string getString(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        throw std::runtime_error("cannot read a JNI string (out of memory)");
    }
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

// Converts UTF-8 to UTF-16 so paths and diagnostics do not depend on JNI's
// modified-UTF-8 rules.
jstring toJString(JNIEnv* env, const std::string& utf8) {
    std::u16string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t code = 0;
        std::size_t extra = 0;
        if (c < 0x80) {
            code = c;
        } else if ((c & 0xE0) == 0xC0) {
            code = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            code = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            code = c & 0x07;
            extra = 3;
        } else {
            out.push_back(u'\uFFFD');
            ++i;
            continue;
        }
        if (i + extra >= utf8.size()) {
            out.push_back(u'\uFFFD');
            break;
        }
        bool valid = true;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            code = (code << 6) | (cc & 0x3F);
        }
        if (!valid) {
            out.push_back(u'\uFFFD');
            ++i;
            continue;
        }
        i += extra + 1;
        if (code <= 0xFFFF) {
            out.push_back(static_cast<char16_t>(code));
        } else if (code <= 0x10FFFF) {
            code -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(u'\uFFFD');
        }
    }
    return env->NewString(reinterpret_cast<const jchar*>(out.data()),
                          static_cast<jsize>(out.size()));
}

std::string errorJson(const std::string& message) {
    return "{\"valid\":false,\"error\":\"" + forge::jsonEscape(message) + "\"}";
}

UniqueFd borrowFd(jlong fd) {
    if (fd < 0 || fd > 0x7FFFFFFF) {
        throw std::runtime_error("invalid file descriptor");
    }
    const int copy = dup(static_cast<int>(fd));
    if (copy < 0) {
        throw std::runtime_error("cannot duplicate the SAF file descriptor");
    }
    return UniqueFd(copy);
}

std::string classifyEntry(const std::string& name) {
    if (forge::hasSuffix(name, ".nca")) return "nca";
    if (forge::hasSuffix(name, ".tik")) return "ticket";
    if (forge::hasSuffix(name, ".cert")) return "certificate";
    return "other";
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_rea_forge_NativeForge_nativeBuildInfo(JNIEnv* env, jclass) {
    std::ostringstream json;
    json << '{';
    json << "\"abi\":\"arm64-v8a\"";
    json << ",\"androidApi\":" << android_get_device_api_level();
    json << ",\"pageSize\":" << static_cast<long>(sysconf(_SC_PAGESIZE));
    json << ",\"nativeVersion\":\"0.2.0-forge\"";
    json << ",\"runtimeImplemented\":false";
    json << ",\"note\":\"validation and analysis library; it does not boot Horizon OS or NVN\"";
    json << '}';
    return toJString(env, json.str());
}

JNIEXPORT jstring JNICALL
Java_com_rea_forge_NativeForge_nativeValidateNsp(JNIEnv* env, jclass, jlong fd, jstring jname) {
    try {
        const std::string name = getString(env, jname);
        UniqueFd file = borrowFd(fd);
        const uint64_t size = forge::fileSize(file.get());
        if (!forge::isSeekable(file.get())) {
            throw std::runtime_error(
                "the SAF provider returned a non-seekable descriptor; copy the file to local storage first");
        }
        const auto archive = forge::parsePfs0(file.get(), 0, size);
        std::vector<forge::NcaInfo> ncas;
        std::vector<uint8_t> kinds;
        ncas.reserve(archive.entries.size());
        kinds.reserve(archive.entries.size());
        for (const auto& entry : archive.entries) {
            const std::string kind = classifyEntry(entry.name);
            kinds.push_back(kind == "nca" ? 0 : kind == "ticket" ? 1 : kind == "certificate" ? 2 : 3);
            if (kind == "nca" && entry.size >= 0x300) {
                const auto header = forge::readAt(file.get(), entry.absolute_offset, 0x300,
                                                 "NCA header");
                ncas.push_back(forge::inspectNcaHeader(header.data(), header.size()));
            } else {
                forge::NcaInfo info;
                info.diagnostic = "entry is too small to contain an NCA header";
                ncas.push_back(std::move(info));
            }
        }
        return toJString(env, forge::nspToJson(archive, ncas, kinds));
    } catch (const std::exception& error) {
        return toJString(env, errorJson(error.what()));
    } catch (...) {
        return toJString(env, errorJson("unknown native failure while validating the NSP"));
    }
}

JNIEXPORT jstring JNICALL
Java_com_rea_forge_NativeForge_nativeExtractNsp(JNIEnv* env, jclass, jlong fd, jstring jout) {
    try {
        const std::string out_dir = getString(env, jout);
        if (out_dir.empty()) {
            throw std::runtime_error("no output directory was supplied");
        }
        UniqueFd file = borrowFd(fd);
        const uint64_t size = forge::fileSize(file.get());
        if (!forge::isSeekable(file.get())) {
            throw std::runtime_error("the SAF descriptor is not seekable");
        }
        const auto archive = forge::parsePfs0(file.get(), 0, size);
        std::filesystem::create_directories(out_dir);
        uint64_t total = 0;
        std::ostringstream names;
        bool first = true;
        for (const auto& entry : archive.entries) {
            const std::string safe = forge::sanitizedName(entry.name);
            const std::string out_path = forge::pathJoin(out_dir, safe);
            total += forge::extractPfs0Entry(file.get(), archive, entry, out_path);
            if (!first) names << ',';
            first = false;
            names << '"' << forge::jsonEscape(safe) << '"';
        }
        std::ostringstream json;
        json << "{\"valid\":true,\"extracted\":true,\"bytes\":" << total;
        json << ",\"entries\":[" << names.str() << ']';
        json << ",\"warning\":\"PFS0 payloads are copied verbatim. Retail NCA content remains "
                "encrypted and was not decrypted.\"}";
        return toJString(env, json.str());
    } catch (const std::exception& error) {
        return toJString(env, errorJson(error.what()));
    } catch (...) {
        return toJString(env, errorJson("unknown native failure while extracting the NSP"));
    }
}

JNIEXPORT jstring JNICALL
Java_com_rea_forge_NativeForge_nativeInspectNso(JNIEnv* env, jclass, jlong fd, jstring jname) {
    try {
        UniqueFd file = borrowFd(fd);
        const auto image = forge::loadNso(file.get(), getString(env, jname), false);
        return toJString(env, forge::nsoToJson(image, false));
    } catch (const std::exception& error) {
        return toJString(env, errorJson(error.what()));
    } catch (...) {
        return toJString(env, errorJson("unknown native failure while reading the NSO"));
    }
}

JNIEXPORT jstring JNICALL
Java_com_rea_forge_NativeForge_nativeScanA64(JNIEnv* env, jclass, jlong fd, jstring jname) {
    try {
        UniqueFd file = borrowFd(fd);
        auto image = forge::loadNso(file.get(), getString(env, jname), true);
        if (image.sections.size() < 1) {
            throw std::runtime_error("NSO has no text section");
        }
        const auto scan = forge::scanA64Text(image.sections[0].bytes, true);
        return toJString(env, forge::a64ReportToJson(image, scan));
    } catch (const std::exception& error) {
        return toJString(env, errorJson(error.what()));
    } catch (...) {
        return toJString(env, errorJson("unknown native failure while scanning A64 code"));
    }
}

JNIEXPORT jstring JNICALL
Java_com_rea_forge_NativeForge_nativeInspectNpdm(JNIEnv* env, jclass, jlong fd, jstring jname) {
    try {
        UniqueFd file = borrowFd(fd);
        const uint64_t size = forge::fileSize(file.get());
        if (size == 0 || size > (16u << 20)) {
            throw std::runtime_error("NPDM size is outside the accepted 16 MiB range");
        }
        const auto bytes = forge::readAt(file.get(), 0, static_cast<std::size_t>(size), "NPDM");
        auto json = forge::npdmToJson(forge::inspectNpdm(bytes));
        return toJString(env, json);
    } catch (const std::exception& error) {
        return toJString(env, errorJson(error.what()));
    } catch (...) {
        return toJString(env, errorJson("unknown native failure while reading the NPDM"));
    }
}

JNIEXPORT jstring JNICALL
Java_com_rea_forge_NativeForge_nativeInspectRomFsImage(JNIEnv* env, jclass, jlong fd) {
    try {
        UniqueFd file = borrowFd(fd);
        const uint64_t size = forge::fileSize(file.get());
        return toJString(env, forge::romFsToJson(forge::inspectRomFsImage(file.get(), size)));
    } catch (const std::exception& error) {
        return toJString(env, errorJson(error.what()));
    } catch (...) {
        return toJString(env, errorJson("unknown native failure while reading the RomFS image"));
    }
}

JNIEXPORT jstring JNICALL
Java_com_rea_forge_NativeForge_nativeIndexRomFs(JNIEnv* env, jclass, jstring jpath) {
    try {
        const std::string path = getString(env, jpath);
        return toJString(env, forge::romFsToJson(forge::indexRomFsDirectory(path)));
    } catch (const std::exception& error) {
        return toJString(env, errorJson(error.what()));
    } catch (...) {
        return toJString(env, errorJson("unknown native failure while indexing RomFS"));
    }
}

JNIEXPORT jstring JNICALL
Java_com_rea_forge_NativeForge_nativeScanShaders(JNIEnv* env, jclass, jstring jpath) {
    try {
        const std::string path = getString(env, jpath);
        return toJString(env, forge::shaderInventoryToJson(forge::scanShaderDirectory(path)));
    } catch (const std::exception& error) {
        return toJString(env, errorJson(error.what()));
    } catch (...) {
        return toJString(env, errorJson("unknown native failure while scanning shaders"));
    }
}

JNIEXPORT jstring JNICALL
Java_com_rea_forge_NativeForge_nativeValidateSpirv(JNIEnv* env, jclass, jstring jpath) {
    try {
        const std::string path = getString(env, jpath);
        const auto bytes = forge::readFile(path, 64u << 20);
        std::string error;
        const bool valid = forge::validateSpirv(bytes, error);
        std::ostringstream json;
        json << "{\"valid\":" << (valid ? "true" : "false");
        json << ",\"bytes\":" << bytes.size();
        if (!valid) {
            json << ",\"error\":\"" << forge::jsonEscape(error) << '"';
        }
        json << '}';
        return toJString(env, json.str());
    } catch (const std::exception& error) {
        return toJString(env, errorJson(error.what()));
    } catch (...) {
        return toJString(env, errorJson("unknown native failure while validating SPIR-V"));
    }
}

JNIEXPORT jstring JNICALL
Java_com_rea_forge_NativeForge_nativeBuildPipelineCache(JNIEnv* env, jclass, jobjectArray jpaths,
                                                        jstring jout) {
    try {
        if (jpaths == nullptr) {
            throw std::runtime_error("no SPIR-V paths were supplied");
        }
        const jsize count = env->GetArrayLength(jpaths);
        std::vector<std::string> paths;
        paths.reserve(static_cast<std::size_t>(count));
        for (jsize i = 0; i < count; ++i) {
            auto item = static_cast<jstring>(env->GetObjectArrayElement(jpaths, i));
            paths.push_back(getString(env, item));
            env->DeleteLocalRef(item);
        }
        const std::string out_path = getString(env, jout);
        return toJString(env, forge::pipelineCacheToJson(
                                    forge::buildPipelineCache(paths, out_path)));
    } catch (const std::exception& error) {
        return toJString(env, errorJson(error.what()));
    } catch (...) {
        return toJString(env, errorJson("unknown native failure while building the pipeline cache"));
    }
}

}  // extern "C"
