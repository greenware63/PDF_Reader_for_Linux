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
    ModifyAnnotation,
    SetFormFieldText,
    ToggleFormField,
    SetFormFieldChoice
};

struct EditCommand {
    uint64_t id{0};
    int pageIndex{0};
    EditCommandType type{EditCommandType::AddAnnotation};

    // For Annotation commands
    AnnotationItem before;
    AnnotationItem after;

    // For FormField commands
    QString fieldName;
    int controlIndex{0};
    QString textBefore;
    QString textAfter;
    std::vector<AnnotationPathElement> vectorOutlines;
    bool checkedBefore{false};
    bool checkedAfter{false};
    int priorCheckedControlIndex{-1};
    std::vector<int> choiceBefore;
    std::vector<int> choiceAfter;
};

/// One operation to apply to a worker document so that it matches the log's current state.
struct EditOp {
    enum class Kind {
        AddAnnotation,
        RemoveAnnotation,
        SetFormFieldText,
        ToggleFormField,
        SetFormFieldChoice
    };
    Kind kind{Kind::AddAnnotation};
    bool add{true};          // backward-compat: true for add annotation, false for remove
    int pageIndex{0};
    AnnotationItem item;
    QString name;

    // Form fields
    QString fieldName;
    int controlIndex{0};
    QString text;
    std::vector<AnnotationPathElement> vectorOutlines;
    bool checked{false};
    std::vector<int> selectedIndices;
    int priorCheckedControlIndex{-1};
};

class EditLog : public QObject {
    Q_OBJECT

public:
    explicit EditLog(QObject* parent = nullptr);
    ~EditLog() override = default;

    uint64_t recordAddAnnotation(int pageIndex, const AnnotationItem& item);
    uint64_t recordDeleteAnnotation(int pageIndex, const AnnotationItem& item);
    uint64_t recordModifyAnnotation(int pageIndex, const AnnotationItem& before, const AnnotationItem& after);

    uint64_t recordSetFormFieldText(int pageIndex, const QString& fieldName, int controlIndex,
                                    const QString& before, const QString& after,
                                    const std::vector<AnnotationPathElement>& vectorOutlines = {});
    uint64_t recordToggleFormField(int pageIndex, const QString& fieldName, int controlIndex,
                                   bool before, bool after, int priorCheckedControlIndex = -1);
    uint64_t recordSetFormFieldChoice(int pageIndex, const QString& fieldName, int controlIndex,
                                      const std::vector<int>& before, const std::vector<int>& after);

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
