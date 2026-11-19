#pragma once

/** @file BlackboardUsageReview.h @brief Project-owned read-only Blackboard key usage admission. */

#include "Horo/AI/BlackboardUsageInspection.h"
#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "Horo/Scene/SceneSourceModel.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Horo::Editor {
    class ProjectMutationCoordinator;
    class ProjectMutationLease;
}  // namespace Horo::Editor

namespace Horo::AI {
    class QuerySchemaRegistry;
}

namespace Horo::Application {
    /** @brief Stable owner boundaries where this review cannot establish coverage. */
    enum class BlackboardUsageGapKind : std::uint8_t {
        RegistryInventory,
        DecisionSource,
        DecisionProvider,
        SceneSource,
        PrefabSource,
        EnvironmentQueryBindings,
        PerceptionPolicy,
        DecisionRetarget,
        DecisionClosure,
    };

    /** @brief An exact unresolved source or provider, or a project-level owner with no review adapter. */
    struct BlackboardUsageGap final {
        BlackboardUsageGapKind kind;
        std::optional<Assets::AssetId> asset;
        std::optional<AI::DecisionNodeId> node;
        std::optional<Error> diagnostic;
    };

    /** @brief Canonical source and identity metadata commitments under the reported registry revision. */
    struct BlackboardUsageSourceStamp final {
        Assets::AssetId asset;
        Sha256Digest source;
        Sha256Digest metadata;
    };

    /** @brief One positive typed occurrence backed by a registered Decision asset. */
    struct BlackboardDecisionUsage final {
        Assets::AssetId asset;
        AI::DecisionGraphAssetId graph;
        std::string sourcePath; /**< Exact registered project-relative source path. */
        AI::DecisionBlackboardSourceUse use;
    };

    /** @brief An authored result binding and its registered query asset when the source contract resolves. */
    struct BlackboardQueryUsage final {
        Assets::AssetId decisionAsset;
        std::string decisionPath;
        AI::DecisionBlackboardQueryUse use;
        std::optional<Assets::AssetId> queryAsset;
        std::string queryPath;
    };

    /** @brief A source-resolved controller occurrence in an explicitly authored Scene placement. */
    struct BlackboardPlacedPrefabObject final {
        Assets::AssetId rootPrefab;          /**< Asset named by the containing Scene placement. */
        Assets::AssetId sourcePrefab;        /**< Actual authored owner of the nested or inherited object. */
        Prefab::ExpandedPrefabObjectKey key; /**< Stable Scene instance and nested address. */
    };

    /** @brief Exact authored Scene object, standalone Prefab-local object or resolved Scene placement object. */
    using BlackboardSchemaBindingObject = std::variant<SceneSource::SceneObjectId, Prefab::LocalObjectId, BlackboardPlacedPrefabObject>;

    /** @brief A controller's authored schema binding. These sources carry SchemaId and GraphId without a key or schema revision. */
    struct BlackboardSchemaBinding final {
        Assets::AssetId asset;
        std::string sourcePath;
        BlackboardSchemaBindingObject object;
        AI::ControllerTypeId controller;
        AI::DecisionGraphAssetId decision;
    };

    /** @brief One exact durable schema key selected by the project document owner. */
    struct BlackboardUsageSelection final {
        Assets::AssetId schemaAsset;
        AI::BlackboardSchemaId schema;
        std::uint32_t version{};
        AI::BlackboardKeyId key;
        Sha256Digest source;                            /**< Exact schema bytes accepted by the durable document opening. */
        std::optional<AI::BlackboardKeyId> replacement; /**< Explicit destination for a detached migration preview. */
    };

    /** @brief Proposed replacement bytes for one exact registered source and sidecar capture. */
    struct BlackboardDecisionRetarget final {
        BlackboardUsageSourceStamp original;
        std::vector<std::uint8_t> candidate;
    };

    /** @brief Successful cooker admission for every affected root, fenced to the provider generation. */
    struct BlackboardDecisionValidation final {
        std::size_t roots{};
        std::weak_ptr<const AI::DecisionNodeGeneration> nodes;
    };

