#pragma once

/** @file SceneOutputAdapter.h
 * @brief Detached typed scene commands translated from exact PCG generation plans.
 */
#include "Horo/PCG/PCGGenerationPlan.h"
#include "Horo/Scene/SceneSource.h"

namespace Horo::PCG {
    /** @brief Target-owned stable logical-to-authored mapping, never a live entity handle. */
    struct PCGSceneOutputMapping final {
        GenerationLogicalOutputId logical;
        SceneSource::SceneObjectId object;
        [[nodiscard]] auto operator<=>(const PCGSceneOutputMapping &) const noexcept = default;
    };

    /** @brief Exact immutable core.mesh dependency receipt captured by the asset owner. */
    struct PCGSceneMeshReceipt final {
        Assets::AssetId asset;
        PCGGenerationDependency dependency;
    };

    /** @brief Desired authored value for a create/update; removal has no desired payload. */
    struct PCGSceneOutputIntent final {
        GenerationLogicalOutputId logical;
        SceneSource::SceneObjectSnapshot desired;
        std::optional<PCGSceneMeshReceipt> mesh;
    };

    /** @brief Pure semantic command for a later document-owned aggregate transaction. */
    struct PCGSceneOutputCommand final {
        PCGOutputDelta delta;
        SceneSource::SceneObjectId object;
        std::optional<SceneSource::SceneObjectSnapshot> desired;
        std::optional<PCGSceneMeshReceipt> mesh;
    };

    /** @brief Current owner evidence and finite translation admission; borrowed only for the call. */
    struct PCGSceneOutputContext final {
        PCGGenerationPlanContext generation;
        std::span<const PCGSceneOutputMapping> currentMappings;
        std::span<const SceneSource::SceneObjectId> occupiedObjects; /**< Strictly identity-ordered complete occupied-ID snapshot. */
        std::size_t maximumCommands{1024};
        std::size_t maximumPayloadBytes{1024 * 1024};
        std::size_t maximumRetainedBytes{16 * 1024 * 1024};
    };

    /** @brief Immutable detached commands and their validation/resource receipt; owns no target state. */
    class PCGSceneOutputBatch final {
    public:
        /** @brief Returns the exact validated plan retained by this batch. @return Immutable plan. */
        [[nodiscard]] const PCGGenerationPlan &Plan() const noexcept {
            return plan_;
        }

        /** @brief Returns canonical logical-output-ordered commands. @return Borrowed immutable commands. */
        [[nodiscard]] std::span<const PCGSceneOutputCommand> Commands() const noexcept {
            return commands_;
        }

        /** @brief Returns the conservative adapter storage and codec-preparation charge. @return Bytes including canonical payloads and
         * command metadata. */
        [[nodiscard]] std::size_t ChargedBytes() const noexcept {
            return retainedBytes_;
        }

    private:
        friend Result<PCGSceneOutputBatch> TranslatePCGSceneOutputs(const PCGGenerationPlan &, std::span<const PCGSceneOutputIntent>,
                                                                    const PCGSceneOutputContext &);

        PCGSceneOutputBatch(PCGGenerationPlan plan, std::vector<PCGSceneOutputCommand> commands, std::size_t retainedBytes)
            : plan_(std::move(plan)), commands_(std::move(commands)), retainedBytes_(retainedBytes) {}

        PCGGenerationPlan plan_;
        std::vector<PCGSceneOutputCommand> commands_;
        std::size_t retainedBytes_{};
    };

    /** @brief Validates and fingerprints the unchanged canonical scene payload for plan construction.
     * @param desired Detached typed object with an owner-assigned stable identity.
     * @param maximumBytes Finite canonical payload ceiling, at most 16 MiB.
     * @return Exact canonical payload digest or a typed validation/capacity error.
     */
    [[nodiscard]] Result<PCGGenerationDigest> FingerprintPCGSceneOutput(const SceneSource::SceneObjectSnapshot &desired,
                                                                        std::size_t maximumBytes);

    /** @brief Revalidates an exact plan and translates it into inert scene/static-mesh commands.
     * @param plan Immutable create/update/remove plan with target-issued provenance and resource estimates.
     * @param intents Exactly one desired value per create/update, no value for removal.
     * @param context Current target evidence, logical mappings, occupied authored IDs, lifecycle and storage bounds.
     * @return Canonical detached batch binding validation receipt and separately charged translation storage, or typed failure.
     * @post No entity, scene, asset, history entry or target resource is created, changed or authorized for publication.
     * The document/host must revalidate every owner and authorize a later aggregate transaction.
     */
    [[nodiscard]] Result<PCGSceneOutputBatch> TranslatePCGSceneOutputs(const PCGGenerationPlan &plan,
                                                                       std::span<const PCGSceneOutputIntent> intents,
                                                                       const PCGSceneOutputContext &context);
}  // namespace Horo::PCG
