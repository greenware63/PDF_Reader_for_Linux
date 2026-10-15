#pragma once

#include <QWidget>
#include <QString>

class QLineEdit;
class QLabel;
class QToolButton;
class QCheckBox;

namespace ui {

/**
 * @brief Search bar widget supporting Ctrl+F opening, Esc dismissal, Next/Prev match navigation,
 * match counts ("3 of 17"), and case sensitivity toggling.
 */
class FindBar : public QWidget {
    Q_OBJECT

public:
    explicit FindBar(QWidget* parent = nullptr);
    ~FindBar() override = default;

    /// Gives keyboard focus to search input and selects all current text.
    void focusSearch();

    /// Updates match counter label (e.g. "3 of 17").
    void setMatchCount(int currentMatch, int totalMatches);
    void setSearchingProgress(int pagesSearched, int totalPages);
    void finishSearch(bool truncated);

    /// Clears query and resets counter.
    void clear();

    QString query() const;
    bool matchCase() const;

Q_SIGNALS:
    void searchRequested(const QString& query, bool matchCase);
    void findNextRequested();
    void findPreviousRequested();
    void closed();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
    void onSearchTextChanged(const QString& text);
    void onCaseSensitivityToggled(bool checked);

private:
    void updateStatusLabel();

    QLineEdit* m_searchEdit{nullptr};
    QLabel* m_matchCountLabel{nullptr};
    QToolButton* m_prevButton{nullptr};
    QToolButton* m_nextButton{nullptr};
    QCheckBox* m_caseSensitiveCheckBox{nullptr};
    QToolButton* m_closeButton{nullptr};
    int m_currentMatch{0};
    int m_totalMatches{0};
    int m_pagesSearched{0};
    int m_totalPages{0};
    bool m_isSearching{false};
    bool m_matchLimitReached{false};
};

} // namespace ui
