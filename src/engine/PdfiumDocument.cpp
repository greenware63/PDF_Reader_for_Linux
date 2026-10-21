#include "PdfiumDocument.h"
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_transformpage.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <string>
#include "PdfiumMutex.h"
#include <mutex>
#include <cstring>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cmath>

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

    return loadFromBuffer(std::make_shared<const std::vector<uint8_t>>(
                              static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size),
                          password);
}

DocumentError PdfiumDocument::loadFromBuffer(std::shared_ptr<const std::vector<uint8_t>> bufferCopy,
                                             const std::string& password) {
    if (!bufferCopy || bufferCopy->empty()) {
        m_lastError = DocumentError::InvalidFormat;
        return m_lastError;
    }

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

namespace {

constexpr size_t MAX_OUTLINE_ITEMS = 10000;
constexpr size_t MAX_OUTLINE_DEPTH = 128;

struct OutlineExtractionState {
    std::unordered_set<FPDF_BOOKMARK> visited;
    struct PageGeometry {
        float left{0.0f};
        float bottom{0.0f};
        float right{0.0f};
        float top{0.0f};
        int rotation{0};
    };
    std::unordered_map<int, PageGeometry> pageGeometry;
    size_t itemCount{0};
};

static bool getDisplayedYFraction(FPDF_DOCUMENT doc, int pageIndex, bool hasX, bool hasY,
                                  float x, float y, OutlineExtractionState& state,
                                  double& fraction) {
    if (pageIndex < 0 || !hasY || !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }

    auto geometryIt = state.pageGeometry.find(pageIndex);
    if (geometryIt == state.pageGeometry.end()) {
        FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
        if (!page) {
            return false;
        }

        OutlineExtractionState::PageGeometry geometry;
        const bool hasCropBox = FPDFPage_GetCropBox(page, &geometry.left, &geometry.bottom,
                                                     &geometry.right, &geometry.top);
        if (!hasCropBox &&
            !FPDFPage_GetMediaBox(page, &geometry.left, &geometry.bottom, &geometry.right, &geometry.top)) {
            FPDF_ClosePage(page);
            return false;
        }
        geometry.rotation = FPDFPage_GetRotation(page);
        FPDF_ClosePage(page);
        geometryIt = state.pageGeometry.emplace(pageIndex, geometry).first;
    }

    const auto& geometry = geometryIt->second;
    const double width = static_cast<double>(geometry.right) - geometry.left;
    const double height = static_cast<double>(geometry.top) - geometry.bottom;
    if (!(width > 0.0) || !(height > 0.0)) {
        return false;
    }

    double position = 0.0;
    double extent = height;
    switch ((geometry.rotation % 4 + 4) % 4) {
        case 1:
            position = (hasX ? static_cast<double>(x) : geometry.left) - geometry.left;
            extent = width;
            break;
        case 2:
            position = static_cast<double>(y) - geometry.bottom;
            break;
        case 3:
            position = geometry.right - (hasX ? static_cast<double>(x) : geometry.right);
            extent = width;
            break;
        default:
            position = geometry.top - static_cast<double>(y);
            break;
    }

    fraction = std::clamp(position / extent, 0.0, 1.0);
    return true;
}

static OutlineItem extractBookmarkRecursive(FPDF_DOCUMENT doc, FPDF_BOOKMARK bookmark,
                                            size_t depth, OutlineExtractionState& state) {
    OutlineItem item;
    if (!bookmark || depth >= MAX_OUTLINE_DEPTH || state.itemCount >= MAX_OUTLINE_ITEMS ||
        !state.visited.insert(bookmark).second) {
        return item;
    }
    ++state.itemCount;

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
                item.hasTargetY = getDisplayedYFraction(doc, item.pageIndex, hasX != 0,
                                                        hasY != 0, xVal, yVal, state,
                                                        item.targetYFraction);
            }
        }
    }

    FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, bookmark);
    while (child && state.itemCount < MAX_OUTLINE_ITEMS) {
        if (state.visited.find(child) != state.visited.end() || depth + 1 >= MAX_OUTLINE_DEPTH) {
            break;
        }
        item.children.push_back(extractBookmarkRecursive(doc, child, depth + 1, state));
        child = FPDFBookmark_GetNextSibling(doc, child);
    }

    return item;
}

} // namespace

