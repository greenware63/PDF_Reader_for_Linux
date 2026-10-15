#pragma once

#include "engine/IDocument.h"
#include "PdfiumPage.h"
#include <fpdfview.h>
#include <utility>

namespace pdf {

class PdfiumDocument : public IDocument {
public:
    PdfiumDocument();
    ~PdfiumDocument() override;

    DocumentError loadFromFile(const std::string& filePath, const std::string& password = "") override;
    DocumentError loadFromMemory(const void* data, size_t size, const std::string& password = "") override;
    int pageCount() const override;
    std::pair<double, double> pageSizePoints(int pageIndex) const override;
    std::unique_ptr<IPage> getPage(int pageIndex) override;
    std::vector<OutlineItem> getOutline() const override;
    void close() override;
    bool isOpen() const override { return m_doc != nullptr; }
    DocumentError lastError() const override { return m_lastError; }

private:
    static void ensureLibraryInitialized();
    static DocumentError mapPdfiumError(unsigned long fpdfError);

    DocumentHandle m_doc{nullptr};
    DocumentError m_lastError{DocumentError::None};
};

} // namespace pdf
