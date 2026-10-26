#include "SearchResultsWidget.h"
#include "PdfViewerWidget.h"

#include <QApplication>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTextOption>
#include <QAbstractTextDocumentLayout>

namespace ui {

namespace {

class SearchResultDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        const int line = option.fontMetrics.lineSpacing();
        return QSize(option.rect.width(), line * 2 + 10);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString label = index.data(SearchResultsModel::PageLabelRole).toString();
        const QString snippet = index.data(Qt::DisplayRole).toString();
        const int start = index.data(SearchResultsModel::SnippetStartRole).toInt();
        const int length = index.data(SearchResultsModel::SnippetLengthRole).toInt();

        // Background / selection frame
        opt.text.clear();
        const QWidget* w = opt.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);

        painter->save();
        const QRect r = opt.rect.adjusted(6, 3, -6, -3);
        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette::ColorRole fg = selected ? QPalette::HighlightedText : QPalette::Text;

        // Line 1: page label (dimmed, small)
        QFont small = opt.font;
        small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.85));
        painter->setFont(small);
        QColor dim = opt.palette.color(fg);
        dim.setAlpha(160);
        painter->setPen(dim);
        const QFontMetrics sfm(small);
        painter->drawText(QRect(r.left(), r.top(), r.width(), sfm.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, tr_page(label));

        // Line 2: snippet with the hit in bold. QTextDocument handles bidi (Persian/Arabic) and clipping.
        const bool valid = start >= 0 && length >= 0 && start + length <= snippet.size();
        const QString pre = valid ? snippet.left(start).toHtmlEscaped() : snippet.toHtmlEscaped();
        const QString hit = valid ? snippet.mid(start, length).toHtmlEscaped() : QString();
        const QString post = valid ? snippet.mid(start + length).toHtmlEscaped() : QString();
        QTextDocument doc;
        doc.setDefaultFont(opt.font);
        doc.setDocumentMargin(0);
        QTextOption to;
        to.setWrapMode(QTextOption::NoWrap);
        doc.setDefaultTextOption(to);
        doc.setHtml(QStringLiteral("<span>%1<b style=\"background-color:%4\">%2</b>%3</span>")
                        .arg(pre, hit, post, selected ? QStringLiteral("rgba(255,255,255,30%)") : QStringLiteral("rgba(255,190,0,38%)")));
        painter->translate(r.left(), r.top() + sfm.height() + 1);
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, opt.palette.color(fg));
        ctx.clip = QRectF(0, 0, r.width(), r.height() - sfm.height());
        painter->setClipRect(ctx.clip);
        doc.documentLayout()->draw(painter, ctx);
        painter->restore();
    }

private:
    static QString tr_page(const QString& label) { return QObject::tr("Page %1").arg(label); }
};

} // namespace

// ---------------------------------------------------------------- model

SearchResultsModel::SearchResultsModel(QObject* parent) : QAbstractListModel(parent) {}

void SearchResultsModel::setViewer(PdfViewerWidget* viewer) {
    beginResetModel();
    m_viewer = viewer;
    m_rows = viewer ? viewer->searchMatchCount() : 0;
    endResetModel();
}

void SearchResultsModel::refresh() {
    const int rows = m_viewer ? m_viewer->searchMatchCount() : 0;
    if (rows == m_rows) {
        return; // same list (only the current hit moved)
    }
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

int SearchResultsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_rows;
}

QVariant SearchResultsModel::data(const QModelIndex& index, int role) const {
    if (!m_viewer || !index.isValid() || index.row() < 0 || index.row() >= m_rows ||
        index.row() >= m_viewer->searchMatchCount()) {
        return {};
    }
    const pdf::SearchMatch& m = m_viewer->searchMatches()[static_cast<size_t>(index.row())];
    switch (role) {
        case Qt::DisplayRole:      return m.snippet;
        case PageLabelRole:        return m_viewer->pageLabel(m.pageIndex);
        case SnippetStartRole:     return m.snippetMatchStart;
        case SnippetLengthRole:    return m.snippetMatchLength;
        default:                   return {};
    }
}

// ---------------------------------------------------------------- view

SearchResultsWidget::SearchResultsWidget(QWidget* parent) : QListView(parent) {
    setObjectName(QStringLiteral("SearchResultsList"));
    m_model = new SearchResultsModel(this);
    setModel(m_model);
    setItemDelegate(new SearchResultDelegate(this));
    setUniformItemSizes(true);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(this, &QListView::clicked, this, &SearchResultsWidget::onActivated);
    connect(this, &QListView::activated, this, &SearchResultsWidget::onActivated);
}

void SearchResultsWidget::setViewer(PdfViewerWidget* viewer) {
    if (m_viewer == viewer) {
        return;
    }
    if (m_viewer) {
        disconnect(m_viewer, nullptr, this, nullptr);
    }
    m_viewer = viewer;
    m_model->setViewer(viewer);
    if (viewer) {
        connect(viewer, &PdfViewerWidget::searchResultsChanged, this, &SearchResultsWidget::onSearchResultsChanged);
    }
    m_lastCount = -1;
    onSearchResultsChanged(viewer ? viewer->currentSearchMatchIndex() + 1 : 0,
                           viewer ? viewer->searchMatchCount() : 0);
}

void SearchResultsWidget::onSearchResultsChanged(int /*currentMatch*/, int /*totalMatches*/) {
    m_model->refresh();
    const int count = m_model->rowCount();
    if (count != m_lastCount) {
        m_lastCount = count;
        Q_EMIT resultCountChanged(count);
    }
    // Keep the row of the viewer's current hit selected, without re-triggering navigation.
    const int cur = m_viewer ? m_viewer->currentSearchMatchIndex() : -1;
    if (cur >= 0 && cur < count) {
        const QModelIndex idx = m_model->index(cur);
        if (currentIndex() != idx) {
            selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect);
            scrollTo(idx, QAbstractItemView::EnsureVisible);
        }
    } else {
        clearSelection();
    }
}

void SearchResultsWidget::onActivated(const QModelIndex& index) {
    if (m_viewer && index.isValid()) {
        m_viewer->goToSearchMatch(index.row());
    }
}

} // namespace ui
