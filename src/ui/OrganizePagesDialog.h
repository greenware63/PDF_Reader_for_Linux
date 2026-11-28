#pragma once

#include "PageOrganizePlan.h"
#include <QDialog>
#include <QImage>
#include <QPointer>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QDialogButtonBox;

namespace ui {

class PdfViewerWidget;

/**
 * @brief Modal dialog for staging page reorder, delete, and rotation operations.
 * Requests unrotated thumbnails lazily only for visible items and applies rotation via QTransform.
 */
class OrganizePagesDialog : public QDialog {
    Q_OBJECT

public:
    explicit OrganizePagesDialog(PdfViewerWidget* viewer, QWidget* parent = nullptr);
    ~OrganizePagesDialog() override;

    const std::vector<pdf::PageSpec>& pageSpecs() const noexcept { return m_plan.pageSpecs(); }
    const PageOrganizePlan& plan() const noexcept { return m_plan; }

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
    void requestVisibleThumbnails();
    void onThumbnailReady(int pageIndex, const QImage& image);
    void onThumbnailFailed(int pageIndex);
    void onRotateClockwise();
    void onRotateCounterClockwise();
    void onDeleteSelected();
    void onResetPlan();
    void onRowsMoved();
    void updateControls();

private:
    void rebuildListFromPlan();
    void updateItemVisual(QListWidgetItem* item, const pdf::PageSpec& spec);
    std::vector<int> selectedRowsDescending() const;
    std::vector<int> selectedRowsAscending() const;

    QPointer<PdfViewerWidget> m_viewer;
    PageOrganizePlan m_plan;
    QListWidget* m_listWidget{nullptr};
    QPushButton* m_rotateCcwBtn{nullptr};
    QPushButton* m_rotateCwBtn{nullptr};
    QPushButton* m_deleteBtn{nullptr};
    QPushButton* m_resetBtn{nullptr};
    QDialogButtonBox* m_buttonBox{nullptr};

    std::unordered_map<int, QImage> m_rawThumbnails;
    std::unordered_set<int> m_requestedThumbnails;
};

} // namespace ui
