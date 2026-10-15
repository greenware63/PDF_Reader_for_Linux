#pragma once

// POSIX shared-memory transport for rendered tiles (Linux).
//
// Why not QSharedMemory: Qt's Linux backend is System V shared memory, which creates a key file under the
// temp directory (/tmp/qipc_*). The pdf_worker Landlock policy only allows writes under /dev/shm, so
// QSharedMemory::create() fails inside the sandbox and no tile can ever be returned. shm_open() creates its
// object in /dev/shm, which is exactly what the sandbox permits.
//
// The UI process treats the worker as untrusted: it validates the segment name, checks the size, and copies
// with pread() instead of mmap(), so a worker that shrinks the segment after announcing it cannot cause a
// SIGBUS in the UI.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <QSharedMemory>
#include <QString>
#include <memory>
#endif

namespace pdf::ipc {

/// True for names this protocol generates: "pdf-tile-<decimal pid>-<1..16 hex digits>".
inline bool isValidSharedTileKey(const std::string& key) {
    constexpr const char* prefix = "pdf-tile-";
    constexpr size_t prefixLen = 9;
    if (key.size() < prefixLen + 3 || key.size() > 64 || key.compare(0, prefixLen, prefix) != 0) {
        return false;
    }
    size_t i = prefixLen;
    size_t digits = 0;
    while (i < key.size() && key[i] >= '0' && key[i] <= '9') {
        ++i;
        ++digits;
    }
    if (digits == 0 || i >= key.size() || key[i] != '-') {
        return false;
    }
    ++i;
    size_t hex = 0;
    for (; i < key.size(); ++i, ++hex) {
        const char c = key[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return hex >= 1 && hex <= 16;
}

#ifndef _WIN32
/// Worker side: owns one segment. The destructor unmaps and unlinks it.
class SharedTileSegment {
public:
    SharedTileSegment() = default;
    SharedTileSegment(const SharedTileSegment&) = delete;
    SharedTileSegment& operator=(const SharedTileSegment&) = delete;
    ~SharedTileSegment() { release(); }

    /// Creates a new segment of @p bytes. @return false on any failure (nothing is left behind).
    bool create(const std::string& key, size_t bytes) {
        release();
        if (!isValidSharedTileKey(key) || bytes == 0) {
            return false;
        }
        const std::string name = "/" + key;
        const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0) {
            return false;
        }
        if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
            ::close(fd);
            ::shm_unlink(name.c_str());
            return false;
        }
        void* mem = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd); // the mapping stays valid
        if (mem == MAP_FAILED) {
            ::shm_unlink(name.c_str());
            return false;
        }
        m_name = name;
        m_data = static_cast<uint8_t*>(mem);
        m_size = bytes;
        return true;
    }

    uint8_t* data() noexcept { return m_data; }
    size_t size() const noexcept { return m_size; }

    void release() noexcept {
        if (m_data) {
            ::munmap(m_data, m_size);
            m_data = nullptr;
        }
        if (!m_name.empty()) {
            ::shm_unlink(m_name.c_str());
            m_name.clear();
        }
        m_size = 0;
    }

private:
    std::string m_name;
    uint8_t* m_data{nullptr};
    size_t m_size{0};
};

/**
 * UI side: copies a tile out of the worker's segment row by row and unlinks the segment (the UI is not
 * sandboxed, and unlinking here also stops a crashed worker from leaking /dev/shm entries).
 * @param rowBytes bytes copied per row; @param stride source row pitch; @param dstStride destination pitch.
 * @return false if the name is invalid, the segment is missing/too small, or any read comes up short.
 */
inline bool readAndUnlinkSharedTile(const std::string& key, size_t height, size_t stride, size_t rowBytes,
                                    uint8_t* dst, size_t dstStride) {
    if (!isValidSharedTileKey(key)) {
        return false;
    }
    const std::string name = "/" + key;
    const int fd = ::shm_open(name.c_str(), O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    bool ok = false;
    struct stat st {};
    if (::fstat(fd, &st) == 0 && st.st_size >= 0 && stride >= rowBytes && height > 0 &&
        static_cast<uint64_t>(st.st_size) >= static_cast<uint64_t>(stride) * height) {
        ok = true;
        for (size_t row = 0; row < height && ok; ++row) {
            const ssize_t got = ::pread(fd, dst + row * dstStride, rowBytes, static_cast<off_t>(row * stride));
            ok = (got == static_cast<ssize_t>(rowBytes));
        }
    }
    ::close(fd);
    ::shm_unlink(name.c_str());
    return ok;
}

#else

// Windows implementation: wraps QSharedMemory with identical semantics
class SharedTileSegment {
public:
    SharedTileSegment() = default;
    SharedTileSegment(const SharedTileSegment&) = delete;
    SharedTileSegment& operator=(const SharedTileSegment&) = delete;
    ~SharedTileSegment() { release(); }

    bool create(const std::string& key, size_t bytes) {
        release();
        if (!isValidSharedTileKey(key) || bytes == 0) {
            return false;
        }
        m_shm = std::make_unique<QSharedMemory>(QString::fromStdString(key));
        if (!m_shm->create(static_cast<qsizetype>(bytes))) {
            m_shm.reset();
            return false;
        }
        return true;
    }

    uint8_t* data() noexcept {
        return m_shm ? static_cast<uint8_t*>(m_shm->data()) : nullptr;
    }

    size_t size() const noexcept {
        return m_shm ? static_cast<size_t>(m_shm->size()) : 0;
    }

    void release() noexcept {
        if (m_shm) {
            m_shm->detach();
            m_shm.reset();
        }
    }

private:
    std::unique_ptr<QSharedMemory> m_shm;
};

inline bool readAndUnlinkSharedTile(const std::string& key, size_t height, size_t stride, size_t rowBytes,
                                    uint8_t* dst, size_t dstStride) {
    if (!isValidSharedTileKey(key)) {
        return false;
    }
    QSharedMemory shm(QString::fromStdString(key));
    if (!shm.attach(QSharedMemory::ReadOnly)) {
        return false;
    }
    const size_t minBytes = stride * height;
    if (static_cast<size_t>(shm.size()) < minBytes || stride < rowBytes || height == 0) {
        shm.detach();
        return false;
    }
    const uint8_t* src = static_cast<const uint8_t*>(shm.constData());
    for (size_t row = 0; row < height; ++row) {
        std::memcpy(dst + row * dstStride, src + row * stride, rowBytes);
    }
    shm.detach();
    return true;
}

#endif

} // namespace pdf::ipc
