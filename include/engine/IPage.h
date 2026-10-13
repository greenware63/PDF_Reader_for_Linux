#pragma once

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
     * @brief Renders the page into a QImage.
     * @param scale Zoom factor (1.0 = 100% at 72 DPI, 2.0 = 200%, etc.).
     * @param devicePixelRatio Screen device pixel ratio for crisp HiDPI rendering.
     * @return QImage containing rendered pixels with devicePixelRatio configured.
     */
    virtual QImage render(double scale, double devicePixelRatio) = 0;
};

} // namespace pdf
