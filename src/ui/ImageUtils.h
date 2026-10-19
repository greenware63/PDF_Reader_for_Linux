#pragma once

#include "engine/PixelBuffer.h"
#include "ColorMode.h"
#include <QImage>
#include <QColor>
#include <memory>
#include <array>
#include <algorithm>
#include <cmath>

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

/**
 * @brief Applies document color mode transformation (Invert, Sepia, HighContrast) to a QImage.
 * For Normal mode, returns the source image directly without copying.
 */
inline QImage applyColorMode(const QImage& src, ColorMode mode) {
    if (mode == ColorMode::Normal || src.isNull()) {
        return src;
    }

    QImage result = src.copy();
    if (mode == ColorMode::Inverted) {
        result.invertPixels(QImage::InvertRgb);
        return result;
    }

    if (result.format() != QImage::Format_ARGB32 &&
        result.format() != QImage::Format_RGB32 &&
        result.format() != QImage::Format_ARGB32_Premultiplied) {
        result = result.convertToFormat(QImage::Format_ARGB32);
    }

    const int w = result.width();
    const int h = result.height();

    if (mode == ColorMode::Sepia) {
        // Pre-computed 256-entry lookup table mapping luminance to warm parchment ink/paper
        // Paper background: #FBF0D9 (251, 240, 217)
        // Ink / dark text:  #3E3024 (62, 48, 36)
        static const auto sepiaLut = []() {
            std::array<std::array<uint8_t, 3>, 256> lut{};
            constexpr double r_ink = 62.0, g_ink = 48.0, b_ink = 36.0;
            constexpr double r_pap = 251.0, g_pap = 240.0, b_pap = 217.0;
            for (int i = 0; i < 256; ++i) {
                const double t = static_cast<double>(i) / 255.0;
                lut[i][0] = static_cast<uint8_t>(std::clamp(std::round(r_ink + (r_pap - r_ink) * t), 0.0, 255.0));
                lut[i][1] = static_cast<uint8_t>(std::clamp(std::round(g_ink + (g_pap - g_ink) * t), 0.0, 255.0));
                lut[i][2] = static_cast<uint8_t>(std::clamp(std::round(b_ink + (b_pap - b_ink) * t), 0.0, 255.0));
            }
            return lut;
        }();

        for (int y = 0; y < h; ++y) {
            auto* scanline = reinterpret_cast<QRgb*>(result.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb p = scanline[x];
                const int r = qRed(p);
                const int g = qGreen(p);
                const int b = qBlue(p);
                const int a = qAlpha(p);

                const int maxC = std::max({r, g, b});
                const int minC = std::min({r, g, b});
                const int lum = (299 * r + 587 * g + 114 * b) / 1000;

                if (maxC - minC < 28) {
                    scanline[x] = qRgba(sepiaLut[lum][0], sepiaLut[lum][1], sepiaLut[lum][2], a);
                } else {
                    const int nr = std::clamp((r * 251) / 255, 0, 255);
                    const int ng = std::clamp((g * 240) / 255, 0, 255);
                    const int nb = std::clamp((b * 217) / 255, 0, 255);
                    scanline[x] = qRgba(nr, ng, nb, a);
                }
            }
        }
    } else if (mode == ColorMode::HighContrast) {
        // High contrast dark mode:
        // Deep background: #121212 (18, 18, 18)
        // High contrast text: #F0F0F0 (240, 240, 240)
        static const auto contrastLut = []() {
            std::array<std::array<uint8_t, 3>, 256> lut{};
            for (int i = 0; i < 256; ++i) {
                const double v = (255.0 - static_cast<double>(i)) / 255.0;
                const double t = v * v * (3.0 - 2.0 * v);
                const int outVal = static_cast<int>(std::round(18.0 + (240.0 - 18.0) * t));
                const uint8_t clamped = static_cast<uint8_t>(std::clamp(outVal, 0, 255));
                lut[i][0] = clamped;
                lut[i][1] = clamped;
                lut[i][2] = clamped;
            }
            return lut;
        }();

        for (int y = 0; y < h; ++y) {
            auto* scanline = reinterpret_cast<QRgb*>(result.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb p = scanline[x];
                const int r = qRed(p);
                const int g = qGreen(p);
                const int b = qBlue(p);
                const int a = qAlpha(p);

                const int maxC = std::max({r, g, b});
                const int minC = std::min({r, g, b});
                const int lum = (299 * r + 587 * g + 114 * b) / 1000;

                if (maxC - minC < 32) {
                    scanline[x] = qRgba(contrastLut[lum][0], contrastLut[lum][1], contrastLut[lum][2], a);
                } else {
                    const int ir = 255 - r;
                    const int ig = 255 - g;
                    const int ib = 255 - b;
                    scanline[x] = qRgba(ir, ig, ib, a);
                }
            }
        }
    }

    return result;
}

} // namespace ui
