#pragma once

#include "PixelBuffer.h"
#include "PageText.h"
#include "AnnotationTypes.h"
#include <vector>

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

    /**
     * @brief Lists all annotations present on this page as bounded POD data.
     * If `truncated` is non-null it is set to true when anything was dropped or clipped (more annotations than
     * the cap, the response budget reached, or a string over its limit), so callers can show the list as incomplete.
     * Must be called only from the worker thread.
     */
    virtual std::vector<AnnotationItem> listAnnotations(bool* truncated = nullptr) = 0;

    /**
     * @brief Hit-tests annotations on this page at the given point (in standard PDF user space points).
     * Evaluates in reverse z-order (topmost annotation first).
     * @param xPoints X coordinate in PDF points.
     * @param yPoints Y coordinate in PDF points.
     * @param out Receives the hit AnnotationItem if hit.
     * @return true if an annotation was hit, false otherwise.
     */
    virtual bool hitTestAnnotation(double xPoints, double yPoints, AnnotationItem& out) = 0;

    /**
     * @brief Creates a text markup annotation (Highlight, Underline, StrikeOut, Squiggly) on this page.
     * Generates standard QuadPoints, color (/C), flags, /NM identifier, and normal appearance stream (/AP).
     * @param item Annotation properties (subtype, bounding box, quad points, color, /NM identifier, etc.).
     * @return true if successfully created, false otherwise.
     */
    virtual bool addMarkupAnnotation(const AnnotationItem& item) {
        (void)item;
        return false;
    }

    /**
     * @brief Creates a highlight annotation on this page (convenience wrapper around addMarkupAnnotation).
     * @param item Annotation properties.
     * @return true if successfully created, false otherwise.
     */
    virtual bool addHighlightAnnotation(const AnnotationItem& item) {
        AnnotationItem copy = item;
        copy.subtype = AnnotationSubtype::Highlight;
        return addMarkupAnnotation(copy);
    }

    /**
     * @brief Removes every annotation on this page whose /NM equals `name` (undo of an added annotation).
     * @return the number removed (0 if none matched or the name is empty).
     */
    virtual int removeAnnotationByName(const QString& name) {
        (void)name;
        return 0;
    }
};

} // namespace pdf
