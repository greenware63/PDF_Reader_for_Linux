#pragma once
#include <mutex>

namespace pdf {

/**
 * @brief Process-wide recursive mutex guarding all PDFium FPDF_* calls across the entire process.
 * Guarantees thread safety even if multiple workers or background operations are initialized.
 */
inline std::recursive_mutex& getPdfiumMutex() {
    static std::recursive_mutex s_mutex;
    return s_mutex;
}

} // namespace pdf
