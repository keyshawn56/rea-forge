#include "engine.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "sha256.hpp"

namespace rea {
namespace {

bool isSha256Hex(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    for (char c : value) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

std::string hex64(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%016" PRIx64, value);
    return buffer;
}

}  // namespace

Engine& Engine::instance() {
    static Engine engine;
    return engine;
}

Engine::~Engine() {
    shutdown();
}

void Engine::setLogger(std::function<void(int, const std::string&)> logger) {
    std::lock_guard<std::mutex> lock(mutex_);
    logger_ = std::move(logger);
}

void Engine::log(int level, const std::string& message) const {
    if (logger_) {
        logger_(level, message);
    }
}

bool Engine::boot(int elf_fd, int pak_fd, const std::string& sandbox_directory,
                  const std::string& expected_elf_sha256,
                  const std::string& expected_pak_sha256) {
    std::lock_guard<std::mutex> lock(mutex_);
    log(0, "boot gate: taking ownership of SAF descriptors");
    shutdownLocked();

    elf_fd_ = elf_fd;
    pak_fd_ = pak_fd;

    try {
        if (elf_fd < 0 || pak_fd < 0) {
            throw std::runtime_error("missing input descriptor");
        }
        if (elf_fd == pak_fd) {
            throw std::runtime_error("main.elf and assets.pak share a descriptor");
        }
        if (!isSha256Hex(expected_elf_sha256) || !isSha256Hex(expected_pak_sha256)) {
            throw std::runtime_error("manifest digests are not lowercase SHA-256 hex");
        }

        struct stat elf_stat {};
        struct stat pak_stat {};
        if (fstat(elf_fd, &elf_stat) != 0) {
            throw std::runtime_error(std::string("fstat(main.elf) failed: ") + std::strerror(errno));
        }
        if (fstat(pak_fd, &pak_stat) != 0) {
            throw std::runtime_error(std::string("fstat(assets.pak) failed: ") + std::strerror(errno));
        }
        if (elf_stat.st_size <= 0) {
            throw std::runtime_error("main.elf is empty");
        }
        if (pak_stat.st_size <= 0) {
            throw std::runtime_error("assets.pak is empty");
        }

        log(0, "boot gate: hashing main.elf (" + std::to_string(static_cast<long long>(elf_stat.st_size)) + " bytes)");
        elf_sha256_ = sha256Fd(elf_fd);
        if (elf_sha256_ != expected_elf_sha256) {
            throw std::runtime_error("main.elf digest mismatch: manifest says " +
                                     expected_elf_sha256 + ", descriptor has " + elf_sha256_);
        }

        log(0, "boot gate: hashing assets.pak (" + std::to_string(static_cast<long long>(pak_stat.st_size)) + " bytes)");
        pak_sha256_ = sha256Fd(pak_fd);
        if (pak_sha256_ != expected_pak_sha256) {
            throw std::runtime_error("assets.pak digest mismatch: manifest says " +
                                     expected_pak_sha256 + ", descriptor has " + pak_sha256_);
        }

        LoadOptions options;
        options.log = [this](int level, const std::string& message) { log(level, message); };
        options.resolve_hle_symbol = [this](const std::string& symbol) -> uint64_t {
            log(1, "HLE symbol not implemented in v0.1: " + symbol);
            return 0;
        };
        options.allow_writable_executable = false;

        image_ = ElfLoader::load(elf_fd, options);
        // PT_LOAD pages are anonymous copies; the ELF descriptor is no longer
        // needed once mapping and relocation are complete.
        close(elf_fd_);
        elf_fd_ = -1;

        pak_ = PakArchive::open(pak_fd);
        sandbox_ = sandbox_directory;
        booted_ = true;

        log(0, "boot gate passed");
        log(0, "image entry=" + hex64(image_->entry) +
                   " bias=" + hex64(image_->load_bias) +
                   " segments=" + std::to_string(image_->segments.size()) +
                   " exports=" + std::to_string(image_->exports.size()));
        if (!image_->build_id.empty()) {
            log(0, "image build-id=" + toHex(image_->build_id.data(), image_->build_id.size()));
        }
        log(0, "pak entries=" + std::to_string(pak_->entries().size()) +
                   " bytes=" + std::to_string(static_cast<long long>(pak_->fileSize())));
        log(1, "execution bridge and Vulkan presentation are not present in v0.1; "
               "the image is validated and mapped but is not run");
        return true;
    } catch (const std::exception& error) {
        log(2, std::string("boot gate rejected: ") + error.what());
        shutdownLocked();
        booted_ = false;
        return false;
    }
}

void Engine::configure(const EngineConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    EngineConfig next = config;
    next.width = next.width < 320 ? 320 : (next.width > 7680 ? 7680 : next.width);
    next.height = next.height < 240 ? 240 : (next.height > 4320 ? 4320 : next.height);
    if (next.shadow_size != 1024 && next.shadow_size != 2048 && next.shadow_size != 4096) {
        next.shadow_size = 2048;
    }
    if (next.anisotropy != 1 && next.anisotropy != 4 && next.anisotropy != 8 && next.anisotropy != 16) {
        next.anisotropy = 8;
    }
    if (next.crowd_percent < 25 || next.crowd_percent > 100) {
        next.crowd_percent = 100;
    }
    if (next.upscaler < 0 || next.upscaler > 2) {
        next.upscaler = 0;
    }
    if (next.frame_gen < 0 || next.frame_gen > 2) {
        next.frame_gen = 0;
    }
    config_ = next;

    std::ostringstream message;
    message << "configuration: " << config_.width << 'x' << config_.height
            << " shadow=" << config_.shadow_size
            << " af=" << config_.anisotropy
            << " crowd=" << config_.crowd_percent << '%'
            << " upscaler=" << config_.upscaler
            << " frameGen=" << config_.frame_gen
            << " lowLatency=" << (config_.low_latency ? 1 : 0);
    log(0, message.str());
}

std::string Engine::statsJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream json;
    json << '{';
    json << "\"booted\":" << (booted_ ? "true" : "false");
    json << ",\"execution\":\"" << (booted_ ? "load-only" : "idle") << '"';
    json << ",\"renderer\":\"absent\"";
    json << ",\"entry\":\"" << hex64(image_ ? image_->entry : 0) << '"';
    json << ",\"loadBias\":\"" << hex64(image_ ? image_->load_bias : 0) << '"';
    json << ",\"mappingBytes\":" << (image_ ? image_->mapping_size : 0);
    json << ",\"segments\":" << (image_ ? image_->segments.size() : 0);
    json << ",\"exports\":" << (image_ ? image_->exports.size() : 0);
    json << ",\"pakEntries\":" << (pak_ ? pak_->entries().size() : 0);
    json << ",\"pakBytes\":" << (pak_ ? pak_->fileSize() : 0);
    json << ",\"elfSha256\":\"" << elf_sha256_ << '"';
    json << ",\"pakSha256\":\"" << pak_sha256_ << '"';
    json << ",\"width\":" << config_.width;
    json << ",\"height\":" << config_.height;
    json << ",\"shadowSize\":" << config_.shadow_size;
    json << ",\"anisotropy\":" << config_.anisotropy;
    json << ",\"crowdPercent\":" << config_.crowd_percent;
    json << ",\"upscaler\":" << config_.upscaler;
    json << ",\"frameGen\":" << config_.frame_gen;
    json << ",\"lowLatency\":" << (config_.low_latency ? "true" : "false");
    json << '}';
    return json.str();
}

void Engine::shutdownLocked() {
    if (booted_ || image_ || pak_) {
        log(0, "shutdown: releasing image and pak");
    }
    pak_.reset();
    image_.reset();
    if (elf_fd_ >= 0) {
        close(elf_fd_);
        elf_fd_ = -1;
    }
    if (pak_fd_ >= 0) {
        close(pak_fd_);
        pak_fd_ = -1;
    }
    booted_ = false;
}

void Engine::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdownLocked();
}

}  // namespace rea
