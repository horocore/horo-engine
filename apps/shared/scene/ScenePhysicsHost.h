#pragma once

/** @file ScenePhysicsHost.h
 * @brief Non-installed application observer of the single Scene-owned Physics and Character aggregate.
 */
#include "PhysicsSceneHostAccess.h"

#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

namespace Horo::Runtime {
    struct FixedStepContext;
    class SceneAnimationRuntime;
}  // namespace Horo::Runtime

namespace Horo::Application::Internal {
    /** @brief Parses explicit application Physics policy without backend discovery or fallback.
     * @param name Exact startup value: null or canonical. @return Selected typed mode, or absence for invalid input.
     */
    [[nodiscard]] std::optional<Physics::PhysicsRuntimeMode> ParseScenePhysicsHostMode(std::string_view name) noexcept;
    /** @brief Returns the exact startup spelling for a validated host mode.
     * @param mode Selected typed mode. @return Empty only for an invalid enum representation.
     */
    [[nodiscard]] std::string_view ScenePhysicsHostModeName(Physics::PhysicsRuntimeMode mode) noexcept;

    /** @brief Converts an explicit UTF-8 startup root to a native absolute path without discovering authority.
     * @param value User-supplied UTF-8 root. @return Lexically normalized native absolute path or typed invalid-path error.
     * @details Performs no file I/O, backend selection or permission grant; native root admission follows separately.
     */
    [[nodiscard]] Result<std::filesystem::path> ParseSceneHostAbsolutePath(std::string_view value);

    /** @brief Explicit host-reserved identity range, checked before any native preparation.
     * @details Independent Scene services must receive disjoint ranges or a single exact identity.
     * This is admission evidence, not a global identity allocator.
     */
    struct ScenePhysicsIdentityRange final {
        Runtime::SceneRuntimeId first{1};
        Runtime::SceneRuntimeId last{std::numeric_limits<std::uint64_t>::max()};
    };

    /** @brief Owner-lane application access withdrawn before aggregate rollback, replacement or shutdown.
     * @details This observer owns no native world. It must outlive its registered participant and every
     * candidate produced by that participant. Declare it before the owning RuntimeSceneService.
     */
    class ScenePhysicsHost final {
        class Candidate;
        class Participant;
        struct AnimationFixedState;

    public:
        /** @brief Binds the synchronous observer to the constructing application owner lane. */
        explicit ScenePhysicsHost(ScenePhysicsIdentityRange identities = {});
        ~ScenePhysicsHost();
        ScenePhysicsHost(const ScenePhysicsHost &) = delete;
        ScenePhysicsHost &operator=(const ScenePhysicsHost &) = delete;
        ScenePhysicsHost(ScenePhysicsHost &&) = delete;
        ScenePhysicsHost &operator=(ScenePhysicsHost &&) = delete;

        /** @brief Creates the one InitialPlacement participant for explicit host-selected Physics.
         * @param runtime Borrowed selected runtime that outlives the Scene service.
         * @param authority Borrowed project evidence authority that outlives the Scene service.
         * @param settings Captured immutable world/project settings; no backend discovery is performed.
         * @return Owned participant for registration before Scene startup, or typed wrong-lane/duplicate-owner rejection.
         */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneActivationParticipant>> MakeParticipant(
            Physics::PhysicsRuntime &runtime, Physics::PhysicsSceneActivationAuthority &authority,
            Physics::PhysicsSceneActivationSettings settings);

        /** @brief Creates the structural adapter from the same registered Physics owner before Scene startup.
         * @return Owned adapter or typed wrong-lane/missing-participant rejection.
         * @pre The participant returned by MakeParticipant remains owned by the Scene service.
         */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralParticipant>> MakeStructuralParticipant();

        /** @brief Observes actual aggregate publication on the owner lane without granting a borrow.
         * @return True only after real Publish and before revocation; false on a foreign lane.
         */
        [[nodiscard]] bool HasPublishedScene() const noexcept;

