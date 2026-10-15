#include "PdfiumDocument.h"
#include <fpdf_doc.h>
#include "PdfiumMutex.h"
#include <mutex>
#include <cstring>
#include <vector>

namespace pdf {

static std::once_flag s_initFlag;

void PdfiumDocument::ensureLibraryInitialized() {
    std::call_once(s_initFlag, []() {
        std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
        FPDF_LIBRARY_CONFIG config;
        config.version = 2;
        config.m_pUserFontPaths = nullptr;
        config.m_pIsolate = nullptr;
        config.m_v8EmbedderSlot = 0;
        FPDF_InitLibraryWithConfig(&config);
    });
}

DocumentError PdfiumDocument::mapPdfiumError(unsigned long fpdfError) {
    switch (fpdfError) {
        case FPDF_ERR_SUCCESS:
            return DocumentError::None;
        case FPDF_ERR_FILE:
            return DocumentError::FileNotFound;
        case FPDF_ERR_FORMAT:
            return DocumentError::InvalidFormat;
        case FPDF_ERR_PASSWORD:
            return DocumentError::PasswordRequired;
        default:
            return DocumentError::Unknown;
    }
}

PdfiumDocument::PdfiumDocument() {
    ensureLibraryInitialized();
}

PdfiumDocument::~PdfiumDocument() {
    close();
}

DocumentError PdfiumDocument::loadFromFile(const std::string& filePath, const std::string& password) {
    const char* pwd = password.empty() ? nullptr : password.c_str();

    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());

    // Load into temporary handle first so existing document is preserved on failure
    FPDF_DOCUMENT newDoc = FPDF_LoadDocument(filePath.c_str(), pwd);
    if (!newDoc) {
        unsigned long err = FPDF_GetLastError();
        if (err == FPDF_ERR_PASSWORD && !password.empty()) {
            m_lastError = DocumentError::BadPassword;
        } else {
            m_lastError = mapPdfiumError(err);
        }
        return m_lastError;
    }

    int count = FPDF_GetPageCount(newDoc);
    if (count <= 0) {
        FPDF_CloseDocument(newDoc);
        m_lastError = DocumentError::EmptyDocument;
        return m_lastError;
    }

    // Success: atomically swap active document handle
    m_doc = DocumentHandle(newDoc, [](FPDF_DOCUMENT d) {
        if (d) {
            std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
            FPDF_CloseDocument(d);
        }
    });
    m_lastError = DocumentError::None;
    return DocumentError::None;
}

DocumentError PdfiumDocument::loadFromMemory(const void* data, size_t size, const std::string& password) {
    if (!data || size == 0) {
        m_lastError = DocumentError::InvalidFormat;
        return m_lastError;
    }

    auto bufferCopy = std::make_shared<std::vector<uint8_t>>(
        static_cast<const uint8_t*>(data),
        static_cast<const uint8_t*>(data) + size
    );

    const char* pwd = password.empty() ? nullptr : password.c_str();

    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());

    FPDF_DOCUMENT newDoc = FPDF_LoadMemDocument64(bufferCopy->data(), bufferCopy->size(), pwd);
    if (!newDoc) {
        unsigned long err = FPDF_GetLastError();
        if (err == FPDF_ERR_PASSWORD && !password.empty()) {
            m_lastError = DocumentError::BadPassword;
        } else {
            m_lastError = mapPdfiumError(err);
        }
        return m_lastError;
    }

    int count = FPDF_GetPageCount(newDoc);
    if (count <= 0) {
        FPDF_CloseDocument(newDoc);
        m_lastError = DocumentError::EmptyDocument;
        return m_lastError;
    }

    // Tie memory buffer lifetime directly to DocumentHandle deleter.
    // Keeps memory valid as long as either PdfiumDocument or any PdfiumPage exists!
    m_doc = DocumentHandle(newDoc, [buf = std::move(bufferCopy)](FPDF_DOCUMENT d) {
        if (d) {
            std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
            FPDF_CloseDocument(d);
        }
        // buf is destroyed safely AFTER FPDF_CloseDocument completes
    });
    m_lastError = DocumentError::None;
    return DocumentError::None;
}

int PdfiumDocument::pageCount() const {
    if (!m_doc) return 0;
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    return FPDF_GetPageCount(m_doc.get());
}

std::pair<double, double> PdfiumDocument::pageSizePoints(int pageIndex) const {
    if (!m_doc || pageIndex < 0 || pageIndex >= pageCount()) {
        return {0.0, 0.0};
    }
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    FS_SIZEF size{0.0f, 0.0f};
    if (FPDF_GetPageSizeByIndexF(m_doc.get(), pageIndex, &size)) {
        return {static_cast<double>(size.width), static_cast<double>(size.height)};
    }
    return {0.0, 0.0};
}

std::unique_ptr<IPage> PdfiumDocument::getPage(int pageIndex) {
    if (!m_doc || pageIndex < 0 || pageIndex >= pageCount()) {
        return nullptr;
    }

    auto page = std::make_unique<PdfiumPage>(m_doc, pageIndex);
    if (!page->isValid()) {
        return nullptr;
    }
    return page;
}

static OutlineItem extractBookmarkRecursive(FPDF_DOCUMENT doc, FPDF_BOOKMARK bookmark) {
    OutlineItem item;

    // 1. Title
    unsigned long titleBytes = FPDFBookmark_GetTitle(bookmark, nullptr, 0);
    if (titleBytes > 2) {
        std::vector<char16_t> buf(titleBytes / 2);
        FPDFBookmark_GetTitle(bookmark, buf.data(), titleBytes);
        size_t charCount = (titleBytes / 2);
        if (charCount > 0 && buf[charCount - 1] == 0) {
            --charCount;
        }
        item.title = QString::fromUtf16(buf.data(), static_cast<qsizetype>(charCount));
    }

    // 2. Destination
    FPDF_DEST dest = FPDFBookmark_GetDest(doc, bookmark);
    if (!dest) {
        FPDF_ACTION action = FPDFBookmark_GetAction(bookmark);
        if (action) {
            dest = FPDFAction_GetDest(doc, action);
        }
    }

    if (dest) {
        item.pageIndex = FPDFDest_GetDestPageIndex(doc, dest);
        FPDF_BOOL hasX = 0, hasY = 0, hasZoom = 0;
        FS_FLOAT xVal = 0.0f, yVal = 0.0f, zoomVal = 0.0f;
        if (FPDFDest_GetLocationInPage(dest, &hasX, &hasY, &hasZoom, &xVal, &yVal, &zoomVal)) {
            if (hasY) {
                item.targetYPoints = static_cast<double>(yVal);
            }
        }
    }

    // 3. Children
    FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, bookmark);
    while (child) {
        item.children.push_back(extractBookmarkRecursive(doc, child));
        child = FPDFBookmark_GetNextSibling(doc, child);
    }

    return item;
}

std::vector<OutlineItem> PdfiumDocument::getOutline() const {
    if (!m_doc) {
        return {};
    }
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    std::vector<OutlineItem> outline;

    FPDF_BOOKMARK rootChild = FPDFBookmark_GetFirstChild(m_doc.get(), nullptr);
    while (rootChild) {
        outline.push_back(extractBookmarkRecursive(m_doc.get(), rootChild));
        rootChild = FPDFBookmark_GetNextSibling(m_doc.get(), rootChild);
    }

    return outline;
}

void PdfiumDocument::close() {
    m_doc.reset();
    m_lastError = DocumentError::None;
}

std::unique_ptr<IDocument> createDocument() {
    return std::make_unique<PdfiumDocument>();
}

} // namespace pdf
