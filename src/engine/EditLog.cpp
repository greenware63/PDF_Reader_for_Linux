#include "engine/EditLog.h"
#include "ipc/WorkerClient.h"
#include <algorithm>

namespace pdf {

EditLog::EditLog(QObject* parent)
    : QObject(parent) {
}

uint64_t EditLog::recordAddAnnotation(int pageIndex, const AnnotationItem& item) {
    if (m_cursor < m_commands.size()) {
        const size_t savedBase = m_savedValid ? m_savedCursor : m_savedPrefix;
        if (savedBase > m_cursor) {
            // Commands the saved file contains are about to be discarded: remember them so recovery can
            // remove them again, and so the saved state is never mistaken for the current one.
            for (size_t i = m_cursor; i < savedBase && i < m_commands.size(); ++i) {
                m_savedExtra.push_back(m_commands[i]);
            }
            m_savedPrefix = m_cursor;
            m_savedValid = false;
        }
        m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(m_cursor), m_commands.end());
    }

    EditCommand cmd;
    cmd.id = m_nextCommandId++;
    cmd.pageIndex = pageIndex;
    cmd.type = EditCommandType::AddAnnotation;
    cmd.after = item;

    m_commands.push_back(std::move(cmd));
    m_cursor = m_commands.size();

    Q_EMIT stateChanged();
    Q_EMIT dirtyChanged(isDirty());

    return cmd.id;
}

uint64_t EditLog::recordDeleteAnnotation(int pageIndex, const AnnotationItem& item) {
    if (m_cursor < m_commands.size()) {
        const size_t savedBase = m_savedValid ? m_savedCursor : m_savedPrefix;
        if (savedBase > m_cursor) {
            for (size_t i = m_cursor; i < savedBase && i < m_commands.size(); ++i) {
                m_savedExtra.push_back(m_commands[i]);
            }
            m_savedPrefix = m_cursor;
            m_savedValid = false;
        }
        m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(m_cursor), m_commands.end());
    }

    EditCommand cmd;
    cmd.id = m_nextCommandId++;
    cmd.pageIndex = pageIndex;
    cmd.type = EditCommandType::DeleteAnnotation;
    cmd.before = item;

    m_commands.push_back(std::move(cmd));
    m_cursor = m_commands.size();

    Q_EMIT stateChanged();
    Q_EMIT dirtyChanged(isDirty());

    return cmd.id;
}

uint64_t EditLog::recordModifyAnnotation(int pageIndex, const AnnotationItem& before, const AnnotationItem& after) {
    if (m_cursor < m_commands.size()) {
        const size_t savedBase = m_savedValid ? m_savedCursor : m_savedPrefix;
        if (savedBase > m_cursor) {
            for (size_t i = m_cursor; i < savedBase && i < m_commands.size(); ++i) {
                m_savedExtra.push_back(m_commands[i]);
            }
            m_savedPrefix = m_cursor;
            m_savedValid = false;
        }
        m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(m_cursor), m_commands.end());
    }

    EditCommand cmd;
    cmd.id = m_nextCommandId++;
    cmd.pageIndex = pageIndex;
    cmd.type = EditCommandType::ModifyAnnotation;
    cmd.before = before;
    cmd.after = after;

    m_commands.push_back(std::move(cmd));
    m_cursor = m_commands.size();

    Q_EMIT stateChanged();
    Q_EMIT dirtyChanged(isDirty());

    return cmd.id;
}

uint64_t EditLog::recordSetFormFieldText(int pageIndex, const QString& fieldName, int controlIndex,
                                         const QString& before, const QString& after,
                                         const std::vector<AnnotationPathElement>& vectorOutlines) {
    if (m_cursor < m_commands.size()) {
        const size_t savedBase = m_savedValid ? m_savedCursor : m_savedPrefix;
        if (savedBase > m_cursor) {
            for (size_t i = m_cursor; i < savedBase && i < m_commands.size(); ++i) {
                m_savedExtra.push_back(m_commands[i]);
            }
            m_savedPrefix = m_cursor;
            m_savedValid = false;
        }
        m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(m_cursor), m_commands.end());
    }

    EditCommand cmd;
    cmd.id = m_nextCommandId++;
    cmd.pageIndex = pageIndex;
    cmd.type = EditCommandType::SetFormFieldText;
    cmd.fieldName = fieldName;
    cmd.controlIndex = controlIndex;
    cmd.textBefore = before;
    cmd.textAfter = after;
    cmd.vectorOutlines = vectorOutlines;

    m_commands.push_back(std::move(cmd));
    m_cursor = m_commands.size();

    Q_EMIT stateChanged();
    Q_EMIT dirtyChanged(isDirty());

    return cmd.id;
}

