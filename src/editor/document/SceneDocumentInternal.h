#pragma once

/**
 * @file SceneDocumentInternal.h
 * @brief Private shared implementation types for the scene document command units.
 */

#include "editor/document/SceneDocumentInternalOperations.h"

namespace Horo::Editor {
    /** @brief Prepares and atomically replays generated-object history through the canonical owner. */
    class SceneDocumentHistoryReplay final {
    public:
        /** @brief Replays one exact generated history entry without publishing partial document/history state.
         * @param document Canonical owner. @param history Its sole history. @param redo Replay direction.
         * @return Committed metadata or original zero-mutation validation failure. */
        [[nodiscard]] static Result<SceneCommandResult> Execute(SceneDocument &document, EditorHistory &history, bool redo);
    };

    struct SceneDocumentCommandExecutor::PrefabCommitContext final {
        SceneDocumentDetail::SceneCommandDelta delta;
        Prefab::PrefabInstanceId instance;
        DocumentChangeKind kind;
        bool advanceInstanceId{};
    };

    struct SceneDocumentCommandExecutor::ObjectCommitContext final {
        SceneDocumentDetail::SceneCommandDelta delta;
        SceneObjectId object;
        DocumentChangeKind kind;
    };

    struct EditorHistory::Impl {
        Impl() {
            undo.reserve(SceneDocumentDetail::kMaximumHistoryEntries);
            redo.reserve(SceneDocumentDetail::kMaximumHistoryEntries);
        }

        std::vector<SceneDocumentDetail::HistoryRecord> undo;
        std::vector<SceneDocumentDetail::HistoryRecord> redo;
        std::size_t memoryBytes{0};
    };
}  // namespace Horo::Editor
