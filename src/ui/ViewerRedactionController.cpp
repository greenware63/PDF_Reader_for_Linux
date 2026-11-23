#include "ui/ViewerRedactionController.h"

#include <algorithm>
#include <cmath>

namespace ui {

bool ViewerRedactionController::setMode(bool enabled) noexcept {
    if (m_mode == enabled) {
        return false;
    }
    m_mode = enabled;
    return true;
}

void ViewerRedactionController::beginDrag(int page, const QPointF& docPos) noexcept {
    m_isDragging = true;
    m_dragStartPage = page;
    m_dragStartDocPos = docPos;
    m_dragCurrentDocPos = docPos;
}

void ViewerRedactionController::updateDrag(const QPointF& docPos) noexcept {
    m_dragCurrentDocPos = docPos;
}

void ViewerRedactionController::resetDrag() noexcept {
    m_isDragging = false;
    m_dragStartPage = -1;
}

bool ViewerRedactionController::finishDrag(const layout::DocumentLayout& layout, bool documentOpen,
                                          int& outPage, pdf::ipc::RedactRect& outRect) noexcept {
    m_isDragging = false;
    const int page = m_dragStartPage;
    bool staged = false;
    if (page >= 0 && documentOpen) {
        // Marks are stored in canonical page space (unrotated, top-left origin, PDF points), the
        // space pdf_redact expects. The layout owns the view-rotation transform, so the same
        // code path is correct at every rotation. Both drag corners are mapped, then normalised.
        const auto& pl = layout.getPageLayout(page);
        double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
        const auto toDoc = [](const QPointF& p, int64_t& dx, int64_t& dy) {
            dx = static_cast<int64_t>(std::llround(p.x()));
            dy = static_cast<int64_t>(std::llround(p.y()));
        };
        int64_t sx = 0, sy = 0, ex = 0, ey = 0;
        toDoc(m_dragStartDocPos, sx, sy);
        toDoc(m_dragCurrentDocPos, ex, ey);
        if (layout.documentToPagePoints(page, sx, sy, ax, ay) &&
            layout.documentToPagePoints(page, ex, ey, bx, by)) {
            const double x0 = std::max(0.0, std::min(pl.widthPoints, std::min(ax, bx)));
            const double y0 = std::max(0.0, std::min(pl.heightPoints, std::min(ay, by)));
            const double x1 = std::max(0.0, std::min(pl.widthPoints, std::max(ax, bx)));
            const double y1 = std::max(0.0, std::min(pl.heightPoints, std::max(ay, by)));
            if ((x1 - x0) >= 3.0 && (y1 - y0) >= 3.0) {
                outPage = page;
                outRect = pdf::ipc::RedactRect{x0, y0, x1, y1};
                staged = true;
            }
        }
    }
    m_dragStartPage = -1;
    return staged;
}

bool ViewerRedactionController::addStaged(int pageIndex, const pdf::ipc::RedactRect& rect, int pageCount) {
    if (pageIndex < 0 || pageIndex >= pageCount) {
        return false;
    }
    pdf::ipc::RedactRect norm{
        std::min(rect.left, rect.right),
        std::min(rect.top, rect.bottom),
        std::max(rect.left, rect.right),
        std::max(rect.top, rect.bottom)
    };
    if (norm.width() <= 0.0 || norm.height() <= 0.0) {
        return false;
    }
    m_stagedRedactions[pageIndex].push_back(norm);
    return true;
}

bool ViewerRedactionController::stageSelectionRect(int page, const pdf::PageCharBox& box,
                                                   double widthPoints, double heightPoints) {
    // Safety margin (1.5 pt, ~3.1 px at 150 DPI) ensuring font descenders (g, y, p, q),
    // diacritics/accents, and italic overhang are fully enveloped by the redaction box.
    constexpr double kSelectionPadPt = 1.5;
    const double rLeft = std::min(box.left, box.right);
    const double rTop = std::min(box.top, box.bottom);
    const double rRight = std::max(box.left, box.right);
    const double rBottom = std::max(box.top, box.bottom);

    const double padLeft = std::max(0.0, rLeft - kSelectionPadPt);
    const double padTop = std::max(0.0, rTop - kSelectionPadPt);
    const double padRight = (widthPoints > 0.0) ? std::min(widthPoints, rRight + kSelectionPadPt) : (rRight + kSelectionPadPt);
    const double padBottom = (heightPoints > 0.0) ? std::min(heightPoints, rBottom + kSelectionPadPt) : (rBottom + kSelectionPadPt);

    pdf::ipc::RedactRect rr{padLeft, padTop, padRight, padBottom};
    if (rr.width() > 0.0 && rr.height() > 0.0) {
        m_stagedRedactions[page].push_back(rr);
        return true;
    }
    return false;
}

bool ViewerRedactionController::clear() noexcept {
    if (m_stagedRedactions.empty()) {
        return false;
    }
    m_stagedRedactions.clear();
    return true;
}

int ViewerRedactionController::count() const noexcept {
    int total = 0;
    for (const auto& [_, rects] : m_stagedRedactions) {
        total += static_cast<int>(rects.size());
    }
    return total;
}

void ViewerRedactionController::drawOverlays(QPainter& painter, const layout::DocumentLayout& layout,
                                            const layout::ViewportRange& visPages,
                                            int64_t scrollDocX, int64_t scrollDocY, double effScale) const {
    if (!visPages.isValid() || effScale <= 0.0) {
        return;
    }

    // Draw committed staged redactions
    for (int p = visPages.firstVisiblePage; p <= visPages.lastVisiblePage; ++p) {
        auto it = m_stagedRedactions.find(p);
        if (it == m_stagedRedactions.end() || it->second.empty()) {
            continue;
        }
        for (const auto& r : it->second) {
            // Canonical page points -> document pixels under the current view rotation.
            int64_t d0x = 0, d0y = 0, d1x = 0, d1y = 0;
            if (!layout.pagePointsToDocument(p, r.left, r.top, d0x, d0y) ||
                !layout.pagePointsToDocument(p, r.right, r.bottom, d1x, d1y)) {
                continue;
            }
            const qreal rx = static_cast<qreal>(std::min(d0x, d1x) - scrollDocX);
            const qreal ry = static_cast<qreal>(std::min(d0y, d1y) - scrollDocY);
            const qreal rw = static_cast<qreal>(std::max(d0x, d1x) - std::min(d0x, d1x));
            const qreal rh = static_cast<qreal>(std::max(d0y, d1y) - std::min(d0y, d1y));
            QRectF box(rx, ry, rw, rh);

            // Semi-opaque dark mask
            painter.fillRect(box, QColor(0, 0, 0, 190));
            // Crimson red boundary
            QPen pen(QColor(220, 20, 60), 2);
            painter.setPen(pen);
            painter.drawRect(box);

            // Small badge when box is sufficiently large
            if (rw >= 40.0 && rh >= 16.0) {
                painter.setFont(QFont(QStringLiteral("Arial"), 8, QFont::Bold));
                painter.setPen(QColor(255, 255, 255, 230));
                painter.drawText(box, Qt::AlignCenter, QStringLiteral("REDACT"));
            }
        }
    }

    // Draw active drag rubber-band
    if (m_isDragging && m_dragStartPage >= 0) {
        const qreal rx = static_cast<qreal>(std::min(m_dragStartDocPos.x(), m_dragCurrentDocPos.x()) - static_cast<double>(scrollDocX));
        const qreal ry = static_cast<qreal>(std::min(m_dragStartDocPos.y(), m_dragCurrentDocPos.y()) - static_cast<double>(scrollDocY));
        const qreal rw = static_cast<qreal>(std::abs(m_dragCurrentDocPos.x() - m_dragStartDocPos.x()));
        const qreal rh = static_cast<qreal>(std::abs(m_dragCurrentDocPos.y() - m_dragStartDocPos.y()));
        QRectF dragBox(rx, ry, rw, rh);

        painter.fillRect(dragBox, QColor(220, 20, 60, 70));
        QPen dragPen(QColor(220, 20, 60), 2, Qt::DashLine);
        painter.setPen(dragPen);
        painter.drawRect(dragBox);
    }
}

} // namespace ui
