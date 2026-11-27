#include "PdfiumDocument.h"
#include <fpdf_doc.h>
#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_transformpage.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_signature.h>
#include <fpdf_flatten.h>
#include <string>
#include "PdfiumMutex.h"
#include <mutex>
#include <cstring>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <cerrno>
#include <fstream>
#include <queue>

#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

namespace pdf {

static std::once_flag s_initFlag;
static constexpr size_t DEFAULT_DESCRIPTOR_STREAM_THRESHOLD = 512ULL * 1024ULL * 1024ULL; // 512 MiB
static std::atomic<size_t> s_streamThreshold{DEFAULT_DESCRIPTOR_STREAM_THRESHOLD};

void PdfiumDocument::setDescriptorStreamThresholdForTesting(size_t bytes) {
    s_streamThreshold.store(bytes);
}

void PdfiumDocument::resetDescriptorStreamThresholdForTesting() {
    s_streamThreshold.store(DEFAULT_DESCRIPTOR_STREAM_THRESHOLD);
}

namespace {

struct PreadDocumentSource {
    FPDF_FILEACCESS fileAccess{};
    int fd{-1};
    uint64_t fileSize{0};
    bool ownsFd{true};

    PreadDocumentSource(int inFd, uint64_t inSize, bool inOwnsFd)
        : fd(inFd), fileSize(inSize), ownsFd(inOwnsFd) {
        fileAccess.m_FileLen = static_cast<unsigned long>(fileSize);
        fileAccess.m_GetBlock = &PreadDocumentSource::getBlock;
        fileAccess.m_Param = this;
    }

    ~PreadDocumentSource() {
        if (ownsFd && fd >= 0) {
#ifdef _WIN32
            ::_close(fd);
#else
            ::close(fd);
#endif
            fd = -1;
        }
    }

    PreadDocumentSource(const PreadDocumentSource&) = delete;
    PreadDocumentSource& operator=(const PreadDocumentSource&) = delete;

    static int getBlock(void* param, unsigned long pos, unsigned char* pBuf, unsigned long size) {
        if (!param || !pBuf || size == 0) {
            return 0;
        }
        auto* self = static_cast<PreadDocumentSource*>(param);
        const uint64_t uPos = static_cast<uint64_t>(pos);
        const uint64_t uSize = static_cast<uint64_t>(size);

        // Offset overflow checks:
        if (uPos > self->fileSize || uSize > self->fileSize || uPos + uSize > self->fileSize) {
            return 0;
        }

        size_t totalRead = 0;
        while (totalRead < size) {
#ifdef _WIN32
            static std::mutex s_winSeekMutex;
            std::lock_guard<std::mutex> lk(s_winSeekMutex);
            if (::_lseeki64(self->fd, static_cast<__int64>(uPos + totalRead), SEEK_SET) == -1) {
                return 0;
            }
            int bytes = ::_read(self->fd, pBuf + totalRead, static_cast<unsigned int>(size - totalRead));
            if (bytes <= 0) {
                return 0;
            }
            totalRead += static_cast<size_t>(bytes);
#else
            const off_t targetOffset = static_cast<off_t>(uPos + totalRead);
            ssize_t bytes = ::pread(self->fd, pBuf + totalRead, size - totalRead, targetOffset);
            if (bytes < 0) {
                if (errno == EINTR) {
                    continue; // Interrupted by signal -> retry
                }
                return 0; // I/O or pread error
            }
            if (bytes == 0) {
                // Unexpected EOF (file truncated concurrently on disk)
                return 0;
            }
            totalRead += static_cast<size_t>(bytes);
#endif
        }
        return (totalRead == size) ? 1 : 0;
    }
};

} // namespace

namespace {
// Creates the render-only form-fill environment for a freshly opened document (caller holds the mutex).
std::shared_ptr<FormEnv> makeFormEnv(FPDF_DOCUMENT doc) {
    auto env = std::make_shared<FormEnv>();
    env->info.version = 1; // no callbacks installed, in particular no JS platform / submit / URI handlers
    env->handle = FPDFDOC_InitFormFillEnvironment(doc, &env->info);
    if (env->handle) {
        FPDF_SetFormFieldHighlightAlpha(env->handle, 0); // no blue field tint
    }
    return env;
}
} // namespace

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

