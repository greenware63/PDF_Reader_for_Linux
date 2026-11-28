#include "OrganizePagesDialog.h"
#include "PdfViewerWidget.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QTransform>
#include <QVBoxLayout>
#include <algorithm>

namespace ui {

OrganizePagesDialog::OrganizePagesDialog(PdfViewerWidget* viewer, QWidget* parent)
    : QDialog(parent)
    , m_viewer(viewer)
    , m_plan(viewer ? viewer->pageCount() : 0) {
    setWindowTitle(tr("Organize Pages"));
    resize(760, 540);

    auto* root = new QVBoxLayout(this);

    auto* toolbar = new QHBoxLayout();
    m_rotateCcwBtn = new QPushButton(tr("Rotate &Left (CCW)"), this);
    m_rotateCwBtn = new QPushButton(tr("Rotate &Right (CW)"), this);
    m_deleteBtn = new QPushButton(tr("&Delete"), this);
    m_resetBtn = new QPushButton(tr("Re&set"), this);
    toolbar->addWidget(m_rotateCcwBtn);
    toolbar->addWidget(m_rotateCwBtn);
    toolbar->addWidget(m_deleteBtn);
    toolbar->addStretch();
    toolbar->addWidget(m_resetBtn);
    root->addLayout(toolbar);

    m_listWidget = new QListWidget(this);
    m_listWidget->setViewMode(QListView::IconMode);
    m_listWidget->setIconSize(QSize(128, 160));
    m_listWidget->setGridSize(QSize(152, 200));
    m_listWidget->setMovement(QListView::Snap);
    m_listWidget->setDragDropMode(QAbstractItemView::InternalMove);
    m_listWidget->setDefaultDropAction(Qt::MoveAction);
    m_listWidget->setDragEnabled(true);
    m_listWidget->setAcceptDrops(true);
    m_listWidget->setDropIndicatorShown(true);
    m_listWidget->setResizeMode(QListView::Adjust);
    m_listWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_listWidget->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_listWidget->setWordWrap(true);
    root->addWidget(m_listWidget, 1);

    auto* noteLabel = new QLabel(
        tr("Drag pages to reorder, or select pages to rotate or delete. Organizing writes a new PDF copy."),
        this);
    noteLabel->setWordWrap(true);
    root->addWidget(noteLabel);

    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(m_buttonBox);

    connect(m_rotateCcwBtn, &QPushButton::clicked, this, &OrganizePagesDialog::onRotateCounterClockwise);
    connect(m_rotateCwBtn, &QPushButton::clicked, this, &OrganizePagesDialog::onRotateClockwise);
    connect(m_deleteBtn, &QPushButton::clicked, this, &OrganizePagesDialog::onDeleteSelected);
    connect(m_resetBtn, &QPushButton::clicked, this, &OrganizePagesDialog::onResetPlan);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_listWidget, &QListWidget::itemSelectionChanged, this, &OrganizePagesDialog::updateControls);
    connect(m_listWidget->model(), &QAbstractItemModel::rowsMoved, this, &OrganizePagesDialog::onRowsMoved);

    if (m_listWidget->verticalScrollBar()) {
        connect(m_listWidget->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
            QTimer::singleShot(10, this, &OrganizePagesDialog::requestVisibleThumbnails);
        });
    }

    if (m_viewer) {
        m_viewer->setOrganizeThumbnailMode(true);
        connect(m_viewer, &PdfViewerWidget::organizeThumbnailReady,
                this, &OrganizePagesDialog::onThumbnailReady);
        connect(m_viewer, &PdfViewerWidget::organizeThumbnailFailed,
                this, &OrganizePagesDialog::onThumbnailFailed);
    }

    rebuildListFromPlan();
    updateControls();
}

OrganizePagesDialog::~OrganizePagesDialog() {
    if (m_viewer) {
        m_viewer->setOrganizeThumbnailMode(false);
    }
}

void OrganizePagesDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    QTimer::singleShot(10, this, &OrganizePagesDialog::requestVisibleThumbnails);
}

void OrganizePagesDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    QTimer::singleShot(10, this, &OrganizePagesDialog::requestVisibleThumbnails);
}

void OrganizePagesDialog::rebuildListFromPlan() {
    m_listWidget->clear();
    const auto& specs = m_plan.pageSpecs();
    for (const auto& spec : specs) {
        auto* item = new QListWidgetItem(m_listWidget);
        item->setData(Qt::UserRole, spec.sourceIndex);
        item->setFlags((item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable | Qt::ItemIsEnabled) &
                       ~Qt::ItemIsDropEnabled);
        updateItemVisual(item, spec);
    }
}

void OrganizePagesDialog::updateItemVisual(QListWidgetItem* item, const pdf::PageSpec& spec) {
    if (!item) {
        return;
    }
    QString text = tr("Page %1").arg(spec.sourceIndex + 1);
    if (spec.rotationDegrees != 0) {
        text += tr(" (%1°)").arg(spec.rotationDegrees);
    }
    item->setText(text);

    auto it = m_rawThumbnails.find(spec.sourceIndex);
    if (it != m_rawThumbnails.end() && !it->second.isNull()) {
        QImage img = it->second;
        if (spec.rotationDegrees != 0) {
            QTransform xf;
            xf.rotate(spec.rotationDegrees);
            img = img.transformed(xf, Qt::SmoothTransformation);
        }
        item->setIcon(QIcon(QPixmap::fromImage(img)));
    } else {
        QPixmap placeholder(100, 130);
        placeholder.fill(QColor(245, 245, 245));
        QPainter p(&placeholder);
        p.setPen(QColor(180, 180, 180));
        p.drawRect(0, 0, placeholder.width() - 1, placeholder.height() - 1);
        p.setPen(QColor(120, 120, 120));
        p.drawText(placeholder.rect(), Qt::AlignCenter, QString::number(spec.sourceIndex + 1));
        item->setIcon(QIcon(placeholder));
    }
}

std::vector<int> OrganizePagesDialog::selectedRowsDescending() const {
    auto rows = selectedRowsAscending();
    std::reverse(rows.begin(), rows.end());
    return rows;
}

std::vector<int> OrganizePagesDialog::selectedRowsAscending() const {
    std::vector<int> rows;
    for (int i = 0; i < m_listWidget->count(); ++i) {
        if (auto* it = m_listWidget->item(i); it && it->isSelected()) {
            rows.push_back(i);
        }
    }
    return rows;
}

void OrganizePagesDialog::onRotateClockwise() {
    const auto rows = selectedRowsAscending();
    bool changed = false;
    for (int row : rows) {
        if (m_plan.rotateClockwise(row)) {
            updateItemVisual(m_listWidget->item(row), m_plan.pageSpecs()[static_cast<size_t>(row)]);
            changed = true;
        }
    }
    if (changed) {
        updateControls();
    }
}

void OrganizePagesDialog::onRotateCounterClockwise() {
    const auto rows = selectedRowsAscending();
    bool changed = false;
    for (int row : rows) {
        if (m_plan.rotateCounterClockwise(row)) {
            updateItemVisual(m_listWidget->item(row), m_plan.pageSpecs()[static_cast<size_t>(row)]);
            changed = true;
        }
    }
    if (changed) {
        updateControls();
    }
}

void OrganizePagesDialog::onDeleteSelected() {
    const auto rows = selectedRowsAscending();
    if (!m_plan.deletePositions(rows)) {
        return;
    }
    for (int row : selectedRowsDescending()) {
        delete m_listWidget->takeItem(row);
    }
    updateControls();
    requestVisibleThumbnails();
}

void OrganizePagesDialog::onResetPlan() {
    m_plan.reset();
    rebuildListFromPlan();
    updateControls();
    requestVisibleThumbnails();
}

