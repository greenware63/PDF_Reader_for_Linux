#include "PdfiumDocument.h"
#include "PdfiumPage.h"
#include <mutex>
#include <cstring>

namespace pdf {

static std::once_flag s_initFlag;

void PdfiumDocument::ensureLibraryInitialized() {
    std::call_once(s_initFlag, []() {
        FPDF_LIBRARY_CONFIG config;
        config.version = 2;
        config.m_pUserFontPaths = nullptr;
        config.m_pIsolate = nullptr;
        config.m_v8EmbedderSlot = 0;
        FPDF_InitLibraryWithConfig(&config);
    });
}

PdfiumDocument::PdfiumDocument() {
    ensureLibraryInitialized();
}

PdfiumDocument::~PdfiumDocument() {
    close();
}

bool PdfiumDocument::loadFromFile(const std::string& filePath, const std::string& password) {
    close();
    const char* pwd = password.empty() ? nullptr : password.c_str();
    m_doc = FPDF_LoadDocument(filePath.c_str(), pwd);
    return m_doc != nullptr;
}

bool PdfiumDocument::loadFromMemory(const void* data, size_t size, const std::string& password) {
    close();
    if (!data || size == 0) {
        return false;
    }

    m_memoryBuffer.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    const char* pwd = password.empty() ? nullptr : password.c_str();
    m_doc = FPDF_LoadMemDocument64(m_memoryBuffer.data(), m_memoryBuffer.size(), pwd);
    if (!m_doc) {
        m_memoryBuffer.clear();
        return false;
    }
    return true;
}

int PdfiumDocument::pageCount() const {
    if (!m_doc) return 0;
    return FPDF_GetPageCount(m_doc);
}

std::unique_ptr<IPage> PdfiumDocument::getPage(int pageIndex) {
    if (!m_doc || pageIndex < 0 || pageIndex >= pageCount()) {
        return nullptr;
    }
    return std::make_unique<PdfiumPage>(m_doc, pageIndex);
}

void PdfiumDocument::close() {
    if (m_doc) {
        FPDF_CloseDocument(m_doc);
        m_doc = nullptr;
    }
    m_memoryBuffer.clear();
}

std::unique_ptr<IDocument> createDocument() {
    return std::make_unique<PdfiumDocument>();
}

} // namespace pdf
