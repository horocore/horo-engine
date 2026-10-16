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

    public:
        /** @brief Binds the synchronous observer to the constructing application owner lane. */
        explicit ScenePhysicsHost(ScenePhysicsIdentityRange identities = {});
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

        /** @brief Advances the explicitly paired Character-before-Physics stages for one ordinary host quantum.
         * @param delta Exact immutable positive fixed quantum. @return Original query/Character/Physics failure.
         * @details Uses the previous completed Physics publication for Character queries; never used to advance loading.
         */
        [[nodiscard]] Result<void> AdvanceFixedTick(Duration delta);

    private:
        ScenePhysicsIdentityRange identities_;
        std::thread::id owner_;
        Participant *participant_{};
        Physics::PhysicsSceneActivationCandidate *published_{};
    };
}  // namespace Horo::Application::Internal
