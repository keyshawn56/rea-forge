#include "io.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

namespace forge {

UniqueFd::~UniqueFd() { reset(); }

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int UniqueFd::release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

void UniqueFd::reset(int fd) {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

uint64_t fileSize(int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        throw std::runtime_error(std::string("fstat failed: ") + std::strerror(errno));
    }
    if (st.st_size < 0) {
        throw std::runtime_error("file reports a negative size");
    }
    if (st.st_size != 0) {
        return static_cast<uint64_t>(st.st_size);
    }
    // Some SAF providers report st_size=0 for virtual documents while still
    // supporting lseek. Prefer the seek end when it is larger.
    const off_t end = lseek(fd, 0, SEEK_END);
    if (end > 0) {
        return static_cast<uint64_t>(end);
    }
    return 0;
}

bool isSeekable(int fd) {
    const off_t current = lseek(fd, 0, SEEK_CUR);
    return current >= 0 || errno != ESPIPE;
}

void preadExact(int fd, uint64_t offset, void* dst, std::size_t size, const char* what) {
    auto* out = static_cast<uint8_t*>(dst);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t got = ::pread(fd, out + done, size - done,
                                    static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string(what) + ": pread failed: " + std::strerror(errno));
        }
        if (got == 0) {
            throw std::runtime_error(std::string(what) + ": unexpected end of file");
        }
        done += static_cast<std::size_t>(got);
    }
}

std::vector<uint8_t> readAt(int fd, uint64_t offset, std::size_t size, const char* what) {
    std::vector<uint8_t> out(size);
    if (size != 0) {
        preadExact(fd, offset, out.data(), size, what);
    }
    return out;
}

std::vector<uint8_t> readFile(const std::string& path, std::size_t maxBytes) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw std::runtime_error("cannot stat " + path + ": " + ec.message());
    }
    if (size > maxBytes) {
        throw std::runtime_error(path + " is larger than the analysis limit");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open " + path);
    }
    std::vector<uint8_t> out(static_cast<std::size_t>(size));
    if (!out.empty()) {
        input.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!input) {
            throw std::runtime_error("short read from " + path);
        }
    }
    return out;
}

void writeFile(const std::string& path, const void* data, std::size_t size) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw std::runtime_error("cannot create " + parent.string() + ": " + ec.message());
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write " + path);
    }
    if (size != 0) {
        output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    if (!output) {
        throw std::runtime_error("short write to " + path);
    }
}

std::string hex(const uint8_t* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = kDigits[data[i] >> 4];
        out[i * 2 + 1] = kDigits[data[i] & 0x0f];
    }
    return out;
}

std::string jsonEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
        }
    }
    return out;
}

std::string sanitizedName(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '/' || c == '\\' || c == ':' || c == '\0') {
            out.push_back('_');
        } else {
            out.push_back(ch);
        }
    }
    if (out.empty() || out == "." || out == "..") {
        return "unnamed";
    }
    return out;
}

std::string pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    if (a.back() == '/') {
        return a + b;
    }
    return a + "/" + b;
}

std::string fileName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool hasSuffix(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

std::string num(uint64_t value) { return std::to_string(value); }

}  // namespace forge
