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
        BodyHandle body; /**< Resident body owning this collider shape. */
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
        /** @brief Returns current body bindings; the span is invalidated by quarantine or scene retirement. */
        [[nodiscard]] std::span<const PhysicsSceneBodyBinding> BodyBindings() const noexcept;
        /** @brief Returns current collider-shape bindings; quarantine retires bindings owned by the affected body. */
        [[nodiscard]] std::span<const PhysicsSceneShapeBinding> ShapeBindings() const noexcept;
        /** @brief Returns current constraint bindings; quarantine retires bindings attached to the affected body. */
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

        [[nodiscard]] static std::unique_ptr<PhysicsSceneActivationCandidate> Create(ConstructionData data);

        std::unique_ptr<PhysicsWorld> physics_;
        std::unique_ptr<Character::CharacterWorld> character_;
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
}  // namespace Horo::Physics
