#pragma once
#include "Horo/Runtime/Scene/PersistentEntityIdentity.h"
#include "PhysicsSceneActivationInternal.h"
#include "PhysicsWorldStateRestartAccess.h"

namespace Horo::Physics::Detail {
    using SceneStateKeyRole = PhysicsStateOwnerRole;

    /** @brief Complete durable ownership key; local Scene/native handles and truncated UUID hashes are excluded. */
    struct SceneStateKey final {
        Runtime::PersistentEntityId entity;
        Runtime::PersistentEntityGeneration generation;
        SceneStateKeyRole role{};
        std::uint64_t slot{};
        auto operator<=>(const SceneStateKey &) const noexcept = default;
    };

    struct SceneStateKeyBinding final {
        SceneStateKey key;
        BodyHandle body;
        ShapeHandle shape;
        ConstraintHandle constraint;
        BodyHandle ownerBody;
    };

    struct SceneStateProjectionLimits final {
        std::size_t maximumBindings{}, maximumRetainedBytes{};
    };

    /** @brief Cold owner-built full-key bijection covering EVERY native Scene resource exactly once.
     * Same sorted complete key order issues canonical IDs for source and receiving owner. No wire ID can invent a resource/key.
     * Compound body roots have an explicit body-key role; unowned query fixtures/orphan resources and ambiguous aliases reject.
     * This private Scene adapter supports its actual unique collider-resource ownership; generic Physics codec sharing remains intact.
     */
    class PhysicsSceneStateProjection final {
        friend class PhysicsSceneStateTarget;
        friend class PreparedPhysicsSceneCapture;
        friend class PreparedPhysicsSceneRestore;

    public:
        [[nodiscard]] std::span<const SceneStateKeyBinding> Keys() const noexcept {
            return rows_;
        }

        [[nodiscard]] const CanonicalRestartBindings &Bindings() const noexcept {
            return bindings_;
        }

        [[nodiscard]] std::span<const CanonicalRestartBodyRoute> Routes() const noexcept {
            return routes_;
        }

        [[nodiscard]] Sha256Digest ContentFingerprint(const Sha256Digest &authenticatedContent) const noexcept;

        [[nodiscard]] std::span<const PhysicsStatePersistentOwner> OwnerRecords() const noexcept {
            return owners_;
        }

        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;
        [[nodiscard]] PhysicsStateObjectId BodyId(BodyHandle) const noexcept;
        [[nodiscard]] PhysicsStateObjectId ShapeId(ShapeHandle) const noexcept;
        [[nodiscard]] PhysicsStateObjectId ConstraintId(ConstraintHandle) const noexcept;

    private:
        [[nodiscard]] static Result<PhysicsSceneStateProjection> Build(const PhysicsWorld &, const PhysicsStructuralState &,
                                                                       const Runtime::PersistentEntityIdentityMap &,
                                                                       SceneStateProjectionLimits);
        [[nodiscard]] Result<void> PrepareProjection(const PhysicsWorld &, const PhysicsStructuralState &,
                                                     const Runtime::PersistentEntityIdentityMap &, const CanonicalStateSceneCounts &);
        [[nodiscard]] Result<void> ReadInventory(const PhysicsWorld &, const CanonicalStateSceneCounts &);
        [[nodiscard]] Result<void> AddOwners(const PhysicsStructuralState &, const Runtime::PersistentEntityIdentityMap &);
        [[nodiscard]] Result<void> AddCompoundRoots();
        void IndexInitialOwners();
        [[nodiscard]] Result<void> BindCanonicalIds();
        [[nodiscard]] Result<void> BindRow(PhysicsStateObjectId, const SceneStateKeyBinding &);
        [[nodiscard]] Result<void> CheckCoverage();
        void PrepareCoverage();
        std::vector<SceneStateKeyBinding> rows_;
        std::vector<CanonicalStateSceneBody> nativeBodies_;
        std::vector<ShapeHandle> nativeShapes_;
        std::vector<ConstraintHandle> nativeConstraints_;
        CanonicalRestartBindings bindings_;
        std::vector<CanonicalRestartBodyRoute> routes_;
        std::vector<PhysicsStateObjectId> idByNative_;
        std::vector<PhysicsStatePersistentOwner> owners_;
    };
}  // namespace Horo::Physics::Detail
