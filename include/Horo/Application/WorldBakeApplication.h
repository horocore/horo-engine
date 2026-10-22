#pragma once
/** @file WorldBakeApplication.h
 * @brief Application-owned, cancellable offline cooking of actual immutable world page content.
 */
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Editor/PartitionedWorldWorkspace.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Platform/ProjectReadFiles.h"
#include "Horo/Runtime/Scene/IncrementalSceneCellCook.h"
#include "Horo/Runtime/Scene/SceneCellPayloadCodec.h"
#include "Horo/Runtime/Scene/WorldBuildValidation.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "Horo/WorldStreaming/StreamingCellEncoding.h"

namespace Horo::Application {
    /** @brief Stable inert application failures; original Scene, Assets and root-reader causes remain intact. */
    namespace WorldBakeErrors {
        /** @brief Malformed request, source identity or execution mode. */
        extern const ErrorCodeDescriptor Invalid;
        /** @brief Explicit bounded storage or operation admission is exhausted. */
        extern const ErrorCodeDescriptor Capacity;
        /** @brief An exact actual source page is unavailable. */
        extern const ErrorCodeDescriptor MissingPage;
        /** @brief Required verified provider or schema inputs are missing. */
        extern const ErrorCodeDescriptor Dependency;
        /** @brief A requested provider operation has no captured implementation. */
        extern const ErrorCodeDescriptor Unsupported;
        /** @brief Identity, membership or authored placement conflicts. */
        extern const ErrorCodeDescriptor Conflict;
        /** @brief The captured source or project publication was superseded. */
        extern const ErrorCodeDescriptor Stale;
        /** @brief Cancellation prevented unpublished output adoption. */
        extern const ErrorCodeDescriptor Cancelled;
        /** @brief The owner is closed or called from a non-owning thread. */
        extern const ErrorCodeDescriptor Closed;
    }  // namespace WorldBakeErrors

    /** @brief Exact independent project/schema publication, not an operation or runtime generation. */
    struct WorldBakeProjectFence final {
        Assets::AssetId project;
        std::uint64_t revision{};
        Sha256Digest schemas;
        std::uint64_t registryRevision{}; /**< Exact asset catalog publication captured independently from project/schema state. */
        [[nodiscard]] auto operator<=>(const WorldBakeProjectFence &) const noexcept = default;
    };

    /** @brief Verified dependency evidence captured by the Assets host, including transitive edges. */
    struct WorldBakeDependency final {
        Assets::AssetDependency asset;
        Sha256Digest digest;
        std::uint64_t revision{};
        std::vector<Assets::AssetId> dependencies;
    };

    /** @brief Explicit host-selected committed Assets source; no path, target or provider is inferred by feature code. */
    struct WorldBakeDependencySource final {
        std::filesystem::path targetRoot;
        Assets::AssetCookTargetId target;
        std::shared_ptr<const DurableFileSystem> files;
    };

    /** @brief Owned exact project inputs; no service locator, native handles or lazy provider lookup. */
    struct WorldBakeProjectCapture final {
        WorldBakeProjectFence fence;
        Sha256Digest settings;
        std::vector<Runtime::SceneCellComponentSchema> components;
        std::vector<Runtime::SceneCellBehaviorSchema> behaviors;
        std::vector<WorldBakeDependency> dependencies;
        std::optional<WorldBakeDependencySource>
            dependencySource; /**< Alternative to already captured evidence; resolved on the I/O lane. */
        std::optional<SceneSource::TriggerVolumeConversionContext> triggers;
        std::shared_ptr<const Prefab::PrefabSourceResolverSnapshot> prefabs;
        std::shared_ptr<const Prefab::PrefabValueSchemaSnapshot> prefabSchemas;
        std::optional<Prefab::PrefabLimitProfile> prefabLimits;
    };

    /** @brief Captures actual verified cooked generation envelopes, rather than trusting caller-provided digest strings.
     * @param files Explicit read-only durable filesystem. @param generation Exact Assets generation lease.
     * @param revision Positive host source publication. @param maximumBytes Bounded aggregate read budget.
     * @param maximumAssets Bounded complete generation inventory count, at most 4096.
     * @return Complete verified dependency evidence or original Assets failure; no partial table escapes.
     */
    [[nodiscard]] Result<std::vector<WorldBakeDependency>> CaptureWorldBakeDependencies(const DurableFileSystem &files,
                                                                                        const Assets::AssetCookGeneration &generation,
                                                                                        std::uint64_t revision, std::size_t maximumBytes,
                                                                                        std::size_t maximumAssets = 4096);

