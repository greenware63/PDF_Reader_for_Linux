#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>

namespace layout {

/**
 * @brief Pure C++20 bidirectional coordinate mapper between 64-bit document pixel space
 * and 32-bit GUI scrollbar integer ranges.
 * Fully decoupled from Qt or any GUI toolkit.
 */
class ScrollMapper {
public:
    static constexpr int32_t DEFAULT_MAX_SLIDER = 1000000000; // 10^9

    explicit constexpr ScrollMapper(int32_t maxSliderRange = DEFAULT_MAX_SLIDER) noexcept
        : m_maxSliderRange(maxSliderRange > 0 ? maxSliderRange : DEFAULT_MAX_SLIDER) {}

    /**
     * @brief Computes scrollbar slider parameters for given 64-bit document span and viewport size.
     * @param totalDocPixels Total document height or width in 64-bit device pixels.
     * @param viewportPixels Viewport height or width in 64-bit device pixels.
     * @param outMaxSlider Output maximum value of scrollbar range [0, outMaxSlider].
     * @param outPageStep Output page step for scrollbar.
     * @param outSingleStep Output single step for scrollbar (default 40 device pixels).
     */
    void computeRange(int64_t totalDocPixels, int64_t viewportPixels,
                      int32_t& outMaxSlider, int32_t& outPageStep, int32_t& outSingleStep,
                      int64_t singleStepPixels = 40) const noexcept {
        const int64_t maxDocScroll = std::max<int64_t>(0, totalDocPixels - viewportPixels);
        if (maxDocScroll == 0) {
            outMaxSlider = 0;
            outPageStep = static_cast<int32_t>(std::max<int64_t>(1, viewportPixels));
            outSingleStep = static_cast<int32_t>(std::max<int64_t>(1, singleStepPixels));
            return;
        }

        if (maxDocScroll <= m_maxSliderRange) {
            // 1:1 exact mapping: do not clamp pageStep to maxDocScroll so Qt thumb ratio reflects true viewport fraction
            outMaxSlider = static_cast<int32_t>(maxDocScroll);
            outPageStep = static_cast<int32_t>(std::max<int64_t>(1, viewportPixels));
            outSingleStep = static_cast<int32_t>(std::max<int64_t>(1, singleStepPixels));
        } else {
            // Compressed ratio mapping for enormous 64-bit documents
            outMaxSlider = m_maxSliderRange;
            double ratio = static_cast<double>(m_maxSliderRange) / static_cast<double>(maxDocScroll);
            outPageStep = std::max<int32_t>(1, static_cast<int32_t>(std::round(viewportPixels * ratio)));
            outSingleStep = std::max<int32_t>(1, static_cast<int32_t>(std::round(singleStepPixels * ratio)));
        }
    }

    /**
     * @brief Maps 32-bit scrollbar slider value to 64-bit document offset.
     */
    int64_t sliderToDoc(int32_t sliderVal, int64_t totalDocPixels, int64_t viewportPixels) const noexcept {
        const int64_t maxDocScroll = std::max<int64_t>(0, totalDocPixels - viewportPixels);
        if (maxDocScroll == 0 || sliderVal <= 0) {
            return 0;
        }
        if (maxDocScroll <= m_maxSliderRange) {
            return std::min<int64_t>(sliderVal, maxDocScroll);
        }
        int32_t clampedSlider = std::clamp<int32_t>(sliderVal, 0, m_maxSliderRange);
        double fraction = static_cast<double>(clampedSlider) / static_cast<double>(m_maxSliderRange);
        return static_cast<int64_t>(std::round(fraction * static_cast<double>(maxDocScroll)));
    }

    /**
     * @brief Maps 64-bit document offset to 32-bit scrollbar slider value.
     */
    int32_t docToSlider(int64_t docPixel, int64_t totalDocPixels, int64_t viewportPixels) const noexcept {
        const int64_t maxDocScroll = std::max<int64_t>(0, totalDocPixels - viewportPixels);
        if (maxDocScroll == 0 || docPixel <= 0) {
            return 0;
        }
        int64_t clampedDoc = std::clamp<int64_t>(docPixel, 0, maxDocScroll);
        if (maxDocScroll <= m_maxSliderRange) {
            return static_cast<int32_t>(clampedDoc);
        }
        double fraction = static_cast<double>(clampedDoc) / static_cast<double>(maxDocScroll);
        return static_cast<int32_t>(std::round(fraction * static_cast<double>(m_maxSliderRange)));
    }

    /**
     * @brief Remaps a scrollbar slider value across a viewport resize for a document.
     * Accurately converts old slider to document coordinates using old viewport, then maps to new slider.
     * Prevents combining stale slider values with new viewport dimensions in compressed mode (> 10^9).
     */
    int32_t remapSliderOnResize(int32_t oldSlider, int64_t totalDocPixels, int64_t oldVpPixels, int64_t newVpPixels) const noexcept {
        int64_t doc = sliderToDoc(oldSlider, totalDocPixels, oldVpPixels);
        return docToSlider(doc, totalDocPixels, newVpPixels);
    }

    int32_t maxSliderRange() const noexcept { return m_maxSliderRange; }

private:
    int32_t m_maxSliderRange{DEFAULT_MAX_SLIDER};
};

} // namespace layout
