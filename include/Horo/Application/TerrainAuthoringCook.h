#pragma once
/** @file TerrainAuthoringCook.h @brief Explicit single-attempt terrain source capture and Assets cook publication. */

#include "Horo/Assets/AssetCookService.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Editor/TerrainAuthoringDocument.h"
#include "Horo/Terrain/FoliagePlacementCook.h"
#include "Horo/Terrain/TerrainAuthoringSourceCodec.h"
#include "Horo/Terrain/TerrainSourceArtifacts.h"

namespace Horo::Application {
    namespace TerrainAuthoringCookErrors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor Busy;
        extern const ErrorCodeDescriptor Closed;
        extern const ErrorCodeDescriptor Stale;
        extern const ErrorCodeDescriptor Failed;
        extern const ErrorCodeDescriptor Cancelled;
    }  // namespace TerrainAuthoringCookErrors

    /** @brief Caller-side adoption identity, excluded from source files and byte-affecting cache identity. */
    struct TerrainAuthoringCookFence final {
        Assets::AssetId workspace;
        Editor::TerrainDocumentSessionId session;
        Terrain::TerrainDatasetId dataset;
        Terrain::TerrainSourceRevision source;
        Terrain::TerrainCapabilityRevision capability;
        Sha256Digest sourceDigest;
        Sha256Digest layerDigest;
        auto operator<=>(const TerrainAuthoringCookFence &) const = default;
    };

    /** @brief One captured portable geometry target and exact independent work/storage ceilings. */
    struct TerrainAuthoringCookProfile final {
        Terrain::TerrainConfigurationSnapshot configuration;
        Terrain::TerrainSourceArtifactProfile artifacts;
        AssetCookTargetId target;
        Terrain::TerrainAuthoringCodecLimits source;
        Assets::AssetCookLimits assets;
        Terrain::FoliagePlacementCookLimits foliage{.maximumInstances = 65'536};
        std::uint64_t maximumCapturedBytes{128ULL * 1024 * 1024};
    };

    /** @brief One explicit saved source; dirty in-memory source must be saved before requesting publication. */
    struct TerrainAuthoringCookRequest final {
        std::filesystem::path sourceRoot;
        std::filesystem::path cookedRoot;
        Assets::AssetId asset;
        TerrainAuthoringCookFence fence;
        Sha256Digest acceptedMetadataDigest;
        TerrainAuthoringCookProfile profile;
    };

    /** @brief Actual host services; all borrowed authorities outlive explicit Drain and destruction. */
    struct TerrainAuthoringCookEnvironment final {
        JobSystem *jobs{};
        const Assets::AssetRegistry *registry{};
        const Editor::ProjectMutationCoordinator *mutations{};
        std::shared_ptr<const ImmutableFileStorage> cacheFiles;
        std::shared_ptr<DurableFileSystem> publicationFiles;
        std::function<Result<Assets::AssetId>()> newPublicationOperationId;
        OperationStore *operations{};
        BuildOutputStore *buildOutput{};
    };

    /** @brief Retained terminal publication outcome, including committed results that the editor now regards as stale. */
    struct TerrainAuthoringCookCompletion final {
        TerrainAuthoringCookFence fence;
        Result<Assets::AssetCookReport> result;
    };

    /** @brief Owner-thread control facade with one feature-owned control thread, one attempt and no queued requests.
     * @details Source capture/cache/publication I/O runs on the control thread; ordinary AssetCookService CPU jobs run on
     * the shared JobSystem. No worker retains a document, UI borrow or mutable source. The transfer mutex protects only
     * pending/completed values; it is never held during source reads, cook, native publication or joining.
     * Owner commands revoke adoption immediately on edit/close/replacement. Cancellation is cooperative; a committed
     * old result remains truthful publication evidence and is never represented as current for a different fence.
     */
    class TerrainAuthoringCookHost final {
    public:
        /** @brief Capture inert explicit services and start one waiting control thread.
         * @param environment Host-retained services; no source I/O or cook starts during construction.
         * @return Owned host or typed missing-capability/allocation/thread-start error.
         */
        [[nodiscard]] static Result<std::unique_ptr<TerrainAuthoringCookHost>> Create(TerrainAuthoringCookEnvironment environment);
        /** @brief Cancel and join before releasing services; teardown only, never Draw or ordinary Poll. */
        ~TerrainAuthoringCookHost();
        TerrainAuthoringCookHost(const TerrainAuthoringCookHost &) = delete;
        TerrainAuthoringCookHost &operator=(const TerrainAuthoringCookHost &) = delete;
        /** @brief Admit one owned immutable request; a running/unconsumed attempt rejects immediately.
         * @param request Actual saved source and captured target policy. @return Success or typed busy/closed/invalid result.
         */
        [[nodiscard]] Result<void> Start(TerrainAuthoringCookRequest request);
        /** @brief Request cooperative cancellation without waiting for CPU/native I/O completion. */
        void Cancel() noexcept;
        /** @brief Revoke new attempts and request cancellation; retained terminal evidence remains available to Poll. */
        void Retire() noexcept;
        /** @brief Take one completion using a nonblocking transfer try-lock; performs no I/O, joins or repeated copying.
         * @return Owned terminal result once, or no value when running/temporarily contended/already consumed.
         */
        [[nodiscard]] std::optional<TerrainAuthoringCookCompletion> Poll();
        /** @brief Query whether accepted work still owns its native/capture/job dependencies. @return Atomic running fact. */
        [[nodiscard]] bool IsRunning() const noexcept;
        /** @brief Close admission and join the feature control thread after all accepted children finish.
         * @details Explicit application teardown; native synchronous calls cannot be force-interrupted. The application
         * retains this host and its registry/mutations/files/JobSystem until return. No detached continuation exists.
         */
        void Drain() noexcept;

    private:
        struct State;
        explicit TerrainAuthoringCookHost(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
        std::shared_ptr<CancellationSource> cancellation_;
        bool retired_{};
    };

    /** @brief Host-selected portable geometry policy and derived output location, owning one drained worker facade. */
    struct TerrainAuthoringCookSession final {
        std::unique_ptr<TerrainAuthoringCookHost> host;
        TerrainAuthoringCookProfile profile;
        std::filesystem::path cookedRoot;
    };

    /** @brief Explicit executable composition; no feature performs platform/backend discovery. */
    using TerrainAuthoringCookFactory = std::function<Result<TerrainAuthoringCookSession>(const std::filesystem::path &)>;
}  // namespace Horo::Application