std::vector<OutlineItem> PdfiumDocument::getOutline() const {
    if (!m_doc) {
        return {};
    }
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    std::vector<OutlineItem> outline;
    OutlineExtractionState state;

    FPDF_BOOKMARK rootChild = FPDFBookmark_GetFirstChild(m_doc.get(), nullptr);
    while (rootChild && state.itemCount < MAX_OUTLINE_ITEMS) {
        if (state.visited.find(rootChild) != state.visited.end()) {
            break;
        }
        outline.push_back(extractBookmarkRecursive(m_doc.get(), rootChild, 0, state));
        rootChild = FPDFBookmark_GetNextSibling(m_doc.get(), rootChild);
    }

    return outline;
}

std::string PdfiumDocument::getPageLabel(int pageIndex) const {
    if (!m_doc || pageIndex < 0 || pageIndex >= pageCount()) {
        return "";
    }
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    unsigned long length = FPDF_GetPageLabel(m_doc.get(), pageIndex, nullptr, 0);
    if (length > 2) {
        std::vector<char16_t> buf(length / 2);
        FPDF_GetPageLabel(m_doc.get(), pageIndex, buf.data(), length);
        size_t charCount = length / 2;
        if (charCount > 0 && buf[charCount - 1] == 0) {
            --charCount;
        }
        QString qLabel = QString::fromUtf16(buf.data(), static_cast<qsizetype>(charCount));
        if (!qLabel.isEmpty()) {
            return qLabel.toStdString();
        }
    }
    return std::to_string(pageIndex + 1);
}

std::vector<std::string> PdfiumDocument::getPageLabels() const {
    if (!m_doc) {
        return {};
    }
    const int count = pageCount();
    std::vector<std::string> labels;
    labels.reserve(count);
    for (int i = 0; i < count; ++i) {
        labels.push_back(getPageLabel(i));
    }
    return labels;
}

PixelBuffer PdfiumDocument::renderThumbnail(int pageIndex, int targetWidth) {
    if (!m_doc || pageIndex < 0 || pageIndex >= pageCount()) {
        return {};
    }
    auto page = getPage(pageIndex);
    if (!page) {
        return {};
    }
    return page->renderThumbnail(targetWidth);
}

void PdfiumDocument::close() {
    m_doc.reset();
    m_lastError = DocumentError::None;
}

namespace {
struct MemoryWriter : FPDF_FILEWRITE {
    std::vector<uint8_t> data;
    bool failed{false};
    static constexpr size_t kMaxBytes = 512u * 1024u * 1024u;
    MemoryWriter() {
        version = 1;
        WriteBlock = &MemoryWriter::write;
    }
    static int write(FPDF_FILEWRITE* self, const void* buf, unsigned long size) {
        auto* w = static_cast<MemoryWriter*>(self);
        if (w->failed || w->data.size() + size > kMaxBytes) {
            w->failed = true;
            return 0;
        }
        const auto* p = static_cast<const uint8_t*>(buf);
        w->data.insert(w->data.end(), p, p + size);
        return 1;
    }
};
} // namespace

bool PdfiumDocument::extractPages(int firstPage, int lastPage, std::vector<uint8_t>& out) const {
    out.clear();
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    if (!m_doc) {
        return false;
    }
    const int total = FPDF_GetPageCount(m_doc.get());
    if (firstPage < 0 || lastPage < firstPage || lastPage >= total) {
        return false;
    }
    FPDF_DOCUMENT dest = FPDF_CreateNewDocument();
    if (!dest) {
        return false;
    }
    const std::string range = std::to_string(firstPage + 1) + "-" + std::to_string(lastPage + 1);
    bool ok = FPDF_ImportPages(dest, m_doc.get(), range.c_str(), 0) != 0;
    MemoryWriter writer;
    if (ok) {
        ok = FPDF_SaveAsCopy(dest, &writer, 0) != 0 && !writer.failed && !writer.data.empty();
    }
    FPDF_CloseDocument(dest);
    if (ok) {
        out = std::move(writer.data);
    }
    return ok;
}

std::unique_ptr<IDocument> createDocument() {
    return std::make_unique<PdfiumDocument>();
}

} // namespace pdf