    /** @brief Mandatory bounded tooling ceilings, independent of runtime residency. */
    struct WorldBakeLimits final {
        std::uint32_t maximumObjects{65536};
        std::uint32_t maximumCells{4096};
        std::uint32_t maximumAssignments{65536};
        std::uint32_t maximumCellsPerObject{4096};
        std::size_t maximumOutputBytes{64 * 1024 * 1024};
        std::size_t maximumSourceBytes{64 * 1024 * 1024};
        Runtime::SceneCellCookCacheLimits cache{4096, 4096, 65536, 4096, 16 * 1024 * 1024, 64 * 1024 * 1024};
        Runtime::SceneCellPayloadCodecLimits codec{16 * 1024 * 1024, 64 * 1024 * 1024, {65536, 4096, 64 * 1024 * 1024}};
        Runtime::WorldBuildValidationLimits validation{4096, 65536, 65536, 4096, 1024 * 1024, 64 * 1024 * 1024, 64 * 1024 * 1024};
    };

    /** @brief Validates an inert project/schema/dependency capture before a host replaces its current configuration.
     * @param project Complete owned project inputs. @param limits Positive finite cook bounds.
     * @return Success or typed invalid/dependency/capacity rejection; no services, source or operation state are changed. */
    [[nodiscard]] Result<void> ValidateWorldBakeProjectCapture(const WorldBakeProjectCapture &project, const WorldBakeLimits &limits = {});
    /** @brief Bake and validate share actual conversion/cooking/qualification; validate never replaces baked output. */
    enum class WorldBakeMode : std::uint8_t {
        Bake,
        Validate
    };

    /** @brief One exact source page and host-authorized navigable source path. */
    struct WorldBakePageSource final {
        WorldStreaming::WorldAuthoringPageDescriptor page;
        DiagnosticSourceLocation source;
        std::string relativePath; /**< Exact registry-resolved root-relative source spelling for an unloaded page. */
    };

    /** @brief Exact retained root-reader authority, independent from mutable project/schema publication revisions. */
    struct WorldBakeSourceAuthority final {
        std::shared_ptr<const Platform::IProjectReadFiles> files;
        Assets::AssetId owner;
        std::uint64_t generation{};
        std::shared_ptr<const Editor::ProjectMutationLease>
            reservation; /**< Application requests retain the existing project writer fence through adoption. */
    };

    /** @brief Retained canonical document lease plus owned project inputs and explicit source capabilities. */
    struct WorldBakeRequest final {
        Editor::PartitionedWorldSnapshotLease document;
        WorldBakeProjectCapture project;
        std::vector<WorldBakePageSource> sources;
        Assets::AssetRegistrySnapshot registry;   /**< Exact catalog publication used for source paths and known artifact type fences. */
        WorldBakeSourceAuthority sourceAuthority; /**< Exact root-bound reader and its independent authority fence. */
        WorldBakeLimits limits;
        WorldBakeMode mode{WorldBakeMode::Bake};
        std::string title; /**< Host-localized user-facing operation title. */
    };

    /** @brief Actual canonical CoreEcs/HOROCELL output and its complete semantic validation receipt. */
    struct WorldBakeOutput final {
        Editor::WorldDocumentFence document;
        WorldBakeProjectFence project;
        Editor::PartitionedWorldSnapshotLease source; /**< Exact retained placement metadata and actual loaded source leases. */
        Runtime::WorldBuildValidationReport validation;
        std::vector<std::shared_ptr<const Runtime::RuntimeSceneCellPayload>> cells;
        std::vector<WorldStreaming::EncodedStreamingCell> artifacts;
        std::vector<std::pair<Assets::AssetId, std::string>>
            sourceRevisions; /**< Exact root-reader content revisions, rechecked before return. */
        std::optional<Assets::AssetCookGeneration>
            dependencyGeneration; /**< Exact verified committed resource generation, never a fabricated digest table. */
        std::vector<Runtime::RuntimeSceneDefinition> sourceScenes; /**< Complete converted sources retain explicit base content. */
    };