    // Retain immutable source bytes for scratch-based page organization (never re-open from path later).
    std::shared_ptr<const std::vector<uint8_t>> retainedBuffer;
    bool isStreamed = false;
    {
        std::ifstream ifs(filePath, std::ios::binary | std::ios::ate);
        if (ifs.good()) {
            const auto sz = static_cast<size_t>(ifs.tellg());
            if (sz > s_streamThreshold.load()) {
                isStreamed = true;
            } else if (sz > 0) {
                auto buf = std::make_shared<std::vector<uint8_t>>(sz);
                ifs.seekg(0, std::ios::beg);
                if (ifs.read(reinterpret_cast<char*>(buf->data()), static_cast<std::streamsize>(sz))) {
                    retainedBuffer = std::move(buf);
                }
            }
        }
    }

    // Success: atomically swap active document handle
    auto env = makeFormEnv(newDoc);
    m_doc = DocumentHandle(newDoc, [env](FPDF_DOCUMENT d) {
        if (d) {
            std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
            // The form-fill environment must be torn down BEFORE its document is closed.
            if (env && env->handle) { FPDFDOC_ExitFormFillEnvironment(env->handle); env->handle = nullptr; }
            FPDF_CloseDocument(d);
        }
    });
    m_form = env;
    m_sourceBuffer = std::move(retainedBuffer);
    m_sourcePassword = password;
    m_isStreamedSource = isStreamed;
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
    auto env = makeFormEnv(newDoc);
    m_sourceBuffer = bufferCopy;
    m_sourcePassword = password;
    m_isStreamedSource = false;
    m_doc = DocumentHandle(newDoc, [buf = std::move(bufferCopy), env](FPDF_DOCUMENT d) {
        if (d) {
            std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
            if (env && env->handle) { FPDFDOC_ExitFormFillEnvironment(env->handle); env->handle = nullptr; }
            FPDF_CloseDocument(d);
        }
        // buf is destroyed safely AFTER FPDF_CloseDocument completes
    });
    m_form = env;
    m_lastError = DocumentError::None;
    return DocumentError::None;
}

DocumentError PdfiumDocument::loadFromDescriptor(int fd, const std::string& password, bool takeOwnership) {
    if (fd < 0) {
        m_lastError = DocumentError::FileNotFound;
        return m_lastError;
    }

    uint64_t fileSize = 0;
#ifdef _WIN32
    struct _stat64 st{};
    if (::_fstat64(fd, &st) != 0) {
        if (takeOwnership) ::_close(fd);
        m_lastError = DocumentError::FileNotFound;
        return m_lastError;
    }
    if ((st.st_mode & _S_IFREG) == 0 || st.st_size < 4) {
        if (takeOwnership) ::_close(fd);
        m_lastError = DocumentError::InvalidFormat;
        return m_lastError;
    }
    fileSize = static_cast<uint64_t>(st.st_size);
#else
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        if (takeOwnership) ::close(fd);
        m_lastError = DocumentError::FileNotFound;
        return m_lastError;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 4) {
        if (takeOwnership) ::close(fd);
        m_lastError = DocumentError::InvalidFormat;
        return m_lastError;
    }
    fileSize = static_cast<uint64_t>(st.st_size);
