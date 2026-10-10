#pragma once

/** @file PrefabSceneCookHost.h
 * @brief Explicit host composition of project compatibility, prefab expansion, AssetCook publication and release handoff.
 */
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Assets/AssetCookService.h"
#include "Horo/Editor/ProjectMigrationTransaction.h"
#include "Horo/Gameplay/BehaviorTypes.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Packages/PackageRestore.h"
#include "Horo/Prefab/PrefabLimits.h"
#include "Horo/Release/ReleasePipelineExecutor.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "Horo/Scene/ScenePrefabExpansionOwner.h"

#include <vector>

namespace Horo::Application {
    namespace PrefabSceneCookErrors {
        /** @brief Host compatibility, required package evidence or source cook composition is invalid. */
        extern const ErrorCodeDescriptor Invalid;
        /** @brief A captured source, project, package or release input changed before publication. */
        extern const ErrorCodeDescriptor Stale;
        /** @brief Host cook was cancelled before committed publication. */
        extern const ErrorCodeDescriptor Cancelled;
    }  // namespace PrefabSceneCookErrors

    /** @brief Owned inert schema authority copied from explicitly selected host registries, with no factories or discovery. */
    class PrefabCookSchemaContext final {
        /** @brief Capture-only capability preventing validation bypass. */
        class ConstructionKey final {
            friend class PrefabCookSchemaContext;
            ConstructionKey() = default;
        };

    public:
        /** @brief Copies complete admitted inert metadata through the shared allocation factory.
         * @param key Private capability issued only by Capture.
         * @param components Frozen component registry copied into owned storage.
         * @param behaviors Admitted behavior descriptors copied into owned storage.
         * @param digest Complete deterministic semantic identity.
         */
        PrefabCookSchemaContext(ConstructionKey key, const Gameplay::ComponentRegistry &components,
                                std::span<const Gameplay::BehaviorDescriptor> behaviors, const Sha256Digest &digest);
        /** @brief Captures bounded frozen component metadata and inert behavior descriptors without activation.
         * @param components Frozen host-selected component registry. @param behaviors Host-selected inert descriptors.
         * @return Immutable context or typed invalid/bounds diagnostic. At most 1024 types per family are captured. */
        [[nodiscard]] static Result<std::shared_ptr<const PrefabCookSchemaContext>> Capture(
            const Gameplay::ComponentRegistry &components, std::span<const Gameplay::BehaviorDescriptor> behaviors);
        /** @brief Validates every retained occurrence before admission; missing or skewed providers fail.
         * @param components Retained component envelopes. @param behaviors Retained behavior occurrences.
         * @return Success only for current schemas and compatible behavior fields/multiplicity. */
        [[nodiscard]] Result<void> Validate(std::span<const Gameplay::SerializedComponent> components,
                                            std::span<const Gameplay::BehaviorComponent> behaviors) const;
        /** @brief Returns the complete captured schema/settings commitment. @return Owned immutable digest. */
        [[nodiscard]] const Sha256Digest &Digest() const noexcept;

    private:
        Gameplay::ComponentRegistry components_;
        std::vector<Gameplay::BehaviorDescriptor> behaviors_;
        Sha256Digest digest_;
    };

    /** @brief One explicitly selected host operation; no metadata hashes supplied by the caller are accepted as source proof. */
    struct PrefabSceneCookRequest final {
        Assets::AssetCookRequest assets;
        Prefab::PrefabProjectPolicy prefabPolicy;
        SceneCook::CookedSceneLimits sceneLimits;
        std::uint64_t maximumCapturedBytes{256U * 1024U * 1024U};
        std::shared_ptr<const PrefabCookSchemaContext> schemas; /**< Explicit immutable project schema authority; absence admits no
                                                               opaque project components/behaviors. Core typed fields remain supported. */
        std::shared_ptr<const Packages::PackageRestoreGraph> restoredPackages; /**< Required when the project has a lockfile;
                                                                             archive evidence is verified against actual lock bytes.
                                                                             Trust/activation remains the package host's authority. */
        std::vector<Assets::AssetId> runtimePrefabRoots; /**< Explicit unique runtime-spawnable roots; empty preserves static-only
                                                        cooking. Source-only nested/variant prefabs are never published as templates. */
    };

