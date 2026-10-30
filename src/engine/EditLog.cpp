#include "engine/EditLog.h"
#include "ipc/WorkerClient.h"

namespace pdf {

EditLog::EditLog(QObject* parent)
    : QObject(parent) {
}

uint64_t EditLog::recordAddAnnotation(int pageIndex, const AnnotationItem& item) {
    if (m_cursor < m_commands.size()) {
        if (m_savedCursor > m_cursor) {
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
    Q_EMIT dirtyChanged(false);
}

void EditLog::clear() {
    m_commands.clear();
    m_cursor = 0;
    m_savedCursor = 0;
    m_savedValid = true;
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

bool EditLog::replay(ipc::WorkerClient* client, int64_t documentId) {
    if (!client) {
        return false;
    }
    const auto active = activeCommands();
    for (const auto& cmd : active) {
        if (cmd.type == EditCommandType::AddAnnotation) {
            auto resp = client->addAnnotationSync(documentId, cmd.pageIndex, cmd.after);
            if (!resp.success) {
                return false;
            }
        }
    }
    return true;
}

} // namespace pdf
