#pragma once

#include "PixelBuffer.h"
#include <QImage>

namespace pdf {

/**
 * @brief Abstract interface representing a single PDF page.
 * Keeps the UI / Viewport layer independent from the underlying engine.
 */
class IPage {
public:
    virtual ~IPage() = default;

    /**
     * @return 0-based page index in the document.
     */
    virtual int pageIndex() const = 0;

    /**
     * @return Page width in standard PDF points (1/72 inch).
     */
    virtual double widthPoints() const = 0;

    /**
     * @return Page height in standard PDF points (1/72 inch).
     */
    virtual double heightPoints() const = 0;

    /**
     * @brief Computes the target physical raster pixel dimensions of the page at the specified scale and DPR.
     */
    virtual void getPixelDimensions(double scale, double devicePixelRatio, int& outWidth, int& outHeight) const = 0;

    /**
     * @brief Renders a specific pixel rectangle (tile) of the page into an engine-agnostic PixelBuffer.
     * @param rect The sub-region rectangle in the page's pixel grid.
     * @param scale Zoom factor (1.0 = 72 DPI, 1.333 = 96 DPI standard desktop).
     * @param devicePixelRatio Device pixel ratio.
     * @return PixelBuffer containing the rendered tile, or invalid PixelBuffer on error.
     */
    virtual PixelBuffer renderTile(const PixelRect& rect, double scale, double devicePixelRatio) = 0;

    /**
     * @brief Renders the entire page into a QImage (convenience wrapper).
     */
    virtual QImage render(double scale, double devicePixelRatio) = 0;
};

} // namespace pdf
