#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <QColor>
#include <QPainter>
#include <QString>

#include "engine/PageText.h"
#include "layout/DocumentLayout.h"
#include "ui/ColorMode.h"

namespace pdf::ipc {
class WorkerClient;
}

namespace ui {

struct TextPosition {
    int pageIndex{-1};
    int charIndex{-1};

    bool isValid() const noexcept { return pageIndex >= 0 && charIndex >= 0; }

    bool operator==(const TextPosition& o) const noexcept {
        return pageIndex == o.pageIndex && charIndex == o.charIndex;
    }

    bool operator!=(const TextPosition& o) const noexcept {
        return !(*this == o);
    }
};

class ViewerSelectionController {
public:
    using TextPosition = ui::TextPosition;
    static constexpr size_t MAX_CACHED_TEXT_PAGES = 48;

    ViewerSelectionController() = default;
    ~ViewerSelectionController() = default;

    ViewerSelectionController(const ViewerSelectionController&) = delete;
    ViewerSelectionController& operator=(const ViewerSelectionController&) = delete;
    ViewerSelectionController(ViewerSelectionController&&) = delete;
    ViewerSelectionController& operator=(ViewerSelectionController&&) = delete;

    // Cache state & accessors
    size_t cachedTextPageCount() const noexcept { return m_pageTextCache.size(); }
    std::shared_ptr<const pdf::PageText> getPageText(int pageIndex) const;
    const std::unordered_map<int, std::shared_ptr<pdf::PageText>>& pageTextCache() const noexcept {
        return m_pageTextCache;
    }

    void clearTextCache() noexcept { m_pageTextCache.clear(); }
    void trimPageTextCache(int currentPage);
    void ensurePageTextRequested(int pageIndex, bool hasDocument, int64_t currentDocumentId,
                                 pdf::ipc::WorkerClient& worker);

    bool handlePageTextReady(int64_t documentId, int64_t currentDocumentId, int pageIndex,
                             std::shared_ptr<pdf::PageText> text, int currentPage);

    // Selection state & accessors
    bool hasSelection() const noexcept {
        return m_selectionStart.isValid() && m_selectionEnd.isValid() &&
               (m_selectionStart.pageIndex != m_selectionEnd.pageIndex ||
                m_selectionStart.charIndex != m_selectionEnd.charIndex);
    }

    const TextPosition& selectionStart() const noexcept { return m_selectionStart; }
    const TextPosition& selectionEnd() const noexcept { return m_selectionEnd; }
    bool isSelecting() const noexcept { return m_isSelecting; }
    void setIsSelecting(bool selecting) noexcept { m_isSelecting = selecting; }

    bool clearSelection() noexcept;
    void reset() noexcept {
        clearTextCache();
        clearSelection();
    }

    QString selectedText() const;

    // Hit-testing & interactive drag helpers
    bool startSelection(int page, double px, double py);
    bool isOverText(int page, double px, double py) const;
    bool updateSelectionEnd(int page, double px, double py, bool& outChanged);
    bool finishSelection(bool& outSelectionCleared);

    // Rendering
    void drawSelection(QPainter& painter, int pageIndex, const layout::PageLayout& pl,
                       int64_t scrollDocX, int64_t scrollDocY, double effScale,
                       ColorMode colorMode, int rotation) const;

private:
    std::unordered_map<int, std::shared_ptr<pdf::PageText>> m_pageTextCache;
    TextPosition m_selectionStart;
    TextPosition m_selectionEnd;
    bool m_isSelecting{false};
};

} // namespace ui
