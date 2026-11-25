#pragma once

/** @file MountedWorldHost.h
 * @brief Shared application ownership of a committed World mount.
 */
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/Navigation/NavigationCellTileSelection.h"
#include "Horo/Runtime/Render/RenderOriginScene.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/WorldStreaming/OriginFrame.h"
#include "Horo/WorldStreaming/WorldStreamingIdentity.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime {
    struct SceneCellWorldAnchor;
    class RuntimeDispatchEvidence;
    class RuntimeDispatchSource;
}  // namespace Horo::Runtime

namespace Horo {
    class JobSystem;
}

namespace Horo::Application::Internal {
    /** @brief Borrowed exact membership and base envelope for one cell in a proposed live topology. */
    struct MountedWorldNavigationCell final {
        WorldStreaming::StreamingCellId cell;
        const Navigation::NavigationCellTileSelection &selection;
        std::span<const std::uint8_t> envelope;
    };

    /** @brief Scene command intent for an exact mounted Navigation coverage change. */
    enum class MountedWorldNavigationChange : std::uint8_t {
        Attach,
        Retire,
        ForceEvict
    };

    /** @brief Detached publication and coverage of the newly attached cell, independent of the resident union. */
    struct MountedWorldNavigationPreparation final {
        std::shared_ptr<const Runtime::SceneGroupPublication> publication;
        bool attachedCellHasCoverage{};
    };

    /** @brief Prepares a command owner for the complete proposed resident NavMesh set on the Scene owner lane.
     * @details For Attach, the final row is the newly attached cell whose coverage is returned in the preparation.
     */
    using MountedWorldNavigationPublication =
        std::function<Result<MountedWorldNavigationPreparation>(const Runtime::RuntimeSceneView &,
                                                                std::span<const MountedWorldNavigationCell>, MountedWorldNavigationChange)>;
    /** @brief Advances provider-affine retirement and reports whether old query and tile pins are drained. */
    using MountedWorldNavigationRetirement = std::function<Result<bool>()>;

    /** @brief Explicit owner-lane cancellation and absolute deadline for one mounted cell activation. */
    struct MountedWorldStartup final {
        CancellationToken cancellation;
        std::chrono::steady_clock::time_point deadline;
    };

    namespace MountedWorldErrors {
        /** @brief The cold World startup deadline expired before all content was ready. */
        extern const ErrorCodeDescriptor Deadline;
    }  // namespace MountedWorldErrors

    /** @brief Exact authored occurrence and independently pinned cooked CoreEcs cell identity. */
    struct CommittedWorldInitialCell final {
        WorldStreaming::StreamingCellId cell;
        Runtime::SceneInstanceId occurrence;
        Runtime::SceneDefinitionId definition;
        Runtime::SceneDefinitionRevision revision;
    };

    /** @brief Observable single-cell mounted streaming lifecycle for the existing application owner. */
    enum class MountedWorldLiveCellState : std::uint8_t {
        Idle,
        Requested,
        Loading,
        Prepared,
        Attaching,
        Active,
        Retiring
    };

    /** @brief A visible cell can have declared coverage that contains no traversable tiles. */
    enum class MountedWorldCellNavigationReadiness : std::uint8_t {
        NotDeclared,
        Unavailable,
        Ready
    };

    /** @brief Current Scene visibility and Navigation availability for the exact admitted cell generation. */
    struct MountedWorldCellReadiness final {
        WorldStreaming::StreamingFence fence;
        bool geometryVisible{};
        MountedWorldCellNavigationReadiness navigation{};
    };

    /** @brief Explicit per-unit owner timing and complete CoreEcs frame-work bounds. */
    struct MountedWorldLiveLimits final {
        std::uint64_t maximumFrameNanoseconds{};
        std::uint64_t maximumUnitNanoseconds{};
        std::size_t maximumCellEntities{};
        std::size_t maximumCellAssets{};     /**< Zero disables external resources for this mount. */
        std::size_t maximumCellAssetBytes{}; /**< Encoded-byte ceiling per cell and physical payload-byte ceiling per mount. */
        std::size_t maximumResidentCells{1}; /**< Maximum simultaneous live CoreECS occurrences. */

