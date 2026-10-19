#pragma once

#include "PixelBuffer.h"
#include "PageText.h"

namespace pdf {

/**
 * @brief Pure C++ abstract interface representing a single PDF page.
 * Completely decoupled from Qt or any UI framework.
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
     * @brief Computes unconstrained true raster pixel dimensions of the page at the given scale and DPR.
     */
    virtual void getPixelDimensions(double scale, double devicePixelRatio, int& outWidth, int& outHeight) const = 0;

    /**
     * @brief Renders a specific pixel rectangle (tile) of the page into an engine-agnostic PixelBuffer.
     * Tile dimensions are capped at safe memory bounds (e.g. 4096 x 4096 max tile size).
     * @param rect The sub-region rectangle in the page's virtual pixel grid.
     * @param scale Zoom factor (1.0 = 72 DPI, 1.333 = 96 DPI standard desktop).
     * @param devicePixelRatio Screen device pixel ratio.
     * @param rotation Clockwise rotation in degrees (0, 90, 180, 270).
     * @return PixelBuffer containing the rendered tile.
     */
    virtual PixelBuffer renderTile(const PixelRect& rect, double scale, double devicePixelRatio, int rotation = 0) = 0;

    /**
     * @brief Renders the entire page into a PixelBuffer, with memory caps for legacy whole-page viewing.
     * @param rotation Clockwise rotation in degrees (0, 90, 180, 270).
     */
    virtual PixelBuffer renderWholePage(double scale, double devicePixelRatio, int rotation = 0) = 0;

    /**
     * @brief Renders a miniature thumbnail of the page with the given target width,
     * maintaining aspect ratio.
     * @param targetWidth Desired thumbnail width in pixels.
     * @return PixelBuffer containing the rendered thumbnail.
     */
    virtual PixelBuffer renderThumbnail(int targetWidth, int rotation = 0) = 0;

    /**
     * @brief Extracts text and character bounding boxes for the page.
     * Must be called only from the worker thread.
     */
    virtual PageText extractText() = 0;
};

} // namespace pdf
