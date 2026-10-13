#pragma once

#include "engine/PixelBuffer.h"
#include <QImage>
#include <memory>

namespace ui {

/**
 * @brief Zero-copy conversion from engine PixelBuffer to QImage.
 * Keeps the backing buffer alive via custom cleanup callback.
 */
inline QImage toQImage(const pdf::PixelBuffer& buffer, double devicePixelRatio = 1.0) {
    if (!buffer.isValid()) {
        return {};
    }

    QImage::Format fmt = (buffer.format() == pdf::PixelFormat::BGRX8888)
                         ? QImage::Format_RGB32
                         : QImage::Format_ARGB32;

    auto owner = buffer.owner();
    QImage image(
        const_cast<uchar*>(buffer.data()),
        buffer.width(),
        buffer.height(),
        buffer.stride(),
        fmt,
        [](void* p) {
            auto* keepAlive = static_cast<std::shared_ptr<const void>*>(p);
            delete keepAlive;
        },
        new std::shared_ptr<const void>(owner)
    );

    if (devicePixelRatio > 0.0) {
        image.setDevicePixelRatio(devicePixelRatio);
    }
    return image;
}

} // namespace ui
