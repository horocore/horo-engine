#pragma once

/** @file SceneBake.h
 * @brief Headless document integration that commits exact PCG output semantics through canonical Scene history.
 */
#include "Horo/Editor/PCGGraphDocument.h"
#include "Horo/Editor/SceneDocumentTransaction.h"
#include "Horo/PCGScene/SceneOutputAdapter.h"

namespace Horo::PCG {
    /** @brief Exact replacement scope; logical outputs are unique only within this lineage/set/cell. */
    struct PCGSceneBakeScope final {
        GenerationLineageId lineage;
        GeneratedSetId set;
        GenerationCellId cell;
    };

    /** @brief Explicit target-owner binding; creation of this inert value grants no edit capability. */
    struct PCGSceneBakeBinding final {
        GenerationTargetOwnerId owner;
        std::uint64_t capabilityGeneration{};
        PCGCapabilitySet capabilities;
        GenerationDependencyId graphSource; /**< Host-assigned dependency slot bound to the exact working graph digest/revision. */
        PCGSceneBakeScope scope;
    };

    /** @brief Copied target-issued evidence for plan/adapter admission; contains no writable document pointers. */
    struct PCGSceneBakeTarget final {
        Editor::SceneTransactionSnapshot authored;
        PCGGenerationTargetReceipt receipt;
        std::vector<PCGOwnedGeneratedOutput> generated;
        std::vector<PCGSceneOutputMapping> mappings;
        std::vector<SceneSource::SceneObjectId> occupied;
    };

    /** @brief Exact target fence and explicit common-operation cancellation/admission envelope. */
    struct PCGSceneBakeContext final {
        PCGSceneBakeBinding binding;
        Editor::SceneTransactionContext target;
        Editor::SceneTransactionLimits limits;
        std::span<const PCGSceneMeshReceipt> currentMeshAssets; /**< Fresh owner-captured asset receipts, borrowed only until return. */
    };

    /** @brief Actual target publication and exact graph working state used; graph-save state remains independent. */
    struct PCGSceneBakeResult final {
        Editor::SceneTransactionResult target;
        Editor::DocumentSessionId graphSession;
        std::uint64_t graphRevision{};
        Sha256Digest graphState{};
        bool fromUnsaved{};
    };

    /** @brief Bind one exact unsaved/saved working graph state into the generation plan's dependency closure.
     * @param graph Captured canonical graph document. @param dependency Host-selected stable graph-input slot.
     * @return Exact dependency stamp or typed invalid/exhausted identity error.
     */
    [[nodiscard]] Result<PCGGenerationDependency> CapturePCGSceneGraphInput(const Editor::PCGDocumentSnapshot &graph,
                                                                            GenerationDependencyId dependency);
    /** @brief Capture actual target ownership, mappings and current generation/capability evidence.
     * @param target Canonical Scene/document transaction port.
     * @param binding Explicit target owner and current capability publication; generation matches the document opening.
     * @param limits Finite canonical snapshot preparation ceiling.
     * @return Owned target capture or original typed owner/lifecycle/capacity failure.
     */
    [[nodiscard]] Result<PCGSceneBakeTarget> CapturePCGSceneBakeTarget(const Editor::SceneDocumentTransactions &target,
                                                                       const PCGSceneBakeBinding &binding,
                                                                       Editor::SceneTransactionLimits limits = {});
    /** @brief Atomically commit detached PCG output through the actual Scene document and its sole semantic history.
     * @param graph Canonical graph document borrowed synchronously for current source/session validation.
     * @param capturedGraph Exact graph working snapshot used by evaluation; bake never changes graph saved/dirty state.
     * @param target Canonical Scene transaction port; owns authoritative authored/generated state and replay.
     * @param batch Exact inert adapter result; retained plan/current ownership/assets are revalidated before commit.
     * @param context Fresh target fence, permission, current asset receipts and finite reservation.
     * @return One actual Scene commit, exact already-applied no-op, or original typed zero-mutation rejection.
     * @details No node, evaluator, preview, file save or backend selection runs here. Prepare/validation failures and
     * cancellation preserve both documents. Undo/redo uses target.Replay and exact historical authored/provenance values;
     * it never reexecutes this function or a generator. Only this downstream integration imports both document owners.
     */
    [[nodiscard]] Result<PCGSceneBakeResult> BakePCGSceneOutputs(const Editor::PCGGraphDocument &graph,
                                                                 const Editor::PCGDocumentSnapshot &capturedGraph,
                                                                 Editor::SceneDocumentTransactions &target,
                                                                 const PCGSceneOutputBatch &batch, const PCGSceneBakeContext &context);
}  // namespace Horo::PCG
