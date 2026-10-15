#pragma once

/** @file GameplayNavigationContext.h
 * @brief Host-admitted native and script navigation requests with revocable owner-thread delivery.
 */
#include "Horo/Navigation/NavigationMovement.h"

#include <memory>
#include <string>

namespace Horo::Gameplay {
    namespace NavigationCapabilityErrors {
        /** @brief The exact module/Scene scope grants no requested operation. */
        extern const ErrorCodeDescriptor PermissionDenied;
        /** @brief Host retirement permanently suppresses admission and result delivery. */
        extern const ErrorCodeDescriptor ScopeRevoked;
        /** @brief Navigation is serialized on the host owner thread. */
        extern const ErrorCodeDescriptor WrongThread;
    }  // namespace NavigationCapabilityErrors
    /** @brief Closed operation set shared by native and script adapters. */
    enum class GameplayNavigationOperation : std::uint8_t {
        Path = 0,
        MoveTo = 1,
        Follow = 2,
        Count = 3
    };

    /** @brief Owned request; provider selection, budgets and world identities are host policy. */
    struct GameplayNavigationRequest final {
        GameplayNavigationOperation operation{GameplayNavigationOperation::Path};
        Math::Vec3 start{};
        Math::Vec3 destination{};
        std::optional<Navigation::NavigationAgentOwner> target;
        CancellationToken cancellation;
        Telemetry::OperationContext context;
    };

    /** @brief Copied result without provider, Scene, VM or native module callbacks. */
    struct GameplayNavigationResult final {
        std::optional<Navigation::NavigationPath> path;
        std::optional<Navigation::NavigationMovementStatus> movement;
    };

    /** @brief Explicit exact-generation principal, scene and allowed operations. */
    struct GameplayNavigationBinding final {
        std::string moduleId;
        Navigation::NavigationAgentSceneBinding scene;
        bool queryGranted{};
        bool movementGranted{};
    };
    class GameModuleHost;
    struct GameplayNavigationState;
    struct GameplayNavigationRecord;

    /** @brief Opaque owned operation; all methods run on the host navigation owner thread. */
    class GameplayNavigationHandle final {
    public:
        /** @brief Observe an owned result without waiting. @return Empty while pending, or typed terminal/revocation error. Completed
         * values are host-owned and may outlive the scope and native code image. */
        [[nodiscard]] Result<std::shared_ptr<const GameplayNavigationResult>> Poll() const;
        /** @brief Request cancellation without invoking module or VM code. @return True when cancellation was requested. */
        [[nodiscard]] Result<bool> Cancel() const;

    private:
        friend class GameplayNavigationContext;
        GameplayNavigationHandle(std::shared_ptr<GameplayNavigationState> owner, std::shared_ptr<GameplayNavigationRecord> record);
        std::shared_ptr<GameplayNavigationState> owner_;
        std::shared_ptr<GameplayNavigationRecord> record_;
    };

    /**
     * @brief Explicit host adapter above NavigationRuntime, shared by native and script callers.
     * Host creation, refresh, submission, polling and revocation are serialized on the navigation owner thread.
     * Borrowed world/coordinator/movement owners outlive this adapter until DrainRetired succeeds; retained client/handle copies
     * then fail without dereferencing retired services. No user callback is retained or invoked. The host drains
     * the canonical coordinator after revocation and joins Foundation jobs before provider unload. Native SDK code
     * remains pinned privately through retained record/handle destruction; this does not permit retired callbacks.
     */
    class GameplayNavigationContext final {
        /** @brief Private nonaggregate admission key; only Create can construct a context. */
        class CreationKey final {
            friend class GameplayNavigationContext;
            CreationKey() = default;
        };

    public:
        /** @brief Factory implementation constructor, inaccessible without the private admission key.
         * @param key Private factory admission. @param state Host-created bounded state.
         */
        GameplayNavigationContext(CreationKey key, std::shared_ptr<GameplayNavigationState> state);
        /** @brief Prepare one module/scene grant and finite request storage.
         * @param binding Explicit principal (nonempty module identity up to 256 bytes, without embedded NUL), exact scene and operation
         * grants.
         * @param world Composed world, or null for a typed unavailable context.
         * @param coordinator Canonical path owner, or null when unavailable.
         * @param movement Exact agent controller, or null when movement is unavailable.
         * @param policy Host-selected caller, filter, quality and finite query policy.
         * @param maximumPending Finite retained record bound in [1,64].
         * @return Prepared scope or typed invalid/foreign binding or capacity error.
         */
        [[nodiscard]] static Result<std::shared_ptr<GameplayNavigationContext>> Create(GameplayNavigationBinding binding,
                                                                                       Navigation::NavigationWorldLifecycle *world,
                                                                                       Navigation::NavigationCoordinator *coordinator,
                                                                                       Navigation::NavigationMovementController *movement,
                                                                                       Navigation::NavigationPathSubmission policy,
                                                                                       std::size_t maximumPending = 16);
        ~GameplayNavigationContext();
        GameplayNavigationContext(const GameplayNavigationContext &) = delete;
        GameplayNavigationContext &operator=(const GameplayNavigationContext &) = delete;
        /** @brief Capture fresh host observation and poll/cancel records at the declared owner safe point.
         * @param current Atomic committed navigation observation; never supplied by scripts.
         * @return Success or typed identity/affinity/revocation failure. Does not update movement itself.
         */
        [[nodiscard]] Result<void> Refresh(const Navigation::NavigationMovementObservation &current) const;
        /** @brief Admit exactly the same owned command from native or script code.
         * @param request Closed operation, point/target and copied cancellation/correlation.
         * @return Opaque handle or typed permission/unavailable/validation error.
         */
        [[nodiscard]] Result<GameplayNavigationHandle> Submit(GameplayNavigationRequest request) const;
        /** @brief Cancel outstanding work and permanently suppress delivery before module/scene teardown. */
        void Revoke() const noexcept;
        /** @brief Report live owner-thread scope admission without accessing a provider. @return False after retirement or on another
         * thread. */
        [[nodiscard]] bool IsUsable() const noexcept;
        /** @brief Drain retired canonical records after owner publication.
         * @return True once service references can be released; false while pending.
         * @pre Owner thread; borrowed services remain alive until this returns true.
         */
        [[nodiscard]] Result<bool> DrainRetired() const;
        /** @brief Return immutable host principal and scene routing. @return Owned binding reference. */
        [[nodiscard]] const GameplayNavigationBinding &Binding() const noexcept;

    private:
        friend class GameModuleHost;
        /** @brief Retain host-owned native SDK code through late record and cancellation-state destruction.
         * @param image Independent code image owner with no context/module-instance cycle; host thread before native startup.
         * @return Success or typed rejection of a replacement generation; existing grants are unchanged.
         */
        [[nodiscard]] Result<void> RetainCodeImage(std::shared_ptr<void> image) const;
        std::shared_ptr<GameplayNavigationState> state_;
    };
}  // namespace Horo::Gameplay
