#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <cstdlib>

namespace pdf {

enum class PixelFormat {
    BGRX8888, // 32-bit little-endian: B, G, R, unused (maps to QImage::Format_RGB32)
    BGRA8888  // 32-bit little-endian: B, G, R, A
};

struct PixelRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    bool isValid() const { return width > 0 && height > 0; }
};

/**
 * @brief Zero-dependency pixel buffer abstraction.
 * Holds or views raster pixel data without coupling to Qt or any specific memory allocator.
 * Compatible with heap memory, shared memory mappings (memfd), or pre-allocated framebuffers.
 */
class PixelBuffer {
public:
    PixelBuffer() = default;

    /**
     * @brief Allocates an owned pixel buffer.
     */
    static PixelBuffer allocate(int width, int height, PixelFormat format = PixelFormat::BGRX8888) {
        if (width <= 0 || height <= 0) {
            return {};
        }

        const int bytesPerPixel = 4;
        const int stride = width * bytesPerPixel;
        const size_t totalBytes = static_cast<size_t>(stride) * height;

        // Custom shared_ptr deleter using free()
        void* rawMem = std::malloc(totalBytes);
        if (!rawMem) {
            return {};
        }

        std::shared_ptr<void> owner(rawMem, [](void* p) { std::free(p); });

        PixelBuffer buf;
        buf.m_data = static_cast<uint8_t*>(rawMem);
        buf.m_width = width;
        buf.m_height = height;
        buf.m_stride = stride;
        buf.m_format = format;
        buf.m_owner = std::move(owner);
        return buf;
    }

    /**
     * @brief Wraps an existing external memory region (e.g. memfd mapping).
     */
    static PixelBuffer wrap(uint8_t* data, int width, int height, int stride,
                            PixelFormat format, std::shared_ptr<const void> owner) {
        PixelBuffer buf;
        buf.m_data = data;
        buf.m_width = width;
        buf.m_height = height;
        buf.m_stride = stride;
        buf.m_format = format;
        buf.m_owner = std::move(owner);
        return buf;
    }

    int width() const { return m_width; }
    int height() const { return m_height; }
    int stride() const { return m_stride; }
    PixelFormat format() const { return m_format; }
    uint8_t* data() { return m_data; }
    const uint8_t* data() const { return m_data; }
    size_t sizeBytes() const { return static_cast<size_t>(m_stride) * m_height; }
    bool isValid() const { return m_data != nullptr && m_width > 0 && m_height > 0; }
    std::shared_ptr<const void> owner() const { return m_owner; }

private:
    uint8_t* m_data{nullptr};
    int m_width{0};
    int m_height{0};
    int m_stride{0};
    PixelFormat m_format{PixelFormat::BGRX8888};
    std::shared_ptr<const void> m_owner;
};

} // namespace pdf
