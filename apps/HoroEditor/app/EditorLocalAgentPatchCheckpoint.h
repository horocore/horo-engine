#pragma once

/** @file EditorLocalAgentPatchCheckpoint.h @brief Project-local recovery of inert source review state. */
#include "EditorLocalAgentPatch.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Platform.h"

namespace Horo::Editor {
    /** @brief Copies only the immutable proposal, review decisions and policy path identities to the I/O lane. */
    struct EditorPatchCheckpoint final {
        Proposals::ChangeSet proposal;
        Proposals::ReviewDecisions decisions;
        std::vector<SourceDocumentId> offeredPaths;
    };

    /** @brief Durably replaces the project-local recovery checkpoint with the reviewed proposal and its decisions.
     * @return Success when the replacement is durable, otherwise the storage or encoding error.
     */
    [[nodiscard]] Result<void> SaveEditorPatchCheckpoint(DurableFileSystem &files, const std::filesystem::path &root,
                                                         const EditorPatchCheckpoint &checkpoint, const CancellationToken &stop);

    /** @brief Recovers one proposal and its decisions after rechecking the current project's open source snapshots.
     * @return No value when no checkpoint exists, otherwise the recovered review or a typed failure.
     */
    [[nodiscard]] Result<std::optional<EditorLocalAgentPatchReview>> LoadEditorPatchCheckpoint(
        const std::filesystem::path &root, std::string_view project, std::span<const SourceDocumentSnapshot> sources,
        const CancellationToken &stop, const DurableFileSystem *files = nullptr);
}  // namespace Horo::Editor