#endif

    const size_t threshold = s_streamThreshold.load();

    // Strategy 1: Heap Vector Pinning (fileSize <= threshold)
    // Copies file bytes into RAM to guarantee true immutability (immune to SIGBUS on truncation
    // and external tampering) and closes fd immediately.
    if (fileSize <= threshold) {
        auto buffer = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(fileSize));
        size_t totalRead = 0;
        while (totalRead < static_cast<size_t>(fileSize)) {
#ifdef _WIN32
            int bytes = ::_read(fd, buffer->data() + totalRead, static_cast<unsigned int>(fileSize - totalRead));
            if (bytes <= 0) break;
            totalRead += static_cast<size_t>(bytes);
#else
            ssize_t bytes = ::pread(fd, buffer->data() + totalRead, static_cast<size_t>(fileSize) - totalRead, static_cast<off_t>(totalRead));
            if (bytes < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (bytes == 0) break;
            totalRead += static_cast<size_t>(bytes);
#endif
        }

        if (takeOwnership) {
#ifdef _WIN32
            ::_close(fd);
#else
            ::close(fd);
#endif
        }

        if (totalRead != static_cast<size_t>(fileSize)) {
            m_lastError = DocumentError::InvalidFormat;
            return m_lastError;
        }

        return loadFromBuffer(std::move(buffer), password);
    }

    // Strategy 2: FPDF_FILEACCESS with pread (fileSize > threshold)
    auto source = std::make_shared<PreadDocumentSource>(fd, fileSize, takeOwnership);

    const char* pwd = password.empty() ? nullptr : password.c_str();
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());

    FPDF_DOCUMENT newDoc = FPDF_LoadCustomDocument(&source->fileAccess, pwd);
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

    // Tie PreadDocumentSource lifetime (owns fd and FPDF_FILEACCESS) directly to DocumentHandle deleter.
    m_doc = DocumentHandle(newDoc, [src = std::move(source)](FPDF_DOCUMENT d) {
        if (d) {
            std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
            FPDF_CloseDocument(d);
        }
    });
    m_sourceBuffer.reset();
    m_sourcePassword = password;
    m_isStreamedSource = true;
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

    auto page = std::make_unique<PdfiumPage>(m_doc, pageIndex, m_form);
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
    m_form.reset();
    m_doc.reset();
    m_sourceBuffer.reset();
    m_sourcePassword.clear();
    m_isStreamedSource = false;
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

