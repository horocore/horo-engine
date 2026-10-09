#pragma once

/** @file WorldBuildValidation.h
 * @brief Bounded offline world-cell validation and host-published build diagnostics.
 */
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

namespace Horo::Runtime {
    /** @brief One exact authored cell publication and its optional navigable source. */
    struct WorldBuildCell final {
        SceneCellPayloadIdentity expected;
        std::optional<DiagnosticSourceLocation> source;
    };

    /** @brief Exact authored object endpoint; object identities are scoped to the cell scene. */
    struct WorldBuildEndpoint final {
        SceneCellPayloadIdentity cell;
        SceneObjectId object;
    };

    /** @brief A captured required object reference; optional deferred references stay with WorldDependencyPlan. */
    struct WorldBuildReference final {
        WorldBuildEndpoint source;
        WorldBuildEndpoint target;
        std::optional<DiagnosticSourceLocation> location;
    };

    /** @brief Mandatory count, source-byte and logical payload estimate ceilings. */
    struct WorldBuildValidationLimits final {
        std::size_t maximumCells{};
        std::size_t maximumReferences{};
        std::size_t maximumObjects{}; /**< Aggregate cooked object count bounds reference traversal. */
        std::size_t maximumDiagnostics{};
        std::size_t maximumSourceBytes{};
        std::size_t maximumCellBytes{};
        std::size_t maximumWorldBytes{};
    };

    /** @brief Owned complete report; a successful validation call may still report build errors. */
    struct WorldBuildValidationReport final {
        std::uint64_t revision{};
        std::size_t estimatedBytes{}; /**< Scene payload logical bytes only, excluding provider/native/encoded allocations. */
        std::vector<BuildOutputRecord> diagnostics;

        /** @brief Returns whether the complete captured world has no build errors. @return True for an empty diagnostic set. */
        [[nodiscard]] bool CanPublish() const noexcept {
            return diagnostics.empty();
        }
    };

    /**
     * @brief Validates complete offline cell coverage, exact references and Scene payload byte estimates without publishing state.
     * @param partition Immutable topology authority.
     * @param revision Positive host-captured build generation, echoed for owner-thread publication fencing.
     * @param cells Complete authored set; exactly one identity for every partition cell.
     * @param payloads Cooked immutable Scene baselines; null leases represent unavailable payloads.
     * @param references Required authored references with exact source and target publication identity.
     * @param limits Positive storage ceilings; maximumReferences may be zero to prohibit references.
     * @param cancellation Borrowed observer checked throughout validation and before returning.
     * @return Complete canonical report or typed SceneCellPayloadErrors invalid/capacity/cancelled failure with no partial report.
     * @details Tooling/load-time only. All inputs are borrowed until return; the report owns source strings. No I/O, jobs,
     * store append, cache mutation or runtime activation occurs. Hosts reject obsolete report revisions before publication.
     * Source paths are optional host-provided absolute paths, never resolved by this operation. Logical byte estimates do
     * not claim encoded/native provider memory qualification. Shutdown is caller-owned cancellation and synchronous return.
     * @throws std::bad_alloc on bounded scratch/output allocation failure; inputs remain unchanged.
     */
    [[nodiscard]] Result<WorldBuildValidationReport> ValidateWorldBuild(
        const WorldStreaming::WorldPartitionDescriptor &partition, std::uint64_t revision, std::span<const WorldBuildCell> cells,
        std::span<const std::shared_ptr<const RuntimeSceneCellPayload>> payloads, std::span<const WorldBuildReference> references,
        WorldBuildValidationLimits limits, const CancellationToken &cancellation = {});
}  // namespace Horo::Runtime
