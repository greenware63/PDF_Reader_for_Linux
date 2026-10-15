#include "FindBar.h"
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QToolButton>
#include <QCheckBox>
#include <QKeyEvent>

namespace ui {

FindBar::FindBar(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Find in document\u2026"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->installEventFilter(this);
    layout->addWidget(m_searchEdit, 1);

    m_matchCountLabel = new QLabel(this);
    m_matchCountLabel->setStyleSheet(QStringLiteral("color: #666; font-size: 12px; min-width: 55px;"));
    layout->addWidget(m_matchCountLabel);

    m_prevButton = new QToolButton(this);
    m_prevButton->setText(QStringLiteral("\u25c0"));
    m_prevButton->setToolTip(tr("Previous match (Shift+Enter)"));
    m_prevButton->setEnabled(false);
    layout->addWidget(m_prevButton);

    m_nextButton = new QToolButton(this);
    m_nextButton->setText(QStringLiteral("\u25b6"));
    m_nextButton->setToolTip(tr("Next match (Enter)"));
    m_nextButton->setEnabled(false);
    layout->addWidget(m_nextButton);

    m_caseSensitiveCheckBox = new QCheckBox(tr("Match Case"), this);
    m_caseSensitiveCheckBox->setChecked(false); // Case-insensitive matching by default
    layout->addWidget(m_caseSensitiveCheckBox);

    m_closeButton = new QToolButton(this);
    m_closeButton->setText(QStringLiteral("\u2715"));
    m_closeButton->setToolTip(tr("Close (Esc)"));
    layout->addWidget(m_closeButton);

    setStyleSheet(QStringLiteral(
        "ui--FindBar { background: #f4f5f7; border-bottom: 1px solid #dcdcdc; }"
        "QToolButton { border: 1px solid transparent; border-radius: 3px; padding: 3px 6px; }"
        "QToolButton:hover { background: #e2e4e8; border-color: #c0c4cc; }"
        "QToolButton:pressed { background: #d0d3d8; }"
    ));

    connect(m_searchEdit, &QLineEdit::textChanged, this, &FindBar::onSearchTextChanged);
    connect(m_caseSensitiveCheckBox, &QCheckBox::toggled, this, &FindBar::onCaseSensitivityToggled);
    connect(m_prevButton, &QToolButton::clicked, this, &FindBar::findPreviousRequested);
    connect(m_nextButton, &QToolButton::clicked, this, &FindBar::findNextRequested);
    connect(m_closeButton, &QToolButton::clicked, this, [this]() {
        emit closed();
        hide();
    });
}

void FindBar::focusSearch() {
    show();
    m_searchEdit->setFocus();
    m_searchEdit->selectAll();
}

void FindBar::setMatchCount(int currentMatch, int totalMatches) {
    if (m_searchEdit->text().trimmed().isEmpty()) {
        m_matchCountLabel->setText(QString());
        m_prevButton->setEnabled(false);
        m_nextButton->setEnabled(false);
        return;
    }

    if (totalMatches <= 0) {
        m_matchCountLabel->setText(tr("0 of 0"));
        m_prevButton->setEnabled(false);
        m_nextButton->setEnabled(false);
    } else {
        m_matchCountLabel->setText(tr("%1 of %2").arg(currentMatch).arg(totalMatches));
        m_prevButton->setEnabled(true);
        m_nextButton->setEnabled(true);
    }
}

void FindBar::clear() {
    m_searchEdit->clear();
    m_matchCountLabel->clear();
    m_prevButton->setEnabled(false);
    m_nextButton->setEnabled(false);
}

QString FindBar::query() const {
    return m_searchEdit->text();
}

bool FindBar::matchCase() const {
    return m_caseSensitiveCheckBox->isChecked();
}

void FindBar::onSearchTextChanged(const QString& text) {
    if (text.trimmed().isEmpty()) {
        m_matchCountLabel->clear();
        m_prevButton->setEnabled(false);
        m_nextButton->setEnabled(false);
    }
    emit searchRequested(text, m_caseSensitiveCheckBox->isChecked());
}

void FindBar::onCaseSensitivityToggled(bool checked) {
    emit searchRequested(m_searchEdit->text(), checked);
}

bool FindBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_searchEdit && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            emit closed();
            hide();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (keyEvent->modifiers() & Qt::ShiftModifier) {
                emit findPreviousRequested();
            } else {
                emit findNextRequested();
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FindBar::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit closed();
        hide();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace ui
