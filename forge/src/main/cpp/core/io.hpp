#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace forge {

// Owning descriptor used by the JNI boundary. JNI entry points accept fds from
// ParcelFileDescriptor.detachFd(); this type guarantees they are closed even on
// a validation failure.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd();

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    int get() const { return fd_; }
    int release();
    void reset(int fd = -1);

private:
    int fd_ = -1;
};

uint64_t fileSize(int fd);
bool isSeekable(int fd);
void preadExact(int fd, uint64_t offset, void* dst, std::size_t size, const char* what);
std::vector<uint8_t> readAt(int fd, uint64_t offset, std::size_t size, const char* what);
std::vector<uint8_t> readFile(const std::string& path, std::size_t maxBytes = 1u << 30);
void writeFile(const std::string& path, const void* data, std::size_t size);

inline uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t rd64(const uint8_t* p) {
    return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32);
}
inline uint32_t rd32be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
inline bool hasMagic(const uint8_t* p, const char* magic, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != static_cast<uint8_t>(magic[i])) {
            return false;
        }
    }
    return true;
}

std::string hex(const uint8_t* data, std::size_t size);
std::string jsonEscape(std::string_view value);
std::string sanitizedName(std::string_view value);
std::string pathJoin(const std::string& a, const std::string& b);
std::string fileName(const std::string& path);
bool hasSuffix(std::string_view value, std::string_view suffix);

// Formats a number without locale-dependent separators.
std::string num(uint64_t value);

}  // namespace forge
