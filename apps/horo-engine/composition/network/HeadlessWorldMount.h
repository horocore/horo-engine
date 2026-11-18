#pragma once

/** @file HeadlessWorldMount.h
 * @brief Explicit standalone application ownership of a committed World mount.
 */
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/Runtime/Render/RenderOriginScene.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/WorldStreaming/OriginFrame.h"
#include "Horo/WorldStreaming/WorldStreamingIdentity.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime {
    struct SceneCellWorldAnchor;
}  // namespace Horo::Runtime

namespace Horo::Application::Internal {
    struct HeadlessSceneLoadBoundary;

    /** @brief Exact authored occurrence and independently pinned cooked CoreEcs cell identity. */
    struct HeadlessWorldInitialCell final {
        WorldStreaming::StreamingCellId cell;
        Runtime::SceneInstanceId occurrence;
        Runtime::SceneDefinitionId definition;
        Runtime::SceneDefinitionRevision revision;
    };

    /** @brief Exact project launch selection for one separate generated World cook generation. */
    struct HeadlessWorldSelection final {
        std::filesystem::path targetRoot;
        Assets::AssetCookTargetId target;
        Sha256Digest manifest;
        Assets::AssetId index;
        Assets::AssetId dependencies;
        Math::WorldCoordinate64 origin; /**< Explicit authoritative fixed-point startup origin in Horo axes. */
        std::optional<HeadlessWorldInitialCell> initialCell;
    };

    /** @brief Retains generation-fenced renderer output and the actual immutable procedural resources it references. */
    class HeadlessWorldRenderPacket final {
    public:
        HeadlessWorldRenderPacket(HeadlessWorldRenderPacket &&) noexcept = default;
        HeadlessWorldRenderPacket(const HeadlessWorldRenderPacket &) = delete;
        HeadlessWorldRenderPacket &operator=(const HeadlessWorldRenderPacket &) = delete;

        /** @brief Borrows the complete scene while the source Scene and renderer origin are current.
         * @return Current renderer packet or original stale owner error.
         */
        [[nodiscard]] Result<Render::RenderSceneView> Scene() const;

    private:
        friend class HeadlessWorldMount;
        HeadlessWorldRenderPacket(Runtime::RuntimeSceneView parent, Render::RenderOriginFrame frame,
                                  std::vector<Runtime::PrimitiveMeshLease> leases,
                                  std::vector<Render::RenderMeshResourceView> resources) noexcept;

        Runtime::RuntimeSceneView parent_;
        Render::RenderOriginFrame frame_;
        std::vector<Runtime::PrimitiveMeshLease> leases_;
        std::vector<Render::RenderMeshResourceView> resources_;
    };

    /** @brief Private candidate and active World owners retained by the actual standalone Scene record. */
    class HeadlessWorldMount final {
    public:
        /** @brief Prepare one verified generation, owned registry, origin, planner and resident-state authority.
         * @param files Explicit host filesystem, borrowed only for cold admission.
         * @param selection Exact project launch selection.
         * @param generation Non-zero unique owner-lane generation of the receiving Scene record.
         * @param scene Real runtime Scene service that outlives the mount.
         * @param cancellation Parent preparation cancellation.
         * @return Complete owned mount or typed error. No authority escapes on failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<HeadlessWorldMount>> Prepare(const DurableFileSystem &files,
                                                                                 const HeadlessWorldSelection &selection,
                                                                                 std::uint64_t generation,
                                                                                 Runtime::RuntimeSceneService &scene,
                                                                                 const CancellationToken &cancellation = {});

        /** @brief Activate one independent, pinned CoreEcs cell through the private live parent Scene.
         * @param selected Exact authored occurrence and cooked cell identity from host policy.
         * @param boundary Existing cold startup/travel absolute deadline and parent cancellation.
         * @return Success only after the Scene attachment receipt and Active residency agree.
         * @pre The parent is privately prepared. On a post-queue failure, it is shut down before retiring scheduler work.
         */
        [[nodiscard]] Result<void> ActivateInitialCell(const HeadlessWorldInitialCell &selected, const HeadlessSceneLoadBoundary &boundary);

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
         * @return Complete cold renderer packet or a typed absence/stale/unsupported/capacity failure.
         * @details Admits root objects only. World positions remain signed millimeter anchors. Local rotation and scale carry no
         * global translation authority. A local position edit since cell admission requires a new World position publication.
         */
        [[nodiscard]] Result<HeadlessWorldRenderPacket> CaptureGeometry(const Runtime::RuntimeSceneView &parent,
                                                                        Runtime::SceneObjectId camera,
                                                                        std::span<const Runtime::SceneObjectId> meshes,
                                                                        Runtime::PrimitiveMeshCache &primitives,
                                                                        Render::RenderOriginSceneAdapter &renderer) const;

        /** @brief Observe the exact mounted World frame; the caller keeps this complete mount alive. */
        [[nodiscard]] const WorldStreaming::OriginFrameOwner &Origin() const noexcept;
        /** @brief Revoke mounted authority and close the registry before releasing the borrowed Scene service. */
        ~HeadlessWorldMount();

    private:
        struct State;
        explicit HeadlessWorldMount(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Application::Internal
