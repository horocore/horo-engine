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

    /** @brief Exact authored Scene object or standalone Prefab-local object. */
    using BlackboardSchemaBindingObject = std::variant<SceneSource::SceneObjectId, Prefab::LocalObjectId>;

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
        Sha256Digest source; /**< Exact schema bytes accepted by the durable document opening. */
    };

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
    };

    /** @brief Captures and reviews actual registered source/sidecar pairs under the Project mutation coordinator.
     * @param root Canonical current Project root. @param registry Live registry publisher owned by the same Project.
     * @param completeInventory Native complete rebuild revision, when supplied by the host. Stale or absent coverage is reported.
     * @param mutations Project's normal source transaction coordinator. @param selection Exact durable key and schema selection.
     * @param providers Detached observed provider declarations, when the Project owns a snapshot. @param cancellation Cooperative read
     * fence.
     * @return Owned typed occurrences, stamps and gaps, or a typed capture/staleness error. No authored sources or runtime state are
     * changed.
     */
    [[nodiscard]] Result<BlackboardUsageReview> CaptureBlackboardUsageReview(
        const std::filesystem::path &root, const Assets::AssetRegistry &registry,
        std::optional<Assets::AssetRegistryRevision> completeInventory, const Editor::ProjectMutationCoordinator &mutations,
        BlackboardUsageSelection selection, std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> providers = {},
        const CancellationToken &cancellation = {});
}  // namespace Horo::Application