    /** @brief Explicit compiler authority for one proposed removal, captured by the workspace owner. */
    struct BlackboardRetargetValidationInput final {
        std::shared_ptr<const AI::BlackboardSchema> proposedSchema;
        std::shared_ptr<const AI::DecisionNodeGeneration> nodes;
        std::shared_ptr<const AI::QuerySchemaRegistry> queries;
    };

    class BlackboardUsageCommitEvidence;

    /** @brief Read-only observations scoped to a captured Project publication.
     * @details The weak provider stamp identifies the host-owned detached declaration snapshot. The publishing host must recheck it.
     * Gaps represent unknown coverage. This value grants no document mutation authority.
     */
    struct BlackboardUsageReview final {
        BlackboardUsageSelection selection;
        Assets::AssetRegistryRevision registry;
        Sha256Digest closure;
        std::weak_ptr<const AI::DecisionBlackboardProviderMetadata> providers;
        std::vector<BlackboardUsageSourceStamp> sources;
        std::vector<BlackboardDecisionUsage> uses;
        std::vector<BlackboardSchemaBinding> schemaBindings;
        std::vector<BlackboardUsageGap> gaps;
        std::vector<BlackboardQueryUsage> queryUsages;
        std::vector<BlackboardDecisionRetarget> decisionRetargets;
        std::optional<BlackboardDecisionValidation> decisionValidation;
        std::shared_ptr<const BlackboardUsageCommitEvidence> commitEvidence; /**< Exact bounded migration witnesses. */
    };

    /** @brief Captures and reviews actual registered source/sidecar pairs under the Project mutation coordinator.
     * @param root Canonical current Project root. @param registry Live registry publisher owned by the same Project.
     * @param completeInventory Native complete rebuild revision, when supplied by the host. Stale or absent coverage is reported.
     * @param mutations Project's normal source transaction coordinator. @param selection Exact durable key and schema selection.
     * @param providers Detached observed provider declarations, when the Project owns a snapshot. @param cancellation Cooperative read
     * fence. @param validation Optional compiler input for validating the explicit removal's Decision closure.
     * @return Owned typed occurrences, stamps and gaps, or a typed capture/staleness error. No authored sources or runtime state are
     * changed.
     */
    [[nodiscard]] Result<BlackboardUsageReview> CaptureBlackboardUsageReview(
        const std::filesystem::path &root, const Assets::AssetRegistry &registry,
        std::optional<Assets::AssetRegistryRevision> completeInventory, const Editor::ProjectMutationCoordinator &mutations,
        BlackboardUsageSelection selection, std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> providers = {},
        const CancellationToken &cancellation = {}, const BlackboardRetargetValidationInput &validation = {});

    /** @brief Rechecks the complete registered source witnesses at the Blackboard migration commit boundary.
     * @param root Exact canonical project root used for the review.
     * @param registry Live Project registry publisher that supplied the reviewed revision.
     * @param lease Caller-owned exclusive mutation lease for this project.
     * @param review Original immutable review with successful Decision validation and complete captured source witnesses.
     * @param currentProviders Current detached provider declarations supplied by the same Project host.
     * @param currentNodes Current compiler generation supplied by the same host, including a live owner for a nonempty closure.
     * @param cancellation Cooperative stop fence, checked during bounded source and sidecar reads.
     * @return Success while registered source and sidecar bytes, registry revision, and reviewed provider identities remain current.
     * @pre The caller keeps the live lease through publication and resolves review gaps and document revision fences separately.
     * @details This read-only check does not admit a mutation. The original candidate generation must survive until confirmation.
     */
    [[nodiscard]] Result<void> VerifyBlackboardUsageCommitFence(
        const std::filesystem::path &root, const Assets::AssetRegistry &registry, const Editor::ProjectMutationLease &lease,
        const BlackboardUsageReview &review, const std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> &currentProviders,
        const std::shared_ptr<const AI::DecisionNodeGeneration> &currentNodes, const CancellationToken &cancellation = {});
}  // namespace Horo::Application
