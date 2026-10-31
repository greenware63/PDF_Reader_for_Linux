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
    // Saved annotations that are no longer part of the log.
    for (auto it = m_savedExtra.rbegin(); it != m_savedExtra.rend(); ++it) {
        if (it->type == EditCommandType::AddAnnotation) {
            ops.push_back({false, it->pageIndex, {}, it->after.name});
        } else if (it->type == EditCommandType::DeleteAnnotation) {
            ops.push_back({true, it->pageIndex, it->before, it->before.name});
        } else if (it->type == EditCommandType::ModifyAnnotation) {
            ops.push_back({false, it->pageIndex, {}, it->after.name});
            ops.push_back({true, it->pageIndex, it->before, it->before.name});
        }
    }
    const size_t base = m_savedValid ? m_savedCursor : m_savedPrefix;
    if (m_cursor > base) {
        for (size_t i = base; i < m_cursor && i < m_commands.size(); ++i) {
            const auto& cmd = m_commands[i];
            if (cmd.type == EditCommandType::AddAnnotation) {
                ops.push_back({true, cmd.pageIndex, cmd.after, cmd.after.name});
            } else if (cmd.type == EditCommandType::DeleteAnnotation) {
                ops.push_back({false, cmd.pageIndex, {}, cmd.before.name});
            } else if (cmd.type == EditCommandType::ModifyAnnotation) {
                ops.push_back({false, cmd.pageIndex, {}, cmd.before.name});
                ops.push_back({true, cmd.pageIndex, cmd.after, cmd.after.name});
            }
        }
    } else if (m_cursor < base) {
        for (size_t i = std::min(base, m_commands.size()); i > m_cursor; --i) {
            const auto& cmd = m_commands[i - 1];
            if (cmd.type == EditCommandType::AddAnnotation) {
                ops.push_back({false, cmd.pageIndex, {}, cmd.after.name});
            } else if (cmd.type == EditCommandType::DeleteAnnotation) {
                ops.push_back({true, cmd.pageIndex, cmd.before, cmd.before.name});
            } else if (cmd.type == EditCommandType::ModifyAnnotation) {
                ops.push_back({false, cmd.pageIndex, {}, cmd.after.name});
                ops.push_back({true, cmd.pageIndex, cmd.before, cmd.before.name});
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
        if (op.add) {
            if (!client->addAnnotationSync(documentId, op.pageIndex, op.item).success) {
                return false;
            }
        } else {
            if (!client->removeAnnotationSync(documentId, op.pageIndex, op.name).success) {
                return false;
            }
        }
    }
    return true;
}

} // namespace pdf