uint64_t EditLog::recordToggleFormField(int pageIndex, const QString& fieldName, int controlIndex,
                                        bool before, bool after, int priorCheckedControlIndex) {
    if (m_cursor < m_commands.size()) {
        const size_t savedBase = m_savedValid ? m_savedCursor : m_savedPrefix;
        if (savedBase > m_cursor) {
            for (size_t i = m_cursor; i < savedBase && i < m_commands.size(); ++i) {
                m_savedExtra.push_back(m_commands[i]);
            }
            m_savedPrefix = m_cursor;
            m_savedValid = false;
        }
        m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(m_cursor), m_commands.end());
    }

    EditCommand cmd;
    cmd.id = m_nextCommandId++;
    cmd.pageIndex = pageIndex;
    cmd.type = EditCommandType::ToggleFormField;
    cmd.fieldName = fieldName;
    cmd.controlIndex = controlIndex;
    cmd.checkedBefore = before;
    cmd.checkedAfter = after;
    cmd.priorCheckedControlIndex = priorCheckedControlIndex;

    m_commands.push_back(std::move(cmd));
    m_cursor = m_commands.size();

    Q_EMIT stateChanged();
    Q_EMIT dirtyChanged(isDirty());

    return cmd.id;
}

uint64_t EditLog::recordSetFormFieldChoice(int pageIndex, const QString& fieldName, int controlIndex,
                                           const std::vector<int>& before, const std::vector<int>& after) {
    if (m_cursor < m_commands.size()) {
        const size_t savedBase = m_savedValid ? m_savedCursor : m_savedPrefix;
        if (savedBase > m_cursor) {
            for (size_t i = m_cursor; i < savedBase && i < m_commands.size(); ++i) {
                m_savedExtra.push_back(m_commands[i]);
            }
            m_savedPrefix = m_cursor;
            m_savedValid = false;
        }
        m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(m_cursor), m_commands.end());
    }

    EditCommand cmd;
    cmd.id = m_nextCommandId++;
    cmd.pageIndex = pageIndex;
    cmd.type = EditCommandType::SetFormFieldChoice;
    cmd.fieldName = fieldName;
    cmd.controlIndex = controlIndex;
    cmd.choiceBefore = before;
    cmd.choiceAfter = after;

    m_commands.push_back(std::move(cmd));
    m_cursor = m_commands.size();

    Q_EMIT stateChanged();
    Q_EMIT dirtyChanged(isDirty());

    return cmd.id;
}

bool EditLog::canUndo() const {
    return m_cursor > 0;
}

bool EditLog::canRedo() const {
    return m_cursor < m_commands.size();
}

bool EditLog::isDirty() const {
    if (!m_savedValid) {
        return true;
    }
    return m_cursor != m_savedCursor;
}

bool EditLog::undo(EditCommand& outCommand) {
    if (!canUndo()) {
        return false;
    }
    m_cursor--;
    outCommand = m_commands[m_cursor];

    Q_EMIT stateChanged();
    Q_EMIT dirtyChanged(isDirty());
    return true;
}

bool EditLog::redo(EditCommand& outCommand) {
    if (!canRedo()) {
        return false;
    }
    outCommand = m_commands[m_cursor];
    m_cursor++;

    Q_EMIT stateChanged();
    Q_EMIT dirtyChanged(isDirty());
    return true;
}

void EditLog::markSaved() {
    m_savedCursor = m_cursor;
    m_savedValid = true;
    m_savedPrefix = 0;
    m_savedExtra.clear();
    Q_EMIT dirtyChanged(false);
}

void EditLog::clear() {
    m_commands.clear();
    m_cursor = 0;
    m_savedCursor = 0;
    m_savedValid = true;
    m_savedPrefix = 0;
    m_savedExtra.clear();
    Q_EMIT stateChanged();
    Q_EMIT dirtyChanged(false);
}