        /** @brief Returns whether a complete bounded cell unit can be admitted with positive frame headroom. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Opts a mount into live CoreEcs work with an optional first demand in ordinary frames. */
    struct CommittedWorldLiveSelection final {
        MountedWorldLiveLimits limits;
        std::optional<CommittedWorldInitialCell> firstDemand;
    };

    /** @brief Explicit authored native Local placement projected through one exact mounted World origin. */
    struct MountedWorldTrackingRoot final {
        Math::Transform originFromLocal;
        WorldStreaming::OriginFrameLease origin;
    };

    /** @brief Exact project launch selection for one separate generated World cook generation. */
    struct CommittedWorldSelection final {
        std::filesystem::path targetRoot;
        Assets::AssetCookTargetId target;
        Sha256Digest manifest;
        Assets::AssetId index;
        Assets::AssetId dependencies;
        Math::WorldCoordinate64 origin; /**< Explicit authoritative fixed-point startup origin in Horo axes. */
        std::optional<CommittedWorldInitialCell> initialCell;
        std::optional<CommittedWorldLiveSelection> liveCell;
    };

    inline constexpr std::size_t CommittedWorldSelectionArgumentCount = 8;
    inline constexpr std::size_t CommittedWorldCellArgumentCount = 8;

    /** @brief Parses the shared explicit cook root, target, digest, two artifact roles and signed origin arguments. */
    [[nodiscard]] Result<CommittedWorldSelection> ParseCommittedWorldSelection(std::span<char *const> arguments);
    /** @brief Parses the shared explicit cell tuple and independently pinned Scene occurrence, definition and revision. */
    [[nodiscard]] Result<CommittedWorldInitialCell> ParseCommittedWorldInitialCell(std::span<char *const> arguments);
    /** @brief Parses the three explicit frame, unit, and entity limits used by live mount launch options. */
    [[nodiscard]] Result<MountedWorldLiveLimits> ParseMountedWorldLiveLimits(std::span<char *const> arguments);

    /** @brief Retains generation-fenced renderer output and the actual immutable procedural resources it references. */
    class MountedWorldRenderPacket final {
    public:
        MountedWorldRenderPacket(MountedWorldRenderPacket &&) noexcept = default;
        MountedWorldRenderPacket(const MountedWorldRenderPacket &) = delete;
        MountedWorldRenderPacket &operator=(const MountedWorldRenderPacket &) = delete;

        /** @brief Borrows the complete scene while the source Scene and renderer origin are current.
         * @return Current renderer packet or original stale owner error.
         */
        [[nodiscard]] Result<Render::RenderSceneView> Scene() const;

        /** @brief Borrows the origin-relative geometry and exact owning origin for the active mounted Scene.
         * @return Current renderer packet or typed stale/coordinate basis error.
         * @pre The packet and its primitive resource leases must outlive scene borrows.
         */
        [[nodiscard]] Result<Render::RenderOriginLocalScene> OriginScene() const;

        /** @brief Exact origin-relative draw geometry and its corresponding runtime owners. */
        struct IdentifiedOriginScene final {
            Render::RenderOriginLocalScene geometry;
            std::span<const Runtime::EntityRef> drawEntities;
        };

        /** @brief Borrows origin-relative draws and their runtime owners in the same order.
         * @return Complete scoped geometry and identities or a stale/foreign origin or Scene failure.
         * @pre The packet must outlive the synchronous borrow. */
        [[nodiscard]] Result<IdentifiedOriginScene> OriginSceneWithOwners() const;

        /** @brief Checks a newer renderer capture from the same mounted Scene incarnation and view owner.
         * @param previous Prior packet expected by the viewport publication.
         * @return Whether this packet is a provenance-matching successor. Validate Scene before publishing it. */
        [[nodiscard]] bool IsSuccessorOf(const MountedWorldRenderPacket &previous) const noexcept;