    /**
     * @brief Synchronous host-owned scene and explicitly selected runtime-template cook operation.
     * @details This separate host target may compose Editor's existing migration/mutation owner. Assets and Scene runtime
     * do not acquire reverse dependencies. Source migration completes before invocation; static-only prefab sources
     * participate in cache identity but are excluded from the published runtime inventory. Selected dynamic roots use the same
     * capture and transaction: verified non-prefab envelopes stage first, then templates; one generation publishes both.
     */
    class PrefabSceneCookHost final {
    public:
        /** @brief Captures explicit host authorities without I/O, registration, activation or migration.
         * @param jobs Joined cook scheduler. @param catalog Immutable registered non-scene cooker contributions.
         * @param registry Live registry owner captured and revision-fenced for each operation.
         * @param compatibility Read-only compatibility policy. @param mutations Existing shared project transaction owner.
         * @param migrations Existing migration/recovery service. All borrowed authorities outlive this host. */
        PrefabSceneCookHost(JobSystem &jobs, std::shared_ptr<const Assets::CookerCatalogSnapshot> catalog,
                            const Assets::AssetRegistry &registry, const ProjectCompatibilityInspector &compatibility,
                            Editor::ProjectMutationCoordinator &mutations, const Editor::ProjectMigrationTransactionService &migrations);

        /** @brief Closes expansion admission and cancels/joins owned source preparation before releasing host authorities.
         * @details Destruction is a teardown boundary, never a frame operation. The scheduler outlives this host. */
        ~PrefabSceneCookHost();

        /** @brief Captures real project/source/package inputs and publishes only a complete validated generation.
         * @param request Explicit roots, target, bounded policies and host-owned publication capabilities.
         * @param cancellation Cooperative cancellation; accepted siblings are joined before return.
         * @return Published generation or typed compatibility/source/dependency/cook/publication failure.
         * @pre The host has authorized these roots and completed any required migration and package restore.
         * @post Holds the existing project-mutation lease from capture through AssetCook's final pointer commit.
         * @post Failure before pointer replacement preserves the previous generation; committed durability diagnostics remain success. */
        [[nodiscard]] Result<Assets::AssetCookReport> Cook(const PrefabSceneCookRequest &request,
                                                           const CancellationToken &cancellation = {});

        /** @brief Executes the same cook under the release owner's frozen-input checks and returns immutable generation bytes.
         * @param plan Exact release plan. @param facts Host-owned fresh observations checked again at commit.
         * @param request Host-selected cook request for the same project. @param cancellation Active release-stage cancellation.
         * @return Verified generation root and manifest digest; never a cache directory or raw prefab source inventory.
         * @details Intended for the host's IReleasePipelineStages::Cook implementation. Release packaging/signing/publication
         * remain owned by ReleasePipelineExecutor; this operation never independently publishes a release candidate. */
        [[nodiscard]] Result<Release::ReleaseCookedPayload> CookForRelease(const Release::ReleaseExecutionPlan &plan,
                                                                           Release::IReleasePreflightFactsProvider &facts,
                                                                           const PrefabSceneCookRequest &request,
                                                                           const CancellationToken &cancellation);

    private:
        /** @brief Runs one operation with optional validated release semantics included in its immutable cache identity. */
        [[nodiscard]] Result<Assets::AssetCookReport> CookImpl(const PrefabSceneCookRequest &request, const CancellationToken &cancellation,
                                                               const Release::ReleaseExecutionPlan *releasePlan);
        /** @brief Captures and cooks while retaining exclusive project mutation ownership through joined publication.
         * @param request Complete host inputs.
         * @param limits Validated prefab bounds.
         * @param cancellation Cooperative stop token.
         * @param releasePlan Optional frozen release identity.
         * @param projectLease Owned mutation authority; released only after cook completion or unwinding.
         * @return Committed report or typed failure preserving prior authority.
         */
        [[nodiscard]] Result<Assets::AssetCookReport> CookWithProjectLease(const PrefabSceneCookRequest &request,
                                                                           const Prefab::PrefabLimitProfile &limits,
                                                                           const CancellationToken &cancellation,
                                                                           const Release::ReleaseExecutionPlan *releasePlan,
                                                                           Editor::ProjectMutationLease projectLease);
        JobSystem &jobs_;
        std::shared_ptr<const Assets::CookerCatalogSnapshot> catalog_;
        const Assets::AssetRegistry &registry_;
        const ProjectCompatibilityInspector &compatibility_;
        Editor::ProjectMutationCoordinator &mutations_;
        const Editor::ProjectMigrationTransactionService &migrations_;
        SceneSource::ScenePrefabExpansionOwner expansion_; /**< Owned exact-input expansion worker and bounded result cache. */
    };
}  // namespace Horo::Application
