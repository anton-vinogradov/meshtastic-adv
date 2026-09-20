#pragma once
#include <algorithm>
#include <cerrno>
#include <sys/stat.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

inline uint32_t profileTestNow = 100000;
inline bool profileTestFontMounted = false;
inline uint32_t millis() { return profileTestNow; }
inline void delay(unsigned ms) { profileTestNow += ms; }
inline void profileTestLog(const char *, ...) {}
#define LOG_INFO(...) profileTestLog(__VA_ARGS__)
#define LOG_WARN(...) profileTestLog(__VA_ARGS__)
#define LOG_ERROR(...) profileTestLog(__VA_ARGS__)
#define FILE_READ "r"
#define FILE_WRITE "w"

struct ProfileTestEsp {
    uint64_t getEfuseMac() const { return 0x123456789abcULL; }
    void restart() {}
};
inline ProfileTestEsp ESP;
inline int SPI;

class File {
    std::shared_ptr<std::vector<uint8_t>> bytes;
    size_t cursor = 0;
    bool shortRead = false;
public:
    File() = default;
    explicit File(std::shared_ptr<std::vector<uint8_t>> b, bool shortReads = false)
        : bytes(std::move(b)), shortRead(shortReads) {}
    explicit operator bool() const { return bool(bytes); }
    int read(uint8_t *out, size_t n) {
        if (!bytes) return -1;
        n = std::min(n, bytes->size() - cursor);
        if (shortRead && n) n--;
        if (n) std::memcpy(out, bytes->data() + cursor, n);
        cursor += n;
        return static_cast<int>(n);
    }
    size_t write(const uint8_t *in, size_t n) {
        if (!bytes) return 0;
        bytes->resize(std::max(bytes->size(), cursor + n));
        if (n) std::memcpy(bytes->data() + cursor, in, n);
        cursor += n;
        return n;
    }
    size_t size() const { return bytes ? bytes->size() : 0; }
    size_t position() const { return cursor; }
    bool seek(size_t pos) {
        if (!bytes || pos > bytes->size()) return false;
        cursor = pos;
        return true;
    }
    void flush() {}
    void close() { bytes.reset(); }
};

namespace fs {
class FS {
public:
    std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> files;
    bool readFault = false;
    bool failReadOpen = false;
    bool shortReads = false;
    bool throwOnOpen = false;
    bool failMarkerRename = false;
    File open(const char *path, const char *mode) {
        if (throwOnOpen) throw std::bad_alloc();
        if (mode[0] == 'r') {
            if (readFault || failReadOpen) return {};
            auto it = files.find(path);
            return it == files.end() ? File() : File(it->second, shortReads);
        }
        auto &entry = files[path];
        entry = std::make_shared<std::vector<uint8_t>>();
        return File(entry);
    }
    bool exists(const char *path) const { return !readFault && files.count(path); }
    bool mkdir(const char *) { return true; }
    bool remove(const char *path) { return files.erase(path) != 0; }
    bool rename(const char *from, const char *to) {
        if (failMarkerRename && std::string(to) == "/advui_profile_v1") return false;
        auto it = files.find(from);
        if (it == files.end()) return false;
        files[to] = it->second;
        files.erase(it);
        return true;
    }
};
}
inline fs::FS profileTestInternalFs;
#define FSCom profileTestInternalFs
struct ProfileTestSd : fs::FS {
    bool mounted = false;
    bool throwOnBegin = false;
    unsigned ends = 0;
    bool begin(int, int &, uint32_t) {
        mounted = true;
        if (throwOnBegin) throw std::bad_alloc();
        return true;
    }
    void end() { mounted = false; ends++; }
};
inline ProfileTestSd SD;

namespace concurrency {
struct Lock {};
struct LockGuard { explicit LockGuard(Lock *) {} };
}
inline concurrency::Lock profileTestLock;
inline concurrency::Lock *spiLock = &profileTestLock;
namespace Throttle {
inline bool isWithinTimespanMs(uint32_t previous, uint32_t interval) {
    return millis() - previous < interval;
}
}

constexpr uint32_t CRC32_INITIAL = 0xffffffffU;
inline uint32_t crc32Update(const void *ptr, size_t size, uint32_t crc) {
    const auto *bytes = static_cast<const uint8_t *>(ptr);
    while (size--) {
        crc ^= *bytes++;
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
    }
    return crc;
}
inline uint32_t crc32Final(uint32_t crc) { return crc ^ 0xffffffffU; }
inline uint32_t crc32Buffer(const void *ptr, size_t size) {
    return crc32Final(crc32Update(ptr, size, CRC32_INITIAL));
}

// Fault injection at the same VFS boundary used by production metadata probes.
inline int profileTestStat(const char *path, struct stat *info) {
    if (SD.readFault) { errno = EIO; return -1; }
    if (std::strncmp(path, "/sd", 3) || !SD.files.count(path + 3)) {
        errno = ENOENT;
        return -1;
    }
    info->st_mode = S_IFREG;
    return 0;
}
#define stat(path, info) profileTestStat(path, info)
