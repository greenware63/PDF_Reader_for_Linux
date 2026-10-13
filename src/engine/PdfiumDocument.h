#pragma once

#include "engine/IDocument.h"
#include <fpdfview.h>
#include <vector>

namespace pdf {

class PdfiumDocument : public IDocument {
public:
    PdfiumDocument();
    ~PdfiumDocument() override;

    bool loadFromFile(const std::string& filePath, const std::string& password = "") override;
    bool loadFromMemory(const void* data, size_t size, const std::string& password = "") override;
    int pageCount() const override;
    std::unique_ptr<IPage> getPage(int pageIndex) override;
    void close() override;
    bool isOpen() const override { return m_doc != nullptr; }

private:
    static void ensureLibraryInitialized();

    FPDF_DOCUMENT m_doc{nullptr};
    std::vector<uint8_t> m_memoryBuffer; // Keeps in-memory PDF data valid while document is open
};

} // namespace pdf