std::vector<EditCommand> EditLog::activeCommands() const {
    if (m_cursor == 0) {
        return {};
    }
    return std::vector<EditCommand>(m_commands.begin(),
                                    m_commands.begin() + static_cast<std::ptrdiff_t>(m_cursor));
}

std::vector<EditOp> EditLog::opsSinceSave() const {
    std::vector<EditOp> ops;
    // Saved items that are no longer part of the log.
    for (auto it = m_savedExtra.rbegin(); it != m_savedExtra.rend(); ++it) {
        if (it->type == EditCommandType::AddAnnotation) {
            ops.push_back({EditOp::Kind::RemoveAnnotation, false, it->pageIndex, {}, it->after.name, {}, 0, {}, {}, false, {}, -1});
        } else if (it->type == EditCommandType::DeleteAnnotation) {
            ops.push_back({EditOp::Kind::AddAnnotation, true, it->pageIndex, it->before, it->before.name, {}, 0, {}, {}, false, {}, -1});
        } else if (it->type == EditCommandType::ModifyAnnotation) {
            ops.push_back({EditOp::Kind::RemoveAnnotation, false, it->pageIndex, {}, it->after.name, {}, 0, {}, {}, false, {}, -1});
            ops.push_back({EditOp::Kind::AddAnnotation, true, it->pageIndex, it->before, it->before.name, {}, 0, {}, {}, false, {}, -1});
        } else if (it->type == EditCommandType::SetFormFieldText) {
            EditOp op; op.kind = EditOp::Kind::SetFormFieldText; op.pageIndex = it->pageIndex; op.fieldName = it->fieldName;
            op.controlIndex = it->controlIndex; op.text = it->textBefore; op.vectorOutlines = it->vectorOutlines;
            ops.push_back(std::move(op));
        } else if (it->type == EditCommandType::ToggleFormField) {
            EditOp op; op.kind = EditOp::Kind::ToggleFormField; op.pageIndex = it->pageIndex; op.fieldName = it->fieldName;
            op.controlIndex = it->controlIndex; op.checked = it->checkedBefore;
            ops.push_back(std::move(op));
        } else if (it->type == EditCommandType::SetFormFieldChoice) {
            EditOp op; op.kind = EditOp::Kind::SetFormFieldChoice; op.pageIndex = it->pageIndex; op.fieldName = it->fieldName;
            op.controlIndex = it->controlIndex; op.selectedIndices = it->choiceBefore;
            ops.push_back(std::move(op));
        }
    }
    const size_t base = m_savedValid ? m_savedCursor : m_savedPrefix;
    if (m_cursor > base) {
        for (size_t i = base; i < m_cursor && i < m_commands.size(); ++i) {
            const auto& cmd = m_commands[i];
            if (cmd.type == EditCommandType::AddAnnotation) {
                ops.push_back({EditOp::Kind::AddAnnotation, true, cmd.pageIndex, cmd.after, cmd.after.name, {}, 0, {}, {}, false, {}, -1});
            } else if (cmd.type == EditCommandType::DeleteAnnotation) {
                ops.push_back({EditOp::Kind::RemoveAnnotation, false, cmd.pageIndex, {}, cmd.before.name, {}, 0, {}, {}, false, {}, -1});
            } else if (cmd.type == EditCommandType::ModifyAnnotation) {
                ops.push_back({EditOp::Kind::RemoveAnnotation, false, cmd.pageIndex, {}, cmd.before.name, {}, 0, {}, {}, false, {}, -1});
                ops.push_back({EditOp::Kind::AddAnnotation, true, cmd.pageIndex, cmd.after, cmd.after.name, {}, 0, {}, {}, false, {}, -1});
            } else if (cmd.type == EditCommandType::SetFormFieldText) {
                EditOp op; op.kind = EditOp::Kind::SetFormFieldText; op.pageIndex = cmd.pageIndex; op.fieldName = cmd.fieldName;
                op.controlIndex = cmd.controlIndex; op.text = cmd.textAfter; op.vectorOutlines = cmd.vectorOutlines;
                ops.push_back(std::move(op));
            } else if (cmd.type == EditCommandType::ToggleFormField) {
                EditOp op; op.kind = EditOp::Kind::ToggleFormField; op.pageIndex = cmd.pageIndex; op.fieldName = cmd.fieldName;
                op.controlIndex = cmd.controlIndex; op.checked = cmd.checkedAfter;
                ops.push_back(std::move(op));
            } else if (cmd.type == EditCommandType::SetFormFieldChoice) {
                EditOp op; op.kind = EditOp::Kind::SetFormFieldChoice; op.pageIndex = cmd.pageIndex; op.fieldName = cmd.fieldName;
                op.controlIndex = cmd.controlIndex; op.selectedIndices = cmd.choiceAfter;
                ops.push_back(std::move(op));
            }
        }
    } else if (m_cursor < base) {
        for (size_t i = std::min(base, m_commands.size()); i > m_cursor; --i) {
            const auto& cmd = m_commands[i - 1];
            if (cmd.type == EditCommandType::AddAnnotation) {
                ops.push_back({EditOp::Kind::RemoveAnnotation, false, cmd.pageIndex, {}, cmd.after.name, {}, 0, {}, {}, false, {}, -1});
            } else if (cmd.type == EditCommandType::DeleteAnnotation) {
                ops.push_back({EditOp::Kind::AddAnnotation, true, cmd.pageIndex, cmd.before, cmd.before.name, {}, 0, {}, {}, false, {}, -1});
            } else if (cmd.type == EditCommandType::ModifyAnnotation) {
                ops.push_back({EditOp::Kind::RemoveAnnotation, false, cmd.pageIndex, {}, cmd.after.name, {}, 0, {}, {}, false, {}, -1});
                ops.push_back({EditOp::Kind::AddAnnotation, true, cmd.pageIndex, cmd.before, cmd.before.name, {}, 0, {}, {}, false, {}, -1});
            } else if (cmd.type == EditCommandType::SetFormFieldText) {
                EditOp op; op.kind = EditOp::Kind::SetFormFieldText; op.pageIndex = cmd.pageIndex; op.fieldName = cmd.fieldName;
                op.controlIndex = cmd.controlIndex; op.text = cmd.textBefore; op.vectorOutlines = cmd.vectorOutlines;
                ops.push_back(std::move(op));
            } else if (cmd.type == EditCommandType::ToggleFormField) {
                EditOp op; op.kind = EditOp::Kind::ToggleFormField; op.pageIndex = cmd.pageIndex; op.fieldName = cmd.fieldName;
                if (cmd.priorCheckedControlIndex >= 0 && !cmd.checkedBefore && cmd.checkedAfter) {
                    op.controlIndex = cmd.priorCheckedControlIndex;
                    op.checked = true;
                } else {
                    op.controlIndex = cmd.controlIndex;
                    op.checked = cmd.checkedBefore;
                }
                ops.push_back(std::move(op));
            } else if (cmd.type == EditCommandType::SetFormFieldChoice) {
                EditOp op; op.kind = EditOp::Kind::SetFormFieldChoice; op.pageIndex = cmd.pageIndex; op.fieldName = cmd.fieldName;
                op.controlIndex = cmd.controlIndex; op.selectedIndices = cmd.choiceBefore;
                ops.push_back(std::move(op));
            }
        }
    }
    return ops;
}

