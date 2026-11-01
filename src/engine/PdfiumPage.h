#pragma once

#include "engine/IPage.h"
#include <fpdfview.h>
#include <fpdf_formfill.h>
#include <memory>
#include <type_traits>
#include <atomic>

namespace pdf {

using DocumentHandle = std::shared_ptr<std::remove_pointer_t<FPDF_DOCUMENT>>;

// Form-fill environment, used ONLY so PDFium draws AcroForm widget appearances (field values, check marks).
// PDFium skips widgets in FPDF_RenderPageBitmap and draws them in FPDF_FFLDraw. Nothing executes: no JavaScript
// platform (m_pJsPlatform stays null; the shipped PDFium has no V8 anyway), no XFA, no submit/URI/launch
// callbacks, no mouse/keyboard input is ever forwarded, and the field highlight tint is disabled.
// `info` must outlive `handle` (PDFium keeps a pointer to it), hence one shared struct.
struct FormEnv {
    FPDF_FORMFILLINFO info{};
    FPDF_FORMHANDLE handle{nullptr};
};

class PdfiumPage : public IPage {
public:
    PdfiumPage(DocumentHandle doc, int pageIndex, std::shared_ptr<FormEnv> form = nullptr);
    ~PdfiumPage() override;

    bool isValid() const { return m_page != nullptr; }
    int pageIndex() const override { return m_pageIndex; }
    double widthPoints() const override { return m_width; }
    double heightPoints() const override { return m_height; }

    void getPixelDimensions(double scale, double devicePixelRatio, int& outWidth, int& outHeight) const override;
    PixelBuffer renderTile(const PixelRect& rect, double scale, double devicePixelRatio, int rotation = 0) override;
    PixelBuffer renderWholePage(double scale, double devicePixelRatio, int rotation = 0) override;
    PixelBuffer renderThumbnail(int targetWidth, int rotation = 0) override;
    PageText extractText() override;
    std::vector<AnnotationItem> listAnnotations(bool* truncated = nullptr) override;
    bool hitTestAnnotation(double xPoints, double yPoints, AnnotationItem& out) override;
    bool addMarkupAnnotation(const AnnotationItem& item) override;
    bool addHighlightAnnotation(const AnnotationItem& item) override;
    bool addTextAnnotation(const AnnotationItem& item) override;
    bool addGeometricAnnotation(const AnnotationItem& item, double borderWidth = 2.0) override;
    bool addInkAnnotation(const AnnotationItem& item, double borderWidth = 2.0) override;
    bool addFreeTextAnnotation(const AnnotationItem& item, double fontSize = 12.0) override;
    int removeAnnotationByName(const QString& name) override;

    static void setSimulateGetTextFailureForTesting(bool simulate) noexcept;

private:
    void drawFormFields(FPDF_BITMAP bitmap, int startX, int startY, int sizeX, int sizeY, int rotate, int flags);

    DocumentHandle m_doc;
    std::shared_ptr<FormEnv> m_form;
    FPDF_PAGE m_page{nullptr};
    int m_pageIndex{0};
    double m_width{0.0};
    double m_height{0.0};
};

} // namespace pdf