    private:
        friend class MountedWorld;
        MountedWorldRenderPacket(Runtime::RuntimeSceneView parent, Render::RenderOriginFrame frame,
                                 std::vector<Runtime::PrimitiveMeshLease> leases, std::vector<Render::RenderMeshResourceView> resources,
                                 std::vector<Runtime::EntityRef> drawEntities) noexcept;

        Runtime::RuntimeSceneView parent_;
        Render::RenderOriginFrame frame_;
        std::vector<Runtime::PrimitiveMeshLease> leases_;
        std::vector<Render::RenderMeshResourceView> resources_;
        std::vector<Runtime::EntityRef> drawEntities_;
    };

    /** @brief Private candidate and active World owners retained by the selected application Scene record. */
    class MountedWorld final {
    public:
        /** @brief Prepare one verified generation, owned registry, origin, planner and resident-state authority.
         * @param files Explicit host filesystem, borrowed only for cold admission.
         * @param selection Exact project launch selection.
         * @param generation Non-zero unique owner-lane generation of the receiving Scene record.
         * @param scene Real runtime Scene service that outlives the mount.
         * @param cancellation Parent preparation cancellation.
         * @return Complete owned mount or typed error. No authority escapes on failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<MountedWorld>> Prepare(const DurableFileSystem &files,
                                                                           const CommittedWorldSelection &selection,
                                                                           std::uint64_t generation, Runtime::RuntimeSceneService &scene,
                                                                           const CancellationToken &cancellation = {});

        /** @brief Activate one independent, pinned CoreEcs cell through the private live parent Scene.
         * @param selected Exact authored occurrence and cooked cell identity from host policy.
         * @param boundary Existing cold startup/travel absolute deadline and parent cancellation.
         * @param phase Synchronously borrowed actual editor cutoff, or null for standalone cold startup.
         * @return Success only after the Scene attachment receipt and Active residency agree.
         * @pre The parent is privately prepared. On a post-queue failure, it is shut down before retiring scheduler work.
         */
        [[nodiscard]] Result<void> ActivateInitialCell(const CommittedWorldInitialCell &selected, const MountedWorldStartup &boundary,
                                                       const Runtime::FrameContext *phase = nullptr);

        /** @brief Reserves a verified cell and queues it inside the private startup root.
         * @param parent Exact root definition being prepared by the bound Scene service.
         * @param selected Exact committed cell and authored occurrence.
         * @param boundary Parent cancellation and deadline retained through publication.
         * @param registry The Scene service's current structural catalog fence.
         * @return Successful admission (possibly pending) or original typed rejection after reservation retirement.
         * @pre Call on the bound Scene owner lane. The mount must stay alive through reconciliation or cancel the pending authority.
         */
        [[nodiscard]] Result<void> QueueInitialCell(const Runtime::RuntimeSceneDefinition &parent,
                                                    const CommittedWorldInitialCell &selected, const MountedWorldStartup &boundary,
                                                    Assets::AssetRegistryRevision registry);

        /** @brief Settles startup against the receipt after the bound Scene service advances its normal lifecycle cutoff.
         * @return True on exact active root and cell, false while pending, or the originating failure after retirement.
         * @pre Call on the bound Scene owner lane after each startup cutoff until completion.
         */
        [[nodiscard]] Result<bool> ReconcileInitialCell();

        /** @brief Opt in to explicitly selected live CoreEcs cells on a mount with no startup cell.
         * @param dispatch Exact canonical RuntimeHost dispatcher for this Scene owner.
         * @param jobs Existing bounded worker owner, which must outlive this mount's retirement.
         * @param limits Complete explicit preparation and owner-frame work limits.
         * @param assets Pinned project asset registry for mounts admitting external dependencies.
         * @return Success, or typed denial without accessing Scene or starting work.
         * @pre Bind and advance on the Scene owner lane. Withdraw and drain at the ordinary structural cutoff before
         * replacing the Scene. Destroy an undrained mount only at the cold owner boundary, outside worker callbacks.
         */
        [[nodiscard]] Result<void> BindLiveCell(const Runtime::RuntimeDispatchSource &dispatch, const JobSystem &jobs,
                                                const MountedWorldLiveLimits &limits, Assets::AssetRegistrySnapshot assets = {});

