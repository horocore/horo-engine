#pragma once

/** @file PhysicsSceneActivation.h
 * @brief Physics-owned aggregate scene activation participant and Character-world lifetime.
 */

#include "Horo/Physics/CharacterWorldSettings.h"
#include "Horo/Physics/PhysicsQuery.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

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

    /**
     * @brief Backend-neutral runtime binding for one legacy authored trigger volume.
     *
     * The object identity remains authored-scene evidence while the fixture descriptor is an
     * inert, scale-free Physics admission value. No field owns a native solver resource.
     */
    struct PhysicsTriggerVolumeBinding final {
        Runtime::SceneObjectId object;
        PhysicsQueryFixtureDescriptor fixture;
    };

    /**
     * @brief Converts legacy trigger-volume metadata into deterministic analytic sensor descriptors.
     * @param definition Validated immutable scene definition containing legacy trigger components.
     * @return Bindings sorted by authored object identity, or a stable conversion/capacity error.
     * @pre Conversion runs before Physics-world publication and performs no native work.
     * @post Disabled trigger components produce no binding; enabled components contain explicit
     * geometry, world pose, filter identities, and sensor admission policy.
     */
    [[nodiscard]] Result<std::vector<PhysicsTriggerVolumeBinding>> BuildPhysicsTriggerVolumeBindings(
        const Runtime::RuntimeSceneDefinition &definition);

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
