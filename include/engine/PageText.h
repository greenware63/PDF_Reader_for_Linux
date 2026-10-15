#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace pdf {

/**
 * @brief Character bounding box in top-down PDF page points (0, 0 is top-left of page).
 */
struct PageCharBox {
    double left{0.0};
    double top{0.0};
    double right{0.0};
    double bottom{0.0};
    char32_t unicode{0};

    bool isValid() const noexcept {
        return (right > left) && (bottom > top);
    }

    double width() const noexcept { return right - left; }
    double height() const noexcept { return bottom - top; }

    bool contains(double px, double py) const noexcept {
        return px >= left && px <= right && py >= top && py <= bottom;
    }
};

/**
 * @brief Extracted text and glyph geometry for a single PDF page.
 * Entirely decoupled from Qt and PDFium.
 */
struct PageText {
    int pageIndex{0};
    double widthPoints{0.0};
    double heightPoints{0.0};
    std::u16string text;
    std::vector<PageCharBox> chars;

    bool empty() const noexcept { return chars.empty(); }
    size_t charCount() const noexcept { return chars.size(); }

    /**
     * @brief Hit-tests a point (in top-down page points) against character bounding boxes.
     * @return 0-based character index, or -1 if no character is nearby.
     */
    int hitTest(double px, double py, double tolerance = 3.0) const noexcept {
        if (chars.empty()) {
            return -1;
        }

        // 1. Direct hit inside bounding box
        for (size_t i = 0; i < chars.size(); ++i) {
            if (chars[i].isValid() && chars[i].contains(px, py)) {
                return static_cast<int>(i);
            }
        }

        // 2. Tolerance hit: check characters whose vertical span contains py
        int bestIndex = -1;
        double bestDist = 1e9;
        for (size_t i = 0; i < chars.size(); ++i) {
            const auto& b = chars[i];
            if (!b.isValid()) {
                continue;
            }
            if (py >= b.top - tolerance && py <= b.bottom + tolerance) {
                double hDist = 0.0;
                if (px < b.left) {
                    hDist = b.left - px;
                } else if (px > b.right) {
                    hDist = px - b.right;
                }
                if (hDist < bestDist) {
                    bestDist = hDist;
                    bestIndex = static_cast<int>(i);
                }
            }
        }

        if (bestIndex >= 0 && bestDist <= 25.0) {
            return bestIndex;
        }

        // 3. Fallback: closest character within reasonable Euclidean distance (40 points)
        for (size_t i = 0; i < chars.size(); ++i) {
            const auto& b = chars[i];
            if (!b.isValid()) {
                continue;
            }
            double cx = (b.left + b.right) * 0.5;
            double cy = (b.top + b.bottom) * 0.5;
            double d = (cx - px) * (cx - px) + (cy - py) * (cy - py);
            if (d < bestDist) {
                bestDist = d;
                bestIndex = static_cast<int>(i);
            }
        }

        return (bestDist <= 1600.0) ? bestIndex : -1;
    }

    /**
     * @brief Computes merged highlight rectangles for a character range [startChar, endChar].
     * Supports both Left-To-Right (LTR) and Right-To-Left (RTL, e.g. Persian/Arabic) character sequences.
     */
    std::vector<PageCharBox> getSelectionRects(int startChar, int endChar) const {
        std::vector<PageCharBox> rects;
        if (chars.empty()) {
            return rects;
        }

        int first = std::clamp(std::min(startChar, endChar), 0, static_cast<int>(chars.size()) - 1);
        int last = std::clamp(std::max(startChar, endChar), 0, static_cast<int>(chars.size()) - 1);

        PageCharBox currentLine;
        bool hasCurrent = false;

        for (int i = first; i <= last; ++i) {
            const auto& b = chars[i];
            if (!b.isValid()) {
                continue;
            }

            if (!hasCurrent) {
                currentLine = b;
                hasCurrent = true;
            } else {
                double overlapY = std::min(currentLine.bottom, b.bottom) - std::max(currentLine.top, b.top);
                double minH = std::min(currentLine.height(), b.height());
                // Merge if vertical overlap is significant and horizontally proximate (LTR or RTL)
                bool ltrProximate = (b.left - currentLine.right) < 20.0 && (b.left >= currentLine.left - 5.0);
                bool rtlProximate = (currentLine.left - b.right) < 20.0 && (b.right <= currentLine.right + 5.0);
                if (overlapY > 0.5 * minH && (ltrProximate || rtlProximate)) {
                    currentLine.left = std::min(currentLine.left, b.left);
                    currentLine.right = std::max(currentLine.right, b.right);
                    currentLine.top = std::min(currentLine.top, b.top);
                    currentLine.bottom = std::max(currentLine.bottom, b.bottom);
                } else {
                    rects.push_back(currentLine);
                    currentLine = b;
                }
            }
        }

        if (hasCurrent) {
            rects.push_back(currentLine);
        }

        return rects;
    }

    /**
     * @brief Returns the UTF-16 substring corresponding to the character range [startChar, endChar].
     * Encodes 32-bit Unicode code points directly to UTF-16, generating surrogate pairs for
     * supplementary characters (> 0xFFFF) without character index misalignment.
     */
    std::u16string getSelectedText(int startChar, int endChar) const {
        if (chars.empty()) {
            return {};
        }
        int first = std::clamp(std::min(startChar, endChar), 0, static_cast<int>(chars.size()) - 1);
        int last = std::clamp(std::max(startChar, endChar), 0, static_cast<int>(chars.size()) - 1);

        std::u16string result;
        result.reserve((last - first + 1) * 2);
        for (int i = first; i <= last; ++i) {
            char32_t cp = chars[i].unicode;
            if (cp == 0) {
                continue;
            }
            if (cp <= 0xFFFF) {
                result.push_back(static_cast<char16_t>(cp));
            } else if (cp <= 0x10FFFF) {
                cp -= 0x10000;
                result.push_back(static_cast<char16_t>(0xD800 + ((cp >> 10) & 0x3FF)));
                result.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
            }
        }
        return result;
    }
};

} // namespace pdf