        /** @brief Binds the host-selected navigation owner before the first live demand. */
        [[nodiscard]] Result<void> BindLiveNavigation(MountedWorldNavigationPublication publication,
                                                      MountedWorldNavigationRetirement retirement = {});

        /** @brief Reports whether the admitted World inventory contains cell-owned NavMesh coverage. */
        [[nodiscard]] bool HasLiveNavigationContent() const noexcept;

        /** @brief Admit an exact selected cell as demand; the structural frame owns worker submission.
         * @param selected Exact validated mount cell, independent authored occurrence, definition and revision.
         * @param cancellation Parent operation cancellation retained through the activation receipt.
         * @return Success or typed stale, conflicting or unsupported demand.
         */
        [[nodiscard]] Result<void> RequestLiveCell(const CommittedWorldInitialCell &selected, const CancellationToken &cancellation = {});

        /** @brief Withdraw one live demand, cancelling pending preparation or retiring its committed occurrence.
         * @return Success only while this mount owns exactly one pending or published occurrence.
         */
        [[nodiscard]] Result<void> WithdrawLiveCell();

        /** @brief Withdraw a pending or published occurrence selected by its exact authored instance.
         * @param occurrence Instance ID originally admitted by RequestLiveCell.
         * @return Success or typed stale evidence if the mount does not own that occurrence.
         */
        [[nodiscard]] Result<void> WithdrawLiveCell(Runtime::SceneInstanceId occurrence);

        /** @brief Overrides demand for an exact resident cell on the authoritative mount owner lane.
         * @param occurrence Resident occurrence selected by the trusted host.
         * @return Success when retirement is requested, or a typed refusal once its Scene transaction is pending.
         * @details Uses the ordinary retirement fence and waits for provider pins before returning residency capacity.
         */
        [[nodiscard]] Result<void> ForceEvictLiveCell(Runtime::SceneInstanceId occurrence);

        /** @brief Read exact charged CPU asset capacity on the Scene owner lane.
         * @return Shared resident and retiring payload bytes, or a typed error if the live owner is unavailable.
         */
        [[nodiscard]] Result<std::size_t> InspectLiveAssetBytes() const;

        /** @brief Reads Scene visibility and Navigation separately for one live occurrence on the owner lane. */
        [[nodiscard]] Result<MountedWorldCellReadiness> InspectLiveCell(Runtime::SceneInstanceId occurrence) const;

        /** @brief Checks whether an exact live cell occurrence is published in the supplied Scene.
         * @param parent Current Scene view on the owner lane.
         * @param occurrence Scene instance selected by the authoritative simulation.
         * @param fence The observed partition, cell and residency generation.
         * @return True only while that occurrence and generation are structurally visible.
         */
        [[nodiscard]] bool IsLiveRegionPublished(const Runtime::RuntimeSceneView &parent, Runtime::SceneInstanceId occurrence,
                                                 const WorldStreaming::StreamingFence &fence) const noexcept;

        /** @brief Advances at most one bounded unit during an exact, previously unused structural dispatch.
         * @param evidence Live CommitDeferredLifecycleChanges evidence from the bound scheduler.
         * @return Current state or preserved typed error. The caller still owns the ordinary Scene phase cutoff.
         */
        [[nodiscard]] Result<MountedWorldLiveCellState> AdvanceLiveCell(const Runtime::RuntimeDispatchEvidence &evidence);

        /** @brief Observe authored global anchors for the exact current parent and mounted Scene occurrence.
         * @param parent Active Scene borrow on the owning lane.
         * @return Synchronous mounted storage borrow, empty for legacy, foreign, stale or retired Scene evidence.
         */
        [[nodiscard]] std::span<const Runtime::SceneCellWorldAnchor> Anchors(const Runtime::RuntimeSceneView &parent) const noexcept;