        /** @brief Borrows the exact published pair synchronously on the owner lane.
         * @return Original typed affinity, lifecycle or evidence rejection, or a scope valid only for this call's owner operation.
         * @pre Do not retain the scope in jobs, callbacks, modules or through Scene lifecycle operations.
         */
        [[nodiscard]] Result<Physics::Detail::PhysicsSceneHostScope> Borrow();
        /** @brief Resolves the exact controller for one enabled actor in the published Scene.
         * @param scene Current committed Scene view. @param actor Exact entity generation.
         * @return Controller handle or the original scene, actor or native binding failure.
         */
        [[nodiscard]] Result<Character::CharacterControllerHandle> ResolveCharacterController(const Runtime::RuntimeSceneView &scene,
                                                                                              Runtime::EntityRef actor);
        /** @brief Reads achieved world-space velocity from the selected controller's latest completed tick.
         * @param scene Exact published Scene incarnation. @param object Authored actor. @param component Its selected controller.
         * @return Velocity in metres per second or the original admission/snapshot error.
         */
        [[nodiscard]] Result<Math::Vec3> ReadCharacterVelocity(Runtime::SceneRuntimeId scene, Runtime::SceneObjectId object,
                                                               Runtime::PhysicsComponentId component);
        /** @brief Copies the Character-owned collision root in the Scene's local transform convention.
         * @param scene Exact committed Scene view. @param entity Its bound Character actor.
         * @param schedulerTick Exact committed tick being published by the caller.
         * @return Local position and heading with authored scale, or a typed stale/conversion failure.
         */
        [[nodiscard]] Result<Math::Transform> ReadCharacterLocalTransform(const Runtime::RuntimeSceneView &scene,
                                                                          const Runtime::RuntimeEntityView &entity,
                                                                          std::uint64_t schedulerTick);
        /** @brief Copies solved Character motion for the current scheduler attempt before replay publication.
         * @param controller Exact live controller from this Scene's paired Character owner.
         * @param context Current fixed attempt after the paired native solver succeeds.
         * @return An owned result, no movement this tick, or a typed stale/lifecycle failure.
         */
        [[nodiscard]] Result<std::optional<Character::CharacterMovementResult>> ReadCharacterFixedMovement(
            Character::CharacterControllerHandle controller, const Runtime::FixedStepContext &context);
        /** @brief Returns only the actual published Physics candidate's issued state target. */
        [[nodiscard]] Result<Physics::Detail::PhysicsSceneStateTarget> PublishedStateTarget();
        /** @brief Returns only the exact actual pending candidate for this receiving Scene; no Scene-ID-only authority. */
        [[nodiscard]] Result<Physics::Detail::PhysicsSceneStateTarget> PendingStateTarget(Runtime::SceneRuntimeId expected);

        /** @brief Advances the explicitly paired Character-before-Physics stages for one ordinary host quantum.
         * @param delta Exact immutable positive fixed quantum. @return Original query/Character/Physics failure.
         * @details Uses the previous completed Physics publication for Character queries; never used to advance loading.
         * Explicit Null composition omits both stages without publishing a Physics or Character tick.
         */
        [[nodiscard]] Result<void> AdvanceFixedTick(Duration delta);

        /** @brief Stages authoritative Animation requests through the exact Scene-owned Character solver.
         * @param scene Current Scene projection of this pair. @param animation Exact Animation owner of that scene.
         * @param context Actual scheduler attempt, after Animation evaluation. @return Typed staging failure or success.
         */
        [[nodiscard]] Result<void> StageAnimationFixedTick(const Runtime::RuntimeSceneView &scene,
                                                           Runtime::SceneAnimationRuntime &animation,
                                                           const Runtime::FixedStepContext &context);
        /** @brief Runs the native Physics step only after fallible Animation and Character preparation.
         * @param context Same scheduler attempt. @return Typed Physics or lifecycle failure.
         */
        [[nodiscard]] Result<void> PrepareAnimationFixedCommit(const Runtime::FixedStepContext &context);
        /** @brief Publishes the staged Character result after the scheduler commitment fence. */
        void CommitAnimationFixedTick(const Runtime::FixedStepContext &context) noexcept;
        /** @brief Discards staged Character movement and fences the Scene if native Physics was already attempted. */
        void AbortAnimationFixedTick(const Runtime::FixedStepContext &context) noexcept;

    private:
        ScenePhysicsIdentityRange identities_;
        std::thread::id owner_{std::this_thread::get_id()};
        Participant *participant_{};
        Physics::PhysicsSceneActivationCandidate *published_{};
        Runtime::RuntimeSceneView publishedScene_;
        Physics::PhysicsSceneActivationCandidate *pending_{};
        std::unique_ptr<AnimationFixedState> fixedAnimation_;
    };
}  // namespace Horo::Application::Internal