    /**
     * @brief Converts actual retained page payloads, applies authored placement, cooks and validates a complete cell generation.
     * @param request Exact immutable source, project capture, mode and positive ceilings.
     * @param cancellation Cooperative observer; failure/cancellation returns no partially publishable output.
     * @param progress Optional synchronous monotonic stage observer; never retained.
     * @return Owned detached complete output or original typed failure. Root-bound source reads are permitted; no files, cache owner,
     * runtime or diagnostic store is mutated.
     * @details This is tooling work for an application job. Source scenes retain always-present content; Assets release packaging
     * remains an explicit downstream operation. SplitPerCell requires a geometry provider and is rejected, never cloned as a split.
     * @throws std::bad_alloc Before publication; caller retains previous output.
     */
    [[nodiscard]] Result<WorldBakeOutput> CookWorldPages(const WorldBakeRequest &request, const CancellationToken &cancellation = {},
                                                         const std::function<void(float)> &progress = {});

    namespace WorldBakeDetail {
        struct Attempt;
    }

    /**
     * @brief Application host-thread operation owner; closing a panel never owns or cancels this service.
     * @details One active job; replacement cancels it and retains one pending request. Pump alone publishes output and diagnostics
     * after checking the actual current document and project fences. Workers retain immutable captures and never reference this facade.
     * Host calls Submit/Pump/Close on the constructing thread. Jobs, operations and BuildOutput outlive the facade and drained jobs.
     * Close gates admission and cancels accepted work without blocking UI; the application subsequently drains its scheduler.
     */
    class WorldBakeApplication final {
    public:
        /** @brief Borrows explicit process capabilities. @param jobs Scheduler. @param operations Operation authority.
         * @param output Shared bounded diagnostic authority. */
        WorldBakeApplication(JobSystem &jobs, OperationStore &operations, BuildOutputStore &output);
        /** @brief Cancels unadopted work and releases the facade; immutable output leases survive. */
        ~WorldBakeApplication();
        WorldBakeApplication(const WorldBakeApplication &) = delete;
        WorldBakeApplication &operator=(const WorldBakeApplication &) = delete;
        /** @brief Accepts an exact owned capture, cancelling older work only after admission succeeds.
         * @param request Source/project capture and localized title. @return Fresh operation ID, or typed admission failure. */
        [[nodiscard]] Result<OperationId> Submit(WorldBakeRequest request);
        /** @brief Polls completion and rejects obsolete results before any output/diagnostic append.
         * @param current Current canonical document, null on close. @param project Current project/schema fence. */
        void Pump(Editor::PartitionedWorldSnapshotLease current, WorldBakeProjectFence project);
        /** @brief Cancels unadopted work, closes admission and preserves already published output. */
        void Close() noexcept;
        /** @brief Observes last successful bake; validation never overwrites it. @return Owner-thread immutable output lease. */
        [[nodiscard]] std::shared_ptr<const WorldBakeOutput> Published() const noexcept;
        /** @brief Returns the latest active/pending operation for controls; retained terminal history lives in OperationStore.
         * @return Fresh accepted ID or empty when idle. */
        [[nodiscard]] std::optional<OperationId> ActiveOperation() const noexcept;

    private:
        /** @brief Admits one owned job only after older work drains. */
        void StartPending();
        /** @brief Cancels older accepted work after replacement admission. */
        void CancelSuperseded();
        /** @brief Retires obsolete or cancelled pending captures. */
        void RetirePending(const Editor::PartitionedWorldSnapshotLease &current, const WorldBakeProjectFence &project);
        /** @brief Linearizes terminal cancellation against publication. */
        void Complete(const std::shared_ptr<WorldBakeDetail::Attempt> &attempt, const JobSnapshot &snapshot, bool fresh);
        /** @brief Publishes one original typed failure with source navigation. */
        void PublishFailure(const std::shared_ptr<WorldBakeDetail::Attempt> &attempt, const JobSnapshot &snapshot);
        /** @brief Publishes validation diagnostics and successful bake output. */
        void PublishResult(const std::shared_ptr<WorldBakeDetail::Attempt> &attempt);
        /** @brief Applies exactly one terminal operation-store transition. */
        void Finish(const std::shared_ptr<WorldBakeDetail::Attempt> &attempt, OperationState state, std::optional<Error> error = {});
        JobSystem &jobs_;
        OperationStore &operations_;
        BuildOutputStore &output_;
        const std::thread::id thread_{std::this_thread::get_id()};
        std::shared_ptr<WorldBakeDetail::Attempt> pending_;
        std::shared_ptr<WorldBakeDetail::Attempt> active_;
        JobHandle job_;
        std::shared_ptr<const WorldBakeOutput> published_;
        bool closed_{};
    };
}  // namespace Horo::Application