        /**
         * @brief Prepare selected camera and procedural mesh resources from one live mounted cell on the owner lane.
         * @param parent Exact current Scene publication.
         * @param camera Explicit anchored camera object in the attached cell.
         * @param meshes Strictly ascending authored object identities, all required and owned by the same cell.
         * @param primitives Existing CPU mesh cache; returned resource leases outlive cache eviction.
         * @param renderer Same-World renderer adapter; its view and origin must remain valid during capture.
         * @param basis Renderer coordinate basis for all geometry from this mounted selection.
         * @return Complete cold renderer packet or a typed absence/stale/unsupported/capacity failure.
         * @details Admits root objects only. World positions remain signed millimeter anchors. Local rotation and scale carry no
         * global translation authority. A local position edit since cell admission requires a new World position publication.
         */
        [[nodiscard]] Result<MountedWorldRenderPacket> CaptureGeometry(
            const Runtime::RuntimeSceneView &parent, Runtime::SceneObjectId camera, std::span<const Runtime::SceneObjectId> meshes,
            Runtime::PrimitiveMeshCache &primitives, Render::RenderOriginSceneAdapter &renderer,
            Render::RenderOriginCoordinateBasis basis = Render::RenderOriginCoordinateBasis::CameraRelative) const;

        /** @brief Captures origin-relative geometry inside this mount's exact private startup candidate.
         * @param candidate Scene activation participant's current private candidate.
         * @param camera Explicit anchored camera object from the admitted cell.
         * @param meshes Strictly ascending explicit mesh objects from the same cell.
         * @param primitives Existing CPU mesh cache and resource lease owner.
         * @param renderer Exact bound World origin and frontend view.
         * @return Retained geometry with its exact World origin lease, or typed rejection.
         * @details Requires the same scheduler activation reservation and private Scene incarnation. Grants no session authority.
         */
        [[nodiscard]] Result<MountedWorldRenderPacket> CaptureInitialGeometry(const Runtime::RuntimeSceneView &candidate,
                                                                              Runtime::SceneObjectId camera,
                                                                              std::span<const Runtime::SceneObjectId> meshes,
                                                                              Runtime::PrimitiveMeshCache &primitives,
                                                                              Render::RenderOriginSceneAdapter &renderer) const;

        /** @brief Projects this private cell's explicitly selected tracking root into the canonical origin.
         * @param candidate Current exact private startup Scene.
         * @param object Authored root explicitly selected as the native Local placement.
         * @return Rigid origin-local placement and the exact World origin lease, or typed rejection.
         * @details Uses the root's canonical signed position and live rigid orientation. Requires unit authored scale.
         * This is authored placement. Live reference-space observation is separately owned by the native host.
         */
        [[nodiscard]] Result<MountedWorldTrackingRoot> ResolveInitialTrackingRoot(const Runtime::RuntimeSceneView &candidate,
                                                                                  Runtime::SceneObjectId object) const;

        /** @brief Observe the exact mounted World frame; the caller keeps this complete mount alive. */
        [[nodiscard]] const WorldStreaming::OriginFrameOwner &Origin() const noexcept;
        /** @brief Revoke mounted authority and close the registry before releasing the borrowed Scene service. */
        ~MountedWorld();

    private:
        enum class GeometryScope {
            Active,
            PrivateStartup
        };
        /** @brief Shares origin/mesh projection after applying the selected private or live ownership boundary. */
        [[nodiscard]] Result<MountedWorldRenderPacket> CaptureScopedGeometry(
            const Runtime::RuntimeSceneView &parent, Runtime::SceneObjectId camera, std::span<const Runtime::SceneObjectId> meshes,
            Runtime::PrimitiveMeshCache &primitives, Render::RenderOriginSceneAdapter &renderer, Render::RenderOriginCoordinateBasis basis,
            GeometryScope scope) const;
        struct State;
        explicit MountedWorld(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Application::Internal
