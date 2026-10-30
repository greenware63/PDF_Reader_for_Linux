#pragma once

#include <QObject>
#include <vector>
#include <cstdint>
#include "engine/AnnotationTypes.h"

namespace pdf::ipc {
class WorkerClient;
}

namespace pdf {

enum class EditCommandType {
    AddAnnotation,
    DeleteAnnotation,
    ModifyAnnotation
};

struct EditCommand {
    uint64_t id{0};
    int pageIndex{0};
    EditCommandType type{EditCommandType::AddAnnotation};
    AnnotationItem before;
    AnnotationItem after;
};

class EditLog : public QObject {
    Q_OBJECT

public:
    explicit EditLog(QObject* parent = nullptr);
    ~EditLog() override = default;

    uint64_t recordAddAnnotation(int pageIndex, const AnnotationItem& item);

    bool canUndo() const;
    bool canRedo() const;
    bool isDirty() const;

    bool undo(EditCommand& outCommand);
    bool redo(EditCommand& outCommand);

    void markSaved();
    void clear();

    std::vector<EditCommand> activeCommands() const;

    bool replay(ipc::WorkerClient* client, int64_t documentId);

Q_SIGNALS:
    void stateChanged();
    void dirtyChanged(bool dirty);

private:
    uint64_t m_nextCommandId{1};
    std::vector<EditCommand> m_commands;
    size_t m_cursor{0};
    size_t m_savedCursor{0};
    bool m_savedValid{true};
};

} // namespace pdf
