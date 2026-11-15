#pragma once
// Minimal SHA-256 used by the boot gate.
//
// The Kotlin gate hashes the SAF streams first; native code hashes the detached
// descriptors again before mapping anything, so a descriptor swapped between
// validation and boot cannot satisfy the check.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace forge {

class Sha256 {
public:
    Sha256();

    void update(const void* data, std::size_t size);
    std::array<uint8_t, 32> finish();
    std::string finishHex();

private:
    void compress(const uint8_t* block);

    std::array<uint32_t, 8> state_{};
    std::array<uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    uint64_t total_bytes_ = 0;
    bool finished_ = false;
};

// Hashes an open descriptor without closing it. Seeks to offset 0 first when
// the descriptor is seekable; sequential providers are read from their current
// position. Throws std::runtime_error on read failure.
std::string sha256Fd(int fd);

std::string toHex(const uint8_t* data, std::size_t size);

}  // namespace forge