bool PdfiumDocument::extractPages(int firstPage, int lastPage, std::vector<uint8_t>& out,
                                  bool flatten, FlattenMode mode) const {
    out.clear();
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    if (!m_doc) {
        return false;
    }
    if (m_form && m_form->handle) {
        FORM_ForceToKillFocus(m_form->handle);
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
    if (ok && flatten) {
        const int importedCount = FPDF_GetPageCount(dest);
        const int flag = (mode == FlattenMode::Print) ? FLAT_PRINT : FLAT_NORMALDISPLAY;
        for (int i = 0; i < importedCount; ++i) {
            FPDF_PAGE p = FPDF_LoadPage(dest, i);
            if (!p) {
                ok = false;
                break;
            }
            const int flattenRes = FPDFPage_Flatten(p, flag);
            FPDF_ClosePage(p);
            if (flattenRes == FLATTEN_FAIL) {
                ok = false;
                break;
            }
        }
    }
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

FlattenResult PdfiumDocument::flatten(FlattenMode mode) {
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    if (!m_doc) {
        return FlattenResult::Failed;
    }
    if (m_form && m_form->handle) {
        FORM_ForceToKillFocus(m_form->handle);
    }
    const int count = FPDF_GetPageCount(m_doc.get());
    if (count <= 0) {
        return FlattenResult::NothingToDo;
    }
    bool anyFlattened = false;
    bool anyFailed = false;
    const int flag = (mode == FlattenMode::Print) ? FLAT_PRINT : FLAT_NORMALDISPLAY;
    for (int i = 0; i < count; ++i) {
        FPDF_PAGE p = FPDF_LoadPage(m_doc.get(), i);
        if (!p) {
            anyFailed = true;
            continue;
        }
        const int res = FPDFPage_Flatten(p, flag);
        if (res == FLATTEN_SUCCESS) {
            anyFlattened = true;
        } else if (res == FLATTEN_FAIL) {
            anyFailed = true;
        }
        FPDF_ClosePage(p);
    }
    if (anyFailed) {
        return FlattenResult::Failed;
    }
    return anyFlattened ? FlattenResult::Success : FlattenResult::NothingToDo;
}

namespace {

struct FdWriter : public FPDF_FILEWRITE {
    int fd{-1};
    uint64_t bytesWritten{0};
    bool failed{false};
    std::function<void(uint64_t)> progressCallback;

    FdWriter(int inFd, std::function<void(uint64_t)> progress)
        : fd(inFd), progressCallback(std::move(progress)) {
        version = 1;
        WriteBlock = &FdWriter::write;
    }

    static int write(FPDF_FILEWRITE* self, const void* buf, unsigned long size) {
        auto* w = static_cast<FdWriter*>(self);
        if (w->failed || w->fd < 0 || !buf || size == 0) {
            return (w->failed || w->fd < 0) ? 0 : 1;
        }
        const char* p = static_cast<const char*>(buf);
        unsigned long remaining = size;
        while (remaining > 0) {
#ifdef _WIN32
            int n = ::_write(w->fd, p, remaining);
#else
            ssize_t n = ::write(w->fd, p, remaining);
#endif
            if (n < 0) {
                if (errno == EINTR) continue;
                w->failed = true;
                return 0;
            }
            p += n;
            remaining -= static_cast<unsigned long>(n);
            w->bytesWritten += static_cast<uint64_t>(n);
        }
        if (w->progressCallback) {
            w->progressCallback(w->bytesWritten);
        }
        return 1;
    }
};

} // namespace

bool PdfiumDocument::saveAsCopy(int writeFd, bool incremental, uint64_t& outBytesWritten,
                                std::function<void(uint64_t)> progressCallback) {
    outBytesWritten = 0;
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    if (!m_doc || writeFd < 0) {
        return false;
    }

    int sigs = FPDF_GetSignatureCount(m_doc.get());
    if (sigs > 0 && !incremental) {
        return false;
    }

    FdWriter writer(writeFd, std::move(progressCallback));
    unsigned long flags = incremental ? FPDF_INCREMENTAL : FPDF_NO_INCREMENTAL;
    int res = FPDF_SaveAsCopy(m_doc.get(), &writer, flags);
    if (res == 0 || writer.failed) {
        return false;
    }
    outBytesWritten = writer.bytesWritten;
    return outBytesWritten > 0;
}

int PdfiumDocument::signatureCount() const {
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    if (!m_doc) return 0;
    return FPDF_GetSignatureCount(m_doc.get());
}

OrganizeResult PdfiumDocument::organizePages(const std::vector<PageSpec>& pages,
                                             int writeFd,
                                             const OrganizeOptions& options,
                                             std::function<void(uint64_t)> progressCallback) const {
    OrganizeResult result;
    std::lock_guard<std::recursive_mutex> lock(getPdfiumMutex());
    if (!m_doc) {
        result.status = OrganizeStatus::InternalError;
        return result;
    }

    // 1. Validate spec and fd before checking guards or touching writeFd.
    const int srcCount = FPDF_GetPageCount(m_doc.get());
    if (writeFd < 0 || srcCount <= 0 || pages.empty() ||
        pages.size() > kMaxOrganizedPages || pages.size() > static_cast<size_t>(srcCount)) {
        result.status = OrganizeStatus::InvalidSpec;
        return result;
    }

    std::vector<bool> keep(static_cast<size_t>(srcCount), false);
    for (const auto& spec : pages) {
        if (spec.sourceIndex < 0 || spec.sourceIndex >= srcCount) {
            result.status = OrganizeStatus::InvalidSpec;
            return result;
        }
        if (keep[static_cast<size_t>(spec.sourceIndex)]) {
            result.status = OrganizeStatus::InvalidSpec;
            return result;
        }
        if (spec.rotationDegrees != 0 && spec.rotationDegrees != 90 &&
            spec.rotationDegrees != 180 && spec.rotationDegrees != 270) {
            result.status = OrganizeStatus::InvalidSpec;
            return result;
        }
        keep[static_cast<size_t>(spec.sourceIndex)] = true;
    }

    // 2. Guards in order: permissions -> signatures -> encrypted -> streamed source.
    const int secRev = FPDF_GetSecurityHandlerRevision(m_doc.get());
    if (secRev != -1) {
        const unsigned long perms = FPDF_GetDocPermissions(m_doc.get());
        // ISO 32000-1 Table 22 (1-based bits): R3+ requires assemble (bit 11 = 1<<10); R2 requires modify (bit 4 = 1<<3).
        const unsigned long requiredBit = (secRev >= 3) ? (1UL << 10) : (1UL << 3);
        if ((perms & requiredBit) == 0) {
            result.status = OrganizeStatus::PermissionDenied;
            return result;
        }
    }

    if (FPDF_GetSignatureCount(m_doc.get()) > 0 && !options.allowInvalidateSignatures) {
        result.status = OrganizeStatus::SignedNotAcknowledged;
        return result;
    }

    if (secRev != -1 && !options.allowDecrypt) {
        result.status = OrganizeStatus::EncryptedNotAcknowledged;
        return result;
    }

    if (m_isStreamedSource || !m_sourceBuffer || m_sourceBuffer->empty()) {
        result.status = OrganizeStatus::StreamedSourceUnsupported;
        return result;
    }

    // 3. Open scratch document from retained immutable source bytes and password (never from a path).
    const char* pwd = m_sourcePassword.empty() ? nullptr : m_sourcePassword.c_str();
    FPDF_DOCUMENT scratchDoc = FPDF_LoadMemDocument64(m_sourceBuffer->data(), m_sourceBuffer->size(), pwd);
    if (!scratchDoc) {
        result.status = OrganizeStatus::InternalError;
        return result;
    }
    struct ScratchGuard {
        FPDF_DOCUMENT doc;
        ~ScratchGuard() {
            if (doc) FPDF_CloseDocument(doc);
        }
    } scratchGuard{scratchDoc};

    if (FPDF_GetPageCount(scratchDoc) != srcCount) {
        result.status = OrganizeStatus::InternalError;
        return result;
    }

    // 4a. Delete unlisted pages from highest index to lowest so remaining indices stay valid.
    for (int i = srcCount - 1; i >= 0; --i) {
        if (!keep[static_cast<size_t>(i)]) {
            FPDFPage_Delete(scratchDoc, i);
        }
    }

    const int keptCount = static_cast<int>(pages.size());
    if (FPDF_GetPageCount(scratchDoc) != keptCount) {
        result.status = OrganizeStatus::InternalError;
        return result;
    }

    // 4b. Map kept source indices to post-delete indices and reorder if not already ascending.
    std::vector<int> postDeleteIndex(static_cast<size_t>(srcCount), -1);
    int nextIdx = 0;
    for (int i = 0; i < srcCount; ++i) {
        if (keep[static_cast<size_t>(i)]) {
            postDeleteIndex[static_cast<size_t>(i)] = nextIdx++;
        }
    }

    std::vector<int> targetOrder;
    targetOrder.reserve(pages.size());
    bool needsMove = false;
    for (int i = 0; i < keptCount; ++i) {
        const int mapped = postDeleteIndex[static_cast<size_t>(pages[static_cast<size_t>(i)].sourceIndex)];
        if (mapped != i) {
            needsMove = true;
        }
        targetOrder.push_back(mapped);
    }

    if (needsMove) {
        if (!FPDF_MovePages(scratchDoc, targetOrder.data(), static_cast<unsigned long>(targetOrder.size()), 0)) {
            result.status = OrganizeStatus::InternalError;
            return result;
        }
    }

    // 4c. Apply relative rotation (added to each page's existing rotation).
    for (int i = 0; i < keptCount; ++i) {
        const int deg = pages[static_cast<size_t>(i)].rotationDegrees;
        if (deg == 0) {
            continue;
        }
        FPDF_PAGE pg = FPDF_LoadPage(scratchDoc, i);
        if (!pg) {
            result.status = OrganizeStatus::InternalError;
            return result;
        }
        int cur = FPDFPage_GetRotation(pg);
        if (cur < 0) cur = 0;
        const int delta = deg / 90;
        FPDFPage_SetRotation(pg, (cur + delta) % 4);
        FPDF_ClosePage(pg);
    }

    // 5a. Post-pass: count dangling outline items (cycle-safe, capped).
    constexpr int kMaxOutlineNodes = 10000;
    int outlineVisitedCount = 0;
    std::unordered_set<FPDF_BOOKMARK> visitedBookmarks;
    std::queue<FPDF_BOOKMARK> bmQueue;
    for (FPDF_BOOKMARK top = FPDFBookmark_GetFirstChild(scratchDoc, nullptr);
         top && outlineVisitedCount < kMaxOutlineNodes;
         top = FPDFBookmark_GetNextSibling(scratchDoc, top)) {
        if (visitedBookmarks.insert(top).second) {
            bmQueue.push(top);
            ++outlineVisitedCount;
        } else {
            break;
        }
    }
    while (!bmQueue.empty()) {
        FPDF_BOOKMARK bm = bmQueue.front();
        bmQueue.pop();

        FPDF_DEST dest = FPDFBookmark_GetDest(scratchDoc, bm);
        if (!dest) {
            FPDF_ACTION act = FPDFBookmark_GetAction(bm);
            if (act) {
                dest = FPDFAction_GetDest(scratchDoc, act);
            }
        }
        if (dest && FPDFDest_GetDestPageIndex(scratchDoc, dest) < 0) {
            ++result.danglingOutlineCount;
        }

        for (FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(scratchDoc, bm);
             child && outlineVisitedCount < kMaxOutlineNodes;
             child = FPDFBookmark_GetNextSibling(scratchDoc, child)) {
            if (visitedBookmarks.insert(child).second) {
                bmQueue.push(child);
                ++outlineVisitedCount;
            } else {
                break;
            }
        }
    }

    // 5b. Post-pass: count dangling /Link annotations on kept pages (capped).
    constexpr int kMaxLinkScanAnnotations = 50000;
    int scannedAnnots = 0;
    for (int i = 0; i < keptCount && scannedAnnots < kMaxLinkScanAnnotations; ++i) {
        FPDF_PAGE pg = FPDF_LoadPage(scratchDoc, i);
        if (!pg) {
            continue;
        }
        const int annotCount = FPDFPage_GetAnnotCount(pg);
        for (int a = 0; a < annotCount && scannedAnnots < kMaxLinkScanAnnotations; ++a) {
            ++scannedAnnots;
            FPDF_ANNOTATION annot = FPDFPage_GetAnnot(pg, a);
            if (!annot) {
                continue;
            }
            if (FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_LINK) {
                FPDF_LINK link = FPDFAnnot_GetLink(annot);
                if (link) {
                    FPDF_DEST dest = FPDFLink_GetDest(scratchDoc, link);
                    if (!dest) {
                        FPDF_ACTION act = FPDFLink_GetAction(link);
                        if (act) {
                            dest = FPDFAction_GetDest(scratchDoc, act);
                        }
                    }
                    if (dest && FPDFDest_GetDestPageIndex(scratchDoc, dest) < 0) {
                        ++result.danglingLinkCount;
                    }
                }
            }
            FPDFPage_CloseAnnot(annot);
        }
        FPDF_ClosePage(pg);
    }

    // 6. Save non-incremental copy to writeFd.
    // Note: FPDF_SaveAsCopy on an encrypted scratchDoc preserves /Encrypt, so when allowDecrypt
    // is acknowledged for an encrypted source, import the organized pages into a fresh unencrypted doc.
    FPDF_DOCUMENT docToSave = scratchDoc;
    FPDF_DOCUMENT decryptedDoc = nullptr;
    struct DecryptedGuard {
        FPDF_DOCUMENT& doc;
        ~DecryptedGuard() {
            if (doc) FPDF_CloseDocument(doc);
        }
    } decryptedGuard{decryptedDoc};

    if (secRev != -1) {
        decryptedDoc = FPDF_CreateNewDocument();
        if (!decryptedDoc || !FPDF_ImportPagesByIndex(decryptedDoc, scratchDoc, nullptr, 0, 0)) {
            result.status = OrganizeStatus::SaveFailed;
            return result;
        }
        docToSave = decryptedDoc;
    }

    FdWriter writer(writeFd, std::move(progressCallback));
    if (FPDF_SaveAsCopy(docToSave, &writer, FPDF_NO_INCREMENTAL) == 0 ||
        writer.failed || writer.bytesWritten == 0) {
        result.status = OrganizeStatus::SaveFailed;
        return result;
    }

    result.status = OrganizeStatus::Ok;
    result.bytesWritten = writer.bytesWritten;
    return result;
}

std::unique_ptr<IDocument> createDocument() {
    return std::make_unique<PdfiumDocument>();
}

} // namespace pdf