bool EditLog::replay(ipc::WorkerClient* client, int64_t documentId) {
    if (!client) {
        return false;
    }
    for (const auto& op : opsSinceSave()) {
        switch (op.kind) {
            case EditOp::Kind::AddAnnotation:
                if (!client->addAnnotationSync(documentId, op.pageIndex, op.item).success) {
                    return false;
                }
                break;
            case EditOp::Kind::RemoveAnnotation:
                if (!client->removeAnnotationSync(documentId, op.pageIndex, op.name).success) {
                    return false;
                }
                break;
            case EditOp::Kind::SetFormFieldText:
                if (!client->setFormFieldTextSync(documentId, op.pageIndex, op.fieldName, op.controlIndex, op.text, op.vectorOutlines).success) {
                    return false;
                }
                break;
            case EditOp::Kind::ToggleFormField:
                if (!client->toggleFormFieldSync(documentId, op.pageIndex, op.fieldName, op.controlIndex, op.checked).success) {
                    return false;
                }
                break;
            case EditOp::Kind::SetFormFieldChoice:
                if (!client->setFormFieldChoiceSync(documentId, op.pageIndex, op.fieldName, op.controlIndex, op.selectedIndices).success) {
                    return false;
                }
                break;
        }
    }
    return true;
}

} // namespace pdf
