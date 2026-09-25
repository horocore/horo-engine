#pragma once

/** @file PhysicsSceneActivation.h
 * @brief Physics-owned aggregate scene activation participant and Character-world lifetime.
 */

#include "Horo/Physics/CharacterWorldSettings.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace Horo::Character {
    class CharacterWorld;
}

namespace Horo::Physics {
    /** @brief Exact collision-filter and local-origin evidence captured for one scene candidate. */
    struct PhysicsSceneActivationEvidence final {
        std::uint64_t collisionFilterGeneration{}; /**< Exact authoritative filter generation. */
        std::uint64_t originGeneration{};          /**< Exact authoritative local-origin generation. */

        [[nodiscard]] constexpr auto operator<=>(const PhysicsSceneActivationEvidence &) const noexcept = default;
    };

    /** @brief Application-composition authority for generations shared by Scene, Physics, and Character. */
    class PhysicsSceneActivationAuthority final {
    public:
        /** @brief Starts both process-local generation streams at their first valid value. */
        PhysicsSceneActivationAuthority() noexcept;
        /** @brief Captures coherent evidence for a detached candidate. @return Current paired generation snapshot. */
        [[nodiscard]] PhysicsSceneActivationEvidence Capture() const noexcept;
        /** @brief Checks that candidate evidence still matches the authoritative generations.
         * @param evidence Previously captured paired generation snapshot. @return True only on the owner thread while exact. */
        [[nodiscard]] bool IsCurrent(PhysicsSceneActivationEvidence evidence) const noexcept;
        /** @brief Advances collision-filter evidence after an authoritative schema replacement. @return Success or affinity/exhaustion
         * error. */
        [[nodiscard]] Result<void> AdvanceCollisionFilterGeneration();
        /** @brief Advances local-origin evidence after an authoritative origin replacement. @return Success or affinity/exhaustion error.
         */
        [[nodiscard]] Result<void> AdvanceOriginGeneration();

    private:
        /** @brief Advances one owner-thread generation without wrapping. */
        [[nodiscard]] Result<void> Advance(std::uint64_t &generation) const;

        std::thread::id ownerThread_;
        PhysicsSceneActivationEvidence current_{1, 1};
    };

    /** @brief Immutable policy pinned by one Physics scene-activation participant. */
    struct PhysicsSceneActivationSettings final {
        PhysicsWorldSettings physics;
        Character::CharacterWorldSettings character;
    };

    /** @brief Stable authored-body to resident runtime-body binding retained by one scene candidate. */
    struct PhysicsSceneBodyBinding final {
        Runtime::SceneObjectId object;
        Runtime::PhysicsBodySlotId body;
        BodyHandle handle;
    };

    /** @brief Stable authored-collider to resident runtime-shape binding retained by one scene candidate. */
    struct PhysicsSceneShapeBinding final {
        Runtime::SceneObjectId object;
        Runtime::PhysicsColliderSlotId collider;
        ShapeHandle handle;
    };

    /** @brief Stable authored-constraint to resident runtime-constraint binding retained by one scene candidate. */
    struct PhysicsSceneConstraintBinding final {
        Runtime::SceneObjectId object;
        Runtime::PhysicsConstraintSlotId constraint;
        ConstraintHandle handle;
    };

    /**
     * @brief Owns the fully staged Physics and Character state for one unpublished or active scene generation.
     *
     * Native bodies, shapes and constraints are owned by the paired PhysicsWorld. Binding tables are
     * published only with this candidate and are cleared during rollback or retirement; authored IDs
     * never become native handles and no partial table is returned from Prepare.
     */
    class PhysicsSceneActivationCandidate final : public Runtime::SceneActivationCandidate {
        struct ConstructionData final {
            std::unique_ptr<PhysicsWorld> physics;
            std::unique_ptr<Character::CharacterWorld> character;
            const PhysicsRuntime *runtime{};
            const PhysicsSceneActivationAuthority *authority{};
            PhysicsSceneActivationEvidence evidence;
            std::vector<PhysicsSceneBodyBinding> bodyBindings;
            std::vector<PhysicsSceneShapeBinding> shapeBindings;
            std::vector<PhysicsSceneConstraintBinding> constraintBindings;
        };

    public:
        ~PhysicsSceneActivationCandidate() override;

        /** @brief Takes one fully staged aggregate; only the participant can form the private payload. */
        explicit PhysicsSceneActivationCandidate(ConstructionData data) noexcept;

