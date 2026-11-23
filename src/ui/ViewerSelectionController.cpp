#include "ui/ViewerSelectionController.h"

#include <algorithm>
#include <cmath>

#include "ipc/WorkerClient.h"
#include "ui/ImageUtils.h"

namespace ui {

static void rotateRectPointsForDisplay(const pdf::PageCharBox& r, double pageW, double pageH, int rotation,
                                       double& outPx, double& outPy, double& outPw, double& outPh) {
    const int normRot = ((rotation % 360) + 360) % 360;
    outPx = r.left;
    outPy = r.top;
    outPw = r.width();
    outPh = r.height();

    switch (normRot) {
        case 90:
            outPx = pageH - r.bottom;
            outPy = r.left;
            outPw = r.height();
            outPh = r.width();
            break;
        case 180:
            outPx = pageW - r.right;
            outPy = pageH - r.bottom;
            break;
        case 270:
            outPx = r.top;
            outPy = pageW - r.right;
            outPw = r.height();
            outPh = r.width();
            break;
        default:
            break;
    }
}

std::shared_ptr<const pdf::PageText> ViewerSelectionController::getPageText(int pageIndex) const {
    auto it = m_pageTextCache.find(pageIndex);
    return (it != m_pageTextCache.end()) ? it->second : nullptr;
}

void ViewerSelectionController::trimPageTextCache(int currentPage) {
    if (m_pageTextCache.size() <= MAX_CACHED_TEXT_PAGES) {
        return;
    }
    // Pages that must stay: the selection range (its text is needed for copy) ...
    int keepFirst = 1 << 30, keepLast = -1;
    if (m_selectionStart.isValid() && m_selectionEnd.isValid()) {
        keepFirst = std::min(m_selectionStart.pageIndex, m_selectionEnd.pageIndex);
        keepLast = std::max(m_selectionStart.pageIndex, m_selectionEnd.pageIndex);
    }
    // ... and, of the rest, the pages nearest to the one being viewed. Requests still in flight are kept.
    std::vector<int> evictable;
    evictable.reserve(m_pageTextCache.size());
    for (const auto& [page, text] : m_pageTextCache) {
        if (text && (page < keepFirst || page > keepLast)) {
            evictable.push_back(page);
        }
    }
    std::sort(evictable.begin(), evictable.end(), [currentPage](int a, int b) {
        return std::abs(a - currentPage) > std::abs(b - currentPage); // farthest first
    });
    for (int page : evictable) {
        if (m_pageTextCache.size() <= MAX_CACHED_TEXT_PAGES) {
            break;
        }
        m_pageTextCache.erase(page);
    }
}

void ViewerSelectionController::ensurePageTextRequested(int pageIndex, bool hasDocument, int64_t currentDocumentId,
                                                         pdf::ipc::WorkerClient& worker) {
    if (pageIndex < 0 || !hasDocument) return;
    if (m_pageTextCache.find(pageIndex) == m_pageTextCache.end()) {
        m_pageTextCache[pageIndex] = nullptr; // marked in-flight
        worker.requestPageText(currentDocumentId, pageIndex);
    }
}

bool ViewerSelectionController::handlePageTextReady(int64_t documentId, int64_t currentDocumentId, int pageIndex,
                                                     std::shared_ptr<pdf::PageText> text, int currentPage) {
    if (documentId != currentDocumentId) {
        return false;
    }
    m_pageTextCache[pageIndex] = text ? std::move(text) : std::make_shared<pdf::PageText>();
    trimPageTextCache(currentPage);
    return true;
}

bool ViewerSelectionController::clearSelection() noexcept {
    if (m_selectionStart.isValid() || m_selectionEnd.isValid()) {
        m_selectionStart = {};
        m_selectionEnd = {};
        m_isSelecting = false;
        return true;
    }
    m_isSelecting = false;
    return false;
}

QString ViewerSelectionController::selectedText() const {
    if (!hasSelection()) {
        return QString();
    }

    bool forward = (m_selectionStart.pageIndex < m_selectionEnd.pageIndex) ||
                   (m_selectionStart.pageIndex == m_selectionEnd.pageIndex && m_selectionStart.charIndex <= m_selectionEnd.charIndex);
    int p0 = forward ? m_selectionStart.pageIndex : m_selectionEnd.pageIndex;
    int p1 = forward ? m_selectionEnd.pageIndex : m_selectionStart.pageIndex;
    int c0 = forward ? m_selectionStart.charIndex : m_selectionEnd.charIndex;
    int c1 = forward ? m_selectionEnd.charIndex : m_selectionStart.charIndex;

    QString result;
    for (int p = p0; p <= p1; ++p) {
        auto it = m_pageTextCache.find(p);
        if (it == m_pageTextCache.end() || !it->second) {
            continue;
        }
        const auto& pt = *it->second;
        int startChar = (p == p0) ? c0 : 0;
        int endChar = (p == p1) ? c1 : static_cast<int>(pt.charCount()) - 1;
        auto selU16 = pt.getSelectedText(startChar, endChar);
        if (!selU16.empty()) {
            if (!result.isEmpty()) {
                result.append(QChar::LineFeed);
            }
            result.append(QString::fromStdU16String(selU16));
        }
    }
    return result;
}

bool ViewerSelectionController::startSelection(int page, double px, double py) {
    auto it = m_pageTextCache.find(page);
    if (it != m_pageTextCache.end() && it->second) {
        int ch = it->second->hitTest(px, py);
        if (ch >= 0) {
            m_selectionStart = {page, ch};
            m_selectionEnd = {page, ch};
            m_isSelecting = true;
            return true;
        }
    }
    return false;
}

bool ViewerSelectionController::isOverText(int page, double px, double py) const {
    auto it = m_pageTextCache.find(page);
    if (it != m_pageTextCache.end() && it->second) {
        return it->second->hitTest(px, py) >= 0;
    }
    return false;
}

bool ViewerSelectionController::updateSelectionEnd(int page, double px, double py, bool& outChanged) {
    outChanged = false;
    auto it = m_pageTextCache.find(page);
    if (it != m_pageTextCache.end() && it->second && !it->second->empty()) {
        int ch = it->second->hitTest(px, py);
        if (ch < 0) {
            if (py < it->second->chars.front().top) {
                ch = 0;
            } else if (py > it->second->chars.back().bottom) {
                ch = static_cast<int>(it->second->charCount()) - 1;
            } else {
                double minDist = 1e9;
                int nearest = 0;
                for (size_t i = 0; i < it->second->charCount(); ++i) {
                    const auto& b = it->second->chars[i];
                    double cy = (b.top + b.bottom) * 0.5;
                    double d = std::abs(cy - py);
                    if (d < minDist) {
                        minDist = d;
                        nearest = static_cast<int>(i);
                    }
                }
                ch = nearest;
            }
        }
        if (ch >= 0) {
            TextPosition newEnd{page, ch};
            if (newEnd != m_selectionEnd) {
                m_selectionEnd = newEnd;
                outChanged = true;
            }
            return true;
        }
    }
    return false;
}

bool ViewerSelectionController::finishSelection(bool& outSelectionCleared) {
    if (!m_isSelecting) {
        outSelectionCleared = false;
        return false;
    }
    m_isSelecting = false;
    if (m_selectionStart == m_selectionEnd) {
        m_selectionStart = {};
        m_selectionEnd = {};
        outSelectionCleared = true;
    } else {
        outSelectionCleared = false;
    }
    return true;
}

void ViewerSelectionController::drawSelection(QPainter& painter, int pageIndex, const layout::PageLayout& pl,
                                             int64_t scrollDocX, int64_t scrollDocY, double effScale,
                                             ColorMode colorMode, int rotation) const {
    if (!hasSelection() || effScale <= 0.0) {
        return;
    }

    bool forward = (m_selectionStart.pageIndex < m_selectionEnd.pageIndex) ||
                   (m_selectionStart.pageIndex == m_selectionEnd.pageIndex && m_selectionStart.charIndex <= m_selectionEnd.charIndex);
    int p0 = forward ? m_selectionStart.pageIndex : m_selectionEnd.pageIndex;
    int p1 = forward ? m_selectionEnd.pageIndex : m_selectionStart.pageIndex;
    int c0 = forward ? m_selectionStart.charIndex : m_selectionEnd.charIndex;
    int c1 = forward ? m_selectionEnd.charIndex : m_selectionStart.charIndex;

    if (pageIndex < p0 || pageIndex > p1) {
        return;
    }

    auto it = m_pageTextCache.find(pageIndex);
    if (it == m_pageTextCache.end() || !it->second) {
        return;
    }
    const auto& pt = *it->second;

    int startChar = (pageIndex == p0) ? c0 : 0;
    int endChar = (pageIndex == p1) ? c1 : static_cast<int>(pt.charCount()) - 1;

    auto rects = pt.getSelectionRects(startChar, endChar);
    QColor highlightColor;
    switch (colorMode) {
        case ColorMode::Inverted:     highlightColor = QColor(0, 140, 255, 120); break;
        case ColorMode::HighContrast: highlightColor = QColor(255, 215, 0, 130); break;
        case ColorMode::Sepia:        highlightColor = QColor(180, 130, 60, 110); break;
        case ColorMode::Normal:
        default:                      highlightColor = QColor(0, 120, 215, 90); break;
    }

    for (const auto& r : rects) {
        double px = 0.0, py = 0.0, pw = 0.0, ph = 0.0;
        rotateRectPointsForDisplay(r, pl.widthPoints, pl.heightPoints, rotation, px, py, pw, ph);
        QRectF selRect(
            static_cast<qreal>(static_cast<double>(pl.leftPixel - scrollDocX) + px * effScale),
            static_cast<qreal>(static_cast<double>(pl.topPixel - scrollDocY) + py * effScale),
            static_cast<qreal>(pw * effScale),
            static_cast<qreal>(ph * effScale)
        );
        painter.fillRect(selRect, highlightColor);
    }
}

} // namespace ui
