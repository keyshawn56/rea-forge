#pragma once
// Owns the validated boot state: the mapped ELF image, the pak archive, and the
// active graphics configuration.
//
// v0.1 stops after validation and mapping. Guest execution and Vulkan
// presentation are separate bodies of work; stats() reports them as disabled so
// the UI never implies that the image is running.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "elf_loader.hpp"
#include "pak_archive.hpp"

namespace rea {

struct EngineConfig {
    int width = 1920;
    int height = 1080;
    int shadow_size = 2048;
    int anisotropy = 8;
    int crowd_percent = 100;
    int upscaler = 1;       // 0 off, 1 FSR 1, 2 GSR
    int frame_gen = 0;      // 0 off, 1 90 Hz, 2 120 Hz
    bool low_latency = false;
};

class Engine {
public:
    static Engine& instance();

    void setLogger(std::function<void(int, const std::string&)> logger);

    // Takes ownership of both descriptors, including on failure. Returns true
    // only when both expected digests match and the ELF/pak parse successfully.
    bool boot(int elf_fd, int pak_fd, const std::string& sandbox_directory,
              const std::string& expected_elf_sha256,
              const std::string& expected_pak_sha256);

    void configure(const EngineConfig& config);
    std::string statsJson() const;
    void shutdown();

private:
    Engine() = default;
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void shutdownLocked();
    void log(int level, const std::string& message) const;

    mutable std::mutex mutex_;
    std::function<void(int, const std::string&)> logger_;
    std::unique_ptr<LoadedImage> image_;
    std::unique_ptr<PakArchive> pak_;
    int elf_fd_ = -1;
    int pak_fd_ = -1;
    EngineConfig config_;
    std::string sandbox_;
    std::string elf_sha256_;
    std::string pak_sha256_;
    bool booted_ = false;
};

}  // namespace rea
