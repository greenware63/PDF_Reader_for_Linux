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

/// One operation to apply to a worker document so that it matches the log's current state.
struct EditOp {
    bool add{true};          // true: add `item`; false: remove `name`
    int pageIndex{0};
    AnnotationItem item;
    QString name;
};

class EditLog : public QObject {
    Q_OBJECT

public:
    explicit EditLog(QObject* parent = nullptr);
    ~EditLog() override = default;

    uint64_t recordAddAnnotation(int pageIndex, const AnnotationItem& item);
    uint64_t recordDeleteAnnotation(int pageIndex, const AnnotationItem& item);
    uint64_t recordModifyAnnotation(int pageIndex, const AnnotationItem& before, const AnnotationItem& after);

    bool canUndo() const;
    bool canRedo() const;
    bool isDirty() const;

    bool undo(EditCommand& outCommand);
    bool redo(EditCommand& outCommand);

    void markSaved();
    void clear();

    std::vector<EditCommand> activeCommands() const;

    /**
     * @brief Operations that turn a worker document holding the last *saved* state into the current state.
     * Used after a worker crash: reopen the file on disk (the saved state) and apply these. Annotations already
     * saved are not added again; saved annotations that were undone (or discarded by a new edit after an undo)
     * are removed.
     */
    std::vector<EditOp> opsSinceSave() const;

    /// Applies opsSinceSave() to `documentId` using synchronous requests. Returns false on the first failure.
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
    size_t m_savedPrefix{0};                 // when !m_savedValid: commands[0..m_savedPrefix) are in the saved file
    std::vector<EditCommand> m_savedExtra;   // when !m_savedValid: saved-file annotations no longer in the log
};

} // namespace pdf
