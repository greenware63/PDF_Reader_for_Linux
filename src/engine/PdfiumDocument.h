#pragma once

#include "engine/IDocument.h"
#include "PdfiumPage.h"
#include <fpdfview.h>
#include <vector>

namespace pdf {

class PdfiumDocument : public IDocument {
public:
    PdfiumDocument();
    ~PdfiumDocument() override;

    DocumentError loadFromFile(const std::string& filePath, const std::string& password = "") override;
    DocumentError loadFromMemory(const void* data, size_t size, const std::string& password = "") override;
    int pageCount() const override;
    std::unique_ptr<IPage> getPage(int pageIndex) override;
    void close() override;
    bool isOpen() const override { return m_doc != nullptr; }
    DocumentError lastError() const override { return m_lastError; }

private:
    static void ensureLibraryInitialized();
    static DocumentError mapPdfiumError(unsigned long fpdfError);

    DocumentHandle m_doc{nullptr};
    std::shared_ptr<std::vector<uint8_t>> m_memoryBuffer; // Keeps memory alive while doc is open
    DocumentError m_lastError{DocumentError::None};
};

} // namespace pdf