        /** @copydoc Runtime::SceneActivationCandidate::ValidatePublication */
        [[nodiscard]] Result<void> ValidatePublication() const override;
        /** @copydoc Runtime::SceneActivationCandidate::Shutdown */
        void Shutdown() noexcept override;

        /** @brief Returns the exact world generation owned by this candidate. */
        [[nodiscard]] PhysicsWorldId WorldIdentity() const noexcept;
        /** @brief Returns stable body bindings retained by this candidate. */
        [[nodiscard]] std::span<const PhysicsSceneBodyBinding> BodyBindings() const noexcept;
        /** @brief Returns stable collider-shape bindings retained by this candidate. */
        [[nodiscard]] std::span<const PhysicsSceneShapeBinding> ShapeBindings() const noexcept;
        /** @brief Returns stable constraint bindings retained by this candidate. */
        [[nodiscard]] std::span<const PhysicsSceneConstraintBinding> ConstraintBindings() const noexcept;
        /** @brief Resolves an authored body binding without allocating or crossing scene generations. */
        [[nodiscard]] std::optional<BodyHandle> FindBody(Runtime::SceneObjectId object, Runtime::PhysicsBodySlotId body) const noexcept;
        /** @brief Resolves an authored collider-shape binding without allocating. */
        [[nodiscard]] std::optional<ShapeHandle> FindShape(Runtime::SceneObjectId object,
                                                           Runtime::PhysicsColliderSlotId collider) const noexcept;
        /** @brief Resolves an authored constraint binding without allocating. */
        [[nodiscard]] std::optional<ConstraintHandle> FindConstraint(Runtime::SceneObjectId object,
                                                                     Runtime::PhysicsConstraintSlotId constraint) const noexcept;

    private:
        friend class PhysicsSceneActivationParticipant;
        friend class PhysicsPlayWorldSession;

        [[nodiscard]] static std::unique_ptr<PhysicsSceneActivationCandidate> Create(ConstructionData data);

        std::unique_ptr<PhysicsWorld> physics_;
        std::unique_ptr<Character::CharacterWorld> character_;
        const PhysicsRuntime *runtime_{};
        const PhysicsSceneActivationAuthority *authority_{};
        PhysicsSceneActivationEvidence evidence_;
        std::vector<PhysicsSceneBodyBinding> bodyBindings_;
        std::vector<PhysicsSceneShapeBinding> shapeBindings_;
        std::vector<PhysicsSceneConstraintBinding> constraintBindings_;
    };

    /** @brief Prepares and retires paired Physics and Character worlds through RuntimeScene's aggregate boundary. */
    class PhysicsSceneActivationParticipant final : public Runtime::SceneActivationParticipant {
    public:
        /** @brief Binds one process Physics owner, shared generation authority, and immutable activation policy.
         * @param runtime Physics runtime that outlives this participant and its candidates.
         * @param authority Generation authority that outlives this participant and its candidates.
         * @param settings Immutable world policies captured by value. */
        PhysicsSceneActivationParticipant(PhysicsRuntime &runtime, PhysicsSceneActivationAuthority &authority,
                                          PhysicsSceneActivationSettings settings) noexcept;

        /** @copydoc Runtime::SceneActivationParticipant::Prepare */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                                         Runtime::RuntimeSceneView scene) override;

