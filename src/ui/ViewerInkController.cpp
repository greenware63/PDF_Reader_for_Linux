#include "ui/ViewerInkController.h"

#include <algorithm>
#include <utility>

#include <QColor>
#include <QPen>
#include <QPointF>
#include <QPolygonF>

namespace ui {

bool ViewerInkController::setMode(bool enabled) noexcept {
    if (m_inkDrawingMode == enabled) {
        return false;
    }
    m_inkDrawingMode = enabled;
    if (!m_inkDrawingMode) {
        if (m_isDrawingInk) {
            m_isDrawingInk = false;
            m_currentInkStroke.clear();
            m_inkCurrentPage = -1;
        }
    }
    return true;
}

bool ViewerInkController::beginStroke(const layout::DocumentLayout& layout, int64_t docX, int64_t docY) {
    int page = layout.getPageAt(docX, docY);
    if (page >= 0) {
        double ax = 0.0, ay = 0.0;
        if (layout.documentToPagePoints(page, docX, docY, ax, ay)) {
            const auto& pl = layout.getPageLayout(page);
            m_isDrawingInk = true;
            m_inkCurrentPage = page;
            m_currentInkStroke.clear();
            m_currentInkStroke.push_back({ax, pl.heightPoints - ay});
            return true;
        }
    }
    return false;
}

bool ViewerInkController::extendStroke(const layout::DocumentLayout& layout, int64_t docX, int64_t docY) {
    if (m_inkCurrentPage < 0) {
        return false;
    }
    double ax = 0.0, ay = 0.0;
    if (layout.documentToPagePoints(m_inkCurrentPage, docX, docY, ax, ay)) {
        const auto& pl = layout.getPageLayout(m_inkCurrentPage);
        const double userX = ax;
        const double userY = pl.heightPoints - ay;
        if (m_currentInkStroke.empty()) {
            m_currentInkStroke.push_back({userX, userY});
        } else {
            const auto& last = m_currentInkStroke.back();
            const double dx = userX - last.x;
            const double dy = userY - last.y;
            if ((dx * dx + dy * dy >= 1.0) && m_currentInkStroke.size() < pdf::kMaxInkPoints) {
                m_currentInkStroke.push_back({userX, userY});
                return true;
            }
        }
    }
    return false;
}

bool ViewerInkController::finishStroke(int& outPage, std::vector<pdf::AnnotationPoint>& outPoints) {
    m_isDrawingInk = false;
    bool hasStroke = false;
    if (m_inkCurrentPage >= 0 && !m_currentInkStroke.empty()) {
        if (m_currentInkStroke.size() == 1) {
            m_currentInkStroke.push_back({m_currentInkStroke[0].x + 0.1, m_currentInkStroke[0].y});
        }
        outPage = m_inkCurrentPage;
        outPoints = std::move(m_currentInkStroke);
        m_currentInkStroke.clear();
        hasStroke = true;
    }
    m_inkCurrentPage = -1;
    return hasStroke;
}

void ViewerInkController::drawActiveStroke(QPainter& painter, const layout::DocumentLayout& layout,
                                           int pageCount, int64_t scrollDocX, int64_t scrollDocY) const {
    if (!m_isDrawingInk || m_inkCurrentPage < 0 || m_inkCurrentPage >= pageCount || m_currentInkStroke.size() < 2) {
        return;
    }
    const auto& pl = layout.getPageLayout(m_inkCurrentPage);
    QPolygonF poly;
    poly.reserve(static_cast<qsizetype>(m_currentInkStroke.size()));
    for (const auto& pt : m_currentInkStroke) {
        const double pageX = pt.x;
        const double pageY = pl.heightPoints - pt.y;
        int64_t docX = 0, docY = 0;
        if (layout.pagePointsToDocument(m_inkCurrentPage, pageX, pageY, docX, docY)) {
            poly.append(QPointF(static_cast<qreal>(docX - scrollDocX), static_cast<qreal>(docY - scrollDocY)));
        }
    }
    if (poly.size() >= 2) {
        const double ptsToDevPixels = (pl.widthPoints > 0.0) ? (static_cast<double>(pl.widthPixel) / pl.widthPoints) : 1.0;
        const double effBw = std::max(1.0, m_inkBorderWidth * ptsToDevPixels);
        QPen pen(QColor::fromRgba(static_cast<QRgb>(m_inkColorArgb)), static_cast<qreal>(effBw), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.save();
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(poly);
        painter.restore();
    }
}

} // namespace ui
