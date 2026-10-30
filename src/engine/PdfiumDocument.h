#pragma once

#include "engine/IDocument.h"
#include "PdfiumPage.h"
#include <fpdfview.h>
#include <utility>
#include <string>
#include <vector>
#include <atomic>
#include <cstddef>
#include <functional>

namespace pdf {

class PdfiumDocument : public IDocument {
public:
    PdfiumDocument();
    ~PdfiumDocument() override;

    DocumentError loadFromFile(const std::string& filePath, const std::string& password = "") override;
    DocumentError loadFromMemory(const void* data, size_t size, const std::string& password = "") override;
    DocumentError loadFromBuffer(std::shared_ptr<const std::vector<uint8_t>> buffer,
                                 const std::string& password = "") override;
    DocumentError loadFromDescriptor(int fd, const std::string& password = "", bool takeOwnership = true) override;

    // Test hook for unit testing the pread-backed streaming path on small fixtures
    static void setDescriptorStreamThresholdForTesting(size_t bytes);
    static void resetDescriptorStreamThresholdForTesting();

    int pageCount() const override;
    std::pair<double, double> pageSizePoints(int pageIndex) const override;
    std::unique_ptr<IPage> getPage(int pageIndex) override;
    std::vector<OutlineItem> getOutline() const override;
    std::string getPageLabel(int pageIndex) const override;
    std::vector<std::string> getPageLabels() const override;
    PixelBuffer renderThumbnail(int pageIndex, int targetWidth) override;
    bool extractPages(int firstPage, int lastPage, std::vector<uint8_t>& out) const override;
    bool saveAsCopy(int writeFd, bool incremental, uint64_t& outBytesWritten,
                    std::function<void(uint64_t)> progressCallback = nullptr) override;
    int signatureCount() const override;
    void close() override;
    bool isOpen() const override { return m_doc != nullptr; }
    DocumentError lastError() const override { return m_lastError; }

private:
    static void ensureLibraryInitialized();
    static DocumentError mapPdfiumError(unsigned long fpdfError);

    DocumentHandle m_doc{nullptr};
    std::shared_ptr<FormEnv> m_form;
    DocumentError m_lastError{DocumentError::None};
};

} // namespace pdf
