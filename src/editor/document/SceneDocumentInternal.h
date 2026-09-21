#pragma once

/**
 * @file SceneDocumentInternal.h
 * @brief Private shared implementation types for the scene document command units.
 */

#include "editor/document/SceneDocumentInternalOperations.h"

namespace Horo::Editor {
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
