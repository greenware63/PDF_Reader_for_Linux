#pragma once

namespace ui {

/**
 * @brief Viewport document display color modes for comfortable reading.
 */
enum class ColorMode {
    Normal = 0,       ///< Default full-color rendering on standard light background.
    Inverted = 1,     ///< Inverted RGB dark mode (white paper becomes black, black text becomes white).
    Sepia = 2,        ///< Warm paper tone (#FBF0D9) with dark brown text (#3E3024) for reduced blue light.
    HighContrast = 3  ///< High-contrast dark mode (#121212 background with crisp #F0F0F0 text).
};

} // namespace ui
