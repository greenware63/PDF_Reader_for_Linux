#pragma once

#include <QListView>
#include <QAbstractListModel>
#include <QPointer>

namespace ui {

class PdfViewerWidget;

/**
 * @brief Sidebar list of the current document's search hits (page label + snippet with the hit in bold).
 *
 * It is a pure view over PdfViewerWidget::searchMatches(): nothing is copied, the model re-reads the
 * viewer's list and resets whenever the list changes. Clicking/activating a row jumps to that hit
 * (PdfViewerWidget::goToSearchMatch) and the row of the viewer's current hit is kept selected.
 */
class SearchResultsModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { PageLabelRole = Qt::UserRole + 1, SnippetStartRole, SnippetLengthRole };

    explicit SearchResultsModel(QObject* parent = nullptr);
    void setViewer(PdfViewerWidget* viewer);
    void refresh();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

private:
    QPointer<PdfViewerWidget> m_viewer;
    int m_rows{0};
};

class SearchResultsWidget : public QListView {
    Q_OBJECT
public:
    explicit SearchResultsWidget(QWidget* parent = nullptr);

    /// Binds the list to a viewer (null unbinds). Switching tabs calls this with the new current viewer.
    void setViewer(PdfViewerWidget* viewer);
    PdfViewerWidget* viewer() const noexcept { return m_viewer; }
    int resultCount() const { return m_model->rowCount(); }

Q_SIGNALS:
    /// Emitted when the number of listed hits changes (for a tab title like "Search (12)").
    void resultCountChanged(int count);

private Q_SLOTS:
    void onSearchResultsChanged(int currentMatch, int totalMatches);
    void onActivated(const QModelIndex& index);

private:
    SearchResultsModel* m_model{nullptr};
    QPointer<PdfViewerWidget> m_viewer;
    int m_lastCount{-1};
};

} // namespace ui
