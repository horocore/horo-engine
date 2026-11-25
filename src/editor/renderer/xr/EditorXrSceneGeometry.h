#pragma once
/** @file EditorXrSceneGeometry.h
 * @brief Non-installed camera-free canonical geometry handoff from Workspace into the actual Play Scene.
 */
#include "Horo/Runtime/Render/MotionHistory.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <memory>

namespace Horo::Runtime {
    class PrimitiveMeshCache;
}

namespace Horo::Editor {
    class SceneDocument;
    class EditorAssetMeshCache;

    namespace XrSceneDetail {
        struct SourceState;
    }

    /** @brief An authored draw hit in the current Play presentation. */
    struct EditorXrGeometryHit final {
        Runtime::SceneObjectId object;
        float distanceMeters{};
        Math::Vec3 worldPosition;
    };

    namespace XrSceneDetail {
        /** @brief Intersects one bounded table of already admitted draws and corresponding authored identities.
         * @param ray Finite world-space segment.
         * @param instances Prepared draw transforms and mesh handles.
         * @param objects Draw identities in the same order as instances.
         * @param meshes Validated and retained CPU mesh resources.
         * @param output Caller-owned storage for complete results.
         * @return Ordered hit count or an error that preserves output. */
        [[nodiscard]] Result<std::size_t> QueryPointingDraws(const Math::Ray &ray,
                                                             std::span<const Render::RenderStaticMeshInstance> instances,
                                                             std::span<const Render::RenderMotionObjectId> objects,
                                                             std::span<const Render::RenderMeshResourceView> meshes,
                                                             std::span<EditorXrGeometryHit> output);
    }  // namespace XrSceneDetail

    /** @brief Owned exact Scene mapping and mesh leases, with cold bounded topology and allocation-free updates. */
    class PreparedEditorXrGeometry final {
        struct Impl;

    public:
        /** @brief Factory-only authority for adopting validated geometry through standard RAII allocation. */
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class EditorXrSceneSourcePort;
        };

        /** @brief Adopts complete cold geometry; only the capturing source port can issue the key. */
        PreparedEditorXrGeometry(const ConstructionKey &key, std::unique_ptr<Impl> impl) noexcept;
        static constexpr std::size_t MaximumInstances = 128;
        static constexpr std::size_t MaximumSceneSlots = 4096;
        ~PreparedEditorXrGeometry();
        PreparedEditorXrGeometry(const PreparedEditorXrGeometry &) = delete;
        PreparedEditorXrGeometry &operator=(const PreparedEditorXrGeometry &) = delete;
        /** @brief Revalidates workspace lane/lifetime and exact committed document revision before publication.
         * @return Original typed stale/unavailable failure; no borrowed document pointer escapes.
         */
        [[nodiscard]] Result<void> ValidateSource() const;
        /** @brief Borrows owned immutable CPU resources through renderer cold preparation.
         * @return Canonical source generations pinned independently of cache eviction and workspace retirement.
         */
        [[nodiscard]] std::span<const Render::RenderMeshResourceView> Resources() const noexcept;
        /** @brief Borrows the most recent fully validated owned presentation values; no Scene/document pointer escapes.
         * @return Owner-lane borrow invalidated by Update/Revoke/destruction. Revalidate source before frame admission.
         */
        [[nodiscard]] std::span<const Render::RenderStaticMeshInstance> Instances() const noexcept;
        /** @brief Borrows canonical object identities in the same exact order as Instances through this preparation's lifetime. */
        [[nodiscard]] std::span<const Render::RenderMotionObjectId> MotionObjects() const noexcept;
        /** @brief Intersects the latest committed authored draws with a finite World ray.
         * @param ray World-space segment in the same coordinate basis as the draws.
         * @param output Caller-owned space for one nearest triangle hit per intersected object.
         * @return Complete hit count, ordered by distance and then identity, or a source, geometry or capacity failure.
         * @post Failure leaves output unchanged. This query is intended for an explicit pointing press. */
        [[nodiscard]] Result<std::size_t> QueryPointing(const Math::Ray &ray, std::span<EditorXrGeometryHit> output) const;
        /** @brief Updates fixed instance values against the exact candidate Scene and cached parent order.
         * @param scene SAME SceneRuntimeId and entity generations captured cold.
         * @return Synchronous instance borrow, or typed stale/structural/source/transform failure with no partial update.
         * @post No heap, hierarchy search, camera, native call or retained Scene/component pointer. Borrow ends on
         * Update/Revoke/destruction.
         */
        [[nodiscard]] Result<std::span<const Render::RenderStaticMeshInstance>> Update(const Runtime::RuntimeSceneView &scene);
        /** @brief Closes future source/transform reads without discarding owned mesh/native retirement leases. */
        void Revoke() noexcept;

    private:
        friend class EditorXrSceneSourcePort;
        std::unique_ptr<Impl> impl_;
    };

    /** @brief Weak, attachment-owned preparation port; it cannot publish a Scene or load another asset authority. */
    class EditorXrSceneSourcePort final {
    public:
        EditorXrSceneSourcePort() = default;
        /** @brief Captures both primitive/imported geometry from the SAME canonical Workspace owners.
         * @param revision Exact definition revision passed through the actual Play factory.
         * @param candidate Exact unpublished Scene whose authored objects must all resolve to generation-safe EntityRef values.
         * @return Owned bounded preparation, or atomic original missing/stale/capacity/mesh/mapping failure.
         * @pre Creating owner lane; no source/document/cache borrow escapes. Missing imported meshes never silently disappear.
         * @throws std::bad_alloc For bounded cold ownership/topology storage only.
         */
        [[nodiscard]] Result<std::unique_ptr<PreparedEditorXrGeometry>> Capture(Runtime::SceneDefinitionRevision revision,
                                                                                const Runtime::RuntimeSceneView &candidate) const;

    private:
        friend class EditorXrSceneSourceOwner;
        std::weak_ptr<XrSceneDetail::SourceState> state_;
    };

    /** @brief Workspace-owned fence over its real document/caches; destruction revokes before those parents die.
     * @details Declare after the document and caches. Shared fence storage never owns or prolongs those parents.
     * All capture/update/destruction run on the creating lane. Source edits are checked against the document's
     * existing revision/state authority; reload/leave/cancellation explicitly revoke retained prepared work.
     */
    class EditorXrSceneSourceOwner final {
    public:
        /** @brief Borrows the existing canonical owners; no loading, source publication or backend selection occurs. */
        EditorXrSceneSourceOwner(const SceneDocument &document, Runtime::PrimitiveMeshCache &primitives,
                                 const EditorAssetMeshCache &assets);
        ~EditorXrSceneSourceOwner();
        EditorXrSceneSourceOwner(const EditorXrSceneSourceOwner &) = delete;
        EditorXrSceneSourceOwner &operator=(const EditorXrSceneSourceOwner &) = delete;
        /** @brief Returns the same weak preparation port, without pinning the Controller/document/caches. */
        [[nodiscard]] EditorXrSceneSourcePort Port() const noexcept;

    private:
        std::shared_ptr<XrSceneDetail::SourceState> state_;
    };
}  // namespace Horo::Editor