void OrganizePagesDialog::onRowsMoved() {
    std::vector<int> newOrder;
    newOrder.reserve(static_cast<size_t>(m_listWidget->count()));
    for (int i = 0; i < m_listWidget->count(); ++i) {
        if (auto* it = m_listWidget->item(i)) {
            newOrder.push_back(it->data(Qt::UserRole).toInt());
        }
    }
    m_plan.reorderBySourceIndices(newOrder);
    updateControls();
}

void OrganizePagesDialog::updateControls() {
    const auto rows = selectedRowsAscending();
    const bool hasSelection = !rows.empty();
    const bool canDelete = hasSelection && (m_plan.count() - static_cast<int>(rows.size()) >= 1);
    m_rotateCcwBtn->setEnabled(hasSelection);
    m_rotateCwBtn->setEnabled(hasSelection);
    m_deleteBtn->setEnabled(canDelete);
    m_resetBtn->setEnabled(!m_plan.isUnchanged());
    if (auto* okBtn = m_buttonBox->button(QDialogButtonBox::Ok)) {
        okBtn->setEnabled(!m_plan.isUnchanged() && !m_plan.empty());
    }
}

void OrganizePagesDialog::requestVisibleThumbnails() {
    if (!m_viewer || m_listWidget->count() == 0) {
        return;
    }
    const QRect vpRect = m_listWidget->viewport()->rect();
    const bool hasLayout = vpRect.isValid() && vpRect.width() > 0 && vpRect.height() > 0;
    const QRect prefetchRect = hasLayout
        ? vpRect.adjusted(0, -250, 0, 250)
        : QRect(0, 0, 400, 800);

    int startIndex = 0;
    int endIndex = m_listWidget->count() - 1;
    if (hasLayout && m_listWidget->verticalScrollBar()) {
        const int itemH = std::max(1, m_listWidget->gridSize().height());
        const int itemW = std::max(1, m_listWidget->gridSize().width());
        const int cols = std::max(1, vpRect.width() / itemW);
        const int scrollY = m_listWidget->verticalScrollBar()->value();
        const int startRow = std::max(0, (scrollY - 250) / itemH);
        const int endRow = (scrollY + vpRect.height() + 250) / itemH + 1;
        startIndex = std::clamp(startRow * cols, 0, m_listWidget->count() - 1);
        endIndex = std::clamp(endRow * cols + cols, startIndex, m_listWidget->count() - 1);
    }

    for (int i = startIndex; i <= endIndex; ++i) {
        auto* it = m_listWidget->item(i);
        if (!it) {
            continue;
        }
        const QRect rect = m_listWidget->visualItemRect(it);
        if (!hasLayout || rect.intersects(prefetchRect)) {
            const int srcIdx = it->data(Qt::UserRole).toInt();
            if (m_rawThumbnails.find(srcIdx) == m_rawThumbnails.end() &&
                m_requestedThumbnails.insert(srcIdx).second) {
                m_viewer->requestUnrotatedThumbnail(srcIdx, 128);
            }
        }
    }
}

void OrganizePagesDialog::onThumbnailReady(int pageIndex, const QImage& image) {
    if (m_requestedThumbnails.find(pageIndex) == m_requestedThumbnails.end()) {
        return;
    }
    m_requestedThumbnails.erase(pageIndex);
    m_rawThumbnails[pageIndex] = image;

    const auto& specs = m_plan.pageSpecs();
    for (int i = 0; i < m_listWidget->count() && i < static_cast<int>(specs.size()); ++i) {
        if (specs[static_cast<size_t>(i)].sourceIndex == pageIndex) {
            updateItemVisual(m_listWidget->item(i), specs[static_cast<size_t>(i)]);
        }
    }
}

void OrganizePagesDialog::onThumbnailFailed(int pageIndex) {
    m_requestedThumbnails.erase(pageIndex);
}

} // namespace ui