    private:
        PhysicsRuntime *runtime_{};
        PhysicsSceneActivationAuthority *authority_{};
        PhysicsSceneActivationSettings settings_;
    };

    /**
     * @brief Physics-owned play world detached from the authored scene and any edit-preview world.
     *
     * The host owns the source RuntimeScene and keeps the PhysicsRuntime alive longer than this session.
     * Prepare copies all required Physics intent into a private candidate; it retains no authoring
     * document or RuntimeSceneView. Publication, replacement and stop are owner-thread operations at
     * RuntimePhase::CommitDeferredLifecycleChanges, after all fixed ticks and queries have drained.
     * All session access, including binding queries and destruction, stays on that owner thread.
     * Reload always creates a fresh world; solver velocity, sleep and contact state are not transferred.
     */
    class PhysicsPlayWorldSession final {
    public:
        /** @brief Binds one process runtime and immutable world policies without creating a world.
         * @param runtime Runtime that outlives this session.
         * @param settings Settings copied for each detached world candidate. */
        PhysicsPlayWorldSession(PhysicsRuntime &runtime, PhysicsSceneActivationSettings settings) noexcept;
        /** @brief Retires active and pending worlds on the owner thread after fixed work has drained. */
        ~PhysicsPlayWorldSession();
        PhysicsPlayWorldSession(const PhysicsPlayWorldSession &) = delete;
        PhysicsPlayWorldSession &operator=(const PhysicsPlayWorldSession &) = delete;

        /** @brief Stages a new isolated world from a matching immutable definition and resolved scene view.
         * @param definition Authored snapshot used to instantiate scene; remains caller-owned.
         * @param scene Exact resolved scene generation corresponding to definition.
         * @return Success with an unpublished candidate, or a typed error preserving the active world.
         * @pre Owner thread between fixed ticks; only one pending candidate is allowed. */
        [[nodiscard]] Result<void> Prepare(const Runtime::RuntimeSceneDefinition &definition, Runtime::RuntimeSceneView scene);
        /** @brief Publishes the pending world and retires the old one at the host lifecycle safe point.
         * @param phase Current host phase; only CommitDeferredLifecycleChanges is admitted.
         * @param source Original scene view supplied to Prepare; its owner must remain alive until this call.
         * @return Success or a typed error; failed revalidation leaves the old world active. */
        [[nodiscard]] Result<void> Commit(Runtime::RuntimePhase phase, Runtime::RuntimeSceneView source);
        /** @brief Closes play admission and retires both worlds at the host lifecycle safe point.
         * @param phase Current host phase; only CommitDeferredLifecycleChanges is admitted.
         * @return Success, including repeated stop, or a typed phase/affinity error. */
        [[nodiscard]] Result<void> Stop(Runtime::RuntimePhase phase);
        /** @brief Advances only the active Physics world for one exact host fixed tick.
         * @param input Tick and scene generation supplied by the host.
         * @return Solver result or a typed lifecycle/generation error; this does not write scene transforms. */
        [[nodiscard]] Result<void> AdvanceFixedTick(const PhysicsFixedTickInput &input);
        /** @brief Reads the active world's last complete Physics tick without exposing solver ownership.
         * @return Publication marker or a typed owner-thread/inactive error. */
        [[nodiscard]] Result<PhysicsPublishedTick> PublishedTick() const;
        /** @brief Resolves a stable authored body against the active play world.
         * @param object Stable authored object identity.
         * @param body Stable authored body slot.
         * @return Current handle or a typed inactive/missing-binding error. */
        [[nodiscard]] Result<BodyHandle> ResolveBody(Runtime::SceneObjectId object, Runtime::PhysicsBodySlotId body) const;
        /** @brief Rejects retained handles from another or retired play-world generation.
         * @param handle Non-owning body handle to validate.
         * @return Success only for an active bound body, or a typed stale/world/state error. */
        [[nodiscard]] Result<void> ValidateBody(const BodyHandle &handle) const;
        /** @brief Returns the active world generation, or invalid when stopped/unpublished. */
        [[nodiscard]] PhysicsWorldId WorldIdentity() const noexcept;
        /** @brief Reports whether a complete play world has been published. */
        [[nodiscard]] bool IsActive() const noexcept;
        /** @brief Reports whether a start or reload candidate awaits the host safe point. */
        [[nodiscard]] bool HasPendingCandidate() const noexcept;

    private:
        /** @brief Rejects mutation or binding access outside the session owner thread. */
        [[nodiscard]] Result<void> CheckOwner() const;
        /** @brief Requires the exact host lifecycle publication phase on the owner thread. */
        [[nodiscard]] Result<void> CheckSafePoint(Runtime::RuntimePhase phase) const;
        /** @brief Shuts down and releases one detached or published aggregate. */
        void Retire(std::unique_ptr<Runtime::SceneActivationCandidate> &candidate) noexcept;
        /** @brief Casts the known Physics participant's published candidate to its concrete type. */
        [[nodiscard]] const PhysicsSceneActivationCandidate *ActivePhysics() const noexcept;
        /** @copydoc ActivePhysics */
        [[nodiscard]] PhysicsSceneActivationCandidate *ActivePhysics() noexcept;

        std::thread::id ownerThread_;
        PhysicsSceneActivationAuthority authority_;
        PhysicsSceneActivationParticipant participant_;
        std::unique_ptr<Runtime::SceneActivationCandidate> pending_;
        std::unique_ptr<Runtime::SceneActivationCandidate> active_;
        Runtime::SceneRuntimeId pendingScene_;
        Runtime::SceneDefinitionId pendingDefinition_;
        Runtime::SceneDefinitionRevision pendingRevision_;
        Assets::AssetRegistryRevision pendingAssets_;
        Runtime::SceneRuntimeId activeScene_;
    };
}  // namespace Horo::Physics
