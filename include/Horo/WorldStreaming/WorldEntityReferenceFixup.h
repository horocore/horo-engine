#pragma once

/**
 * @file WorldEntityReferenceFixup.h
 * @brief Bounded activation-time mapping and deferred cross-cell reference fixups.
 */

#include "Horo/WorldStreaming/WorldDependencyPlan.h"
#include "Horo/WorldStreaming/WorldStreamingRuntimeComposition.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        struct WorldEntityReferenceFixupRevisionTag;
        struct WorldRuntimeEntityIdTag;
    }  // namespace Detail

    /** @brief Revision of one owned cross-cell fixup record. */
    using WorldEntityReferenceFixupRevision =
        Foundation::Detail::NonZeroId64<Detail::WorldEntityReferenceFixupRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Backend-neutral runtime entity token supplied by the active Scene owner. */
    using WorldRuntimeEntityId = Foundation::Detail::NonZeroId64<Detail::WorldRuntimeEntityIdTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Lifecycle gate for the owner of pending cross-cell fixups. */
    enum class WorldEntityReferenceFixupLedgerLifecycle : std::uint8_t {
        Active,
        Cancelling,
        Closed,
    };

    /** @brief Terminal or deferred result of one entity-reference fixup operation. */
    enum class WorldEntityReferenceFixupDisposition : std::uint8_t {
        Resolved,
        Deferred,
        Cancelled,
        Failed,
    };

    /** @brief State mutation represented by one fixup result. */
    enum class WorldEntityReferenceFixupMutation : std::uint8_t {
        Resolved,
        Deferred,
        Replaced,
        Activated,
        Cancelled,
        Failed,
    };

    /** @brief Mandatory storage ceiling for one fixup owner. */
    struct WorldEntityReferenceFixupLimits final {
        static constexpr std::uint32_t MaximumPendingReferences = 4096;

        std::uint32_t maximumPendingReferences{};  /**< Maximum retained unresolved soft references. */
        std::uint32_t maximumActivationMappings{}; /**< Maximum mappings admitted in one activation batch. */

        /** @brief Checks the bounded limit. @return True when the configured ceiling is supported. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumPendingReferences > 0 && maximumPendingReferences <= MaximumPendingReferences && maximumActivationMappings > 0 &&
                   maximumActivationMappings <= MaximumPendingReferences;
        }
    };

    /** @brief Exact stable endpoint-to-runtime mapping supplied for one activation. */
    struct WorldEntityReferenceBinding final {
        WorldDependencyEndpoint endpoint{}; /**< Stable authored identity and exact content revision. */
        WorldRuntimeEntityId runtime{};     /**< Opaque runtime token owned by the Scene authority. */

        /** @brief Checks the complete mapping representation. @return True when endpoint and runtime token are valid. */
        [[nodiscard]] bool IsValid() const noexcept {
            return endpoint.IsValid() && runtime.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const WorldEntityReferenceBinding &) const noexcept = default;
    };

    /** @brief One stable hard or soft reference with an owner-local replacement revision. */
    struct WorldEntityReferenceFixupRequest final {
        WorldDependencyCandidate reference{};                                /**< Stable source, target and hard/soft policy. */
        WorldEntityReferenceFixupRevision revision{};                        /**< Exact owner-local fixup revision. */
        std::optional<WorldEntityReferenceFixupRevision> expectedRevision{}; /**< CAS revision for replacement only. */

        /** @brief Checks identities, revisions and the closed dependency policy. @return True when structurally valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const WorldEntityReferenceFixupRequest &) const noexcept = default;
    };

    /** @brief Explicit result retaining the request and optional exact runtime target. */
    struct WorldEntityReferenceFixupResult final {
        WorldEntityReferenceFixupRequest request{}; /**< Request that produced this result. */
        WorldEntityReferenceFixupDisposition disposition{WorldEntityReferenceFixupDisposition::Failed}; /**< Reconciliation outcome. */
        WorldEntityReferenceFixupMutation mutation{WorldEntityReferenceFixupMutation::Failed}; /**< Ownership mutation performed. */
        std::optional<WorldRuntimeEntityId> runtime{};                                         /**< Target token only when resolved. */

        /** @brief Checks disposition/payload coherence. @return True when the result is canonical. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Explicit owner of activation mappings and unresolved cross-cell soft references.
     * @details Hard references are never retained when their exact target is unavailable. Soft references are retained only as
     *          stable endpoint records and may be activated later by the same owner lifetime and exact target revision.
     */
    class WorldEntityReferenceFixupLedger final {
    public:
        WorldEntityReferenceFixupLedger(const WorldEntityReferenceFixupLedger &) = delete;
        WorldEntityReferenceFixupLedger &operator=(const WorldEntityReferenceFixupLedger &) = delete;
        /** @brief Transfers the unique owner and closes the moved-from ledger. @param other Ledger to transfer. */
        WorldEntityReferenceFixupLedger(WorldEntityReferenceFixupLedger &&other) noexcept;
        WorldEntityReferenceFixupLedger &operator=(WorldEntityReferenceFixupLedger &&) = delete;

        /**
         * @brief Creates an active bounded fixup owner.
         * @param owner Exact mounted partition and runtime-composition lifetime.
         * @param limits Mandatory pending-reference ceiling.
         * @return Empty owner or typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<WorldEntityReferenceFixupLedger> Create(const StreamingRuntimeOwnerToken &owner,
                                                                            WorldEntityReferenceFixupLimits limits);

        /**
         * @brief Resolves an activation batch or retains one safe soft miss.
         * @param owner Exact current owner lifetime.
         * @param request Stable source/target request; expectedRevision authorizes replacement.
         * @param activeMappings Exact source and currently active target mappings.
         * @return Resolved or deferred result, or typed invalid/stale/unsupported/capacity/lifecycle failure.
         * @post Failure leaves every pending record unchanged.
         */
        [[nodiscard]] Result<WorldEntityReferenceFixupResult> Submit(const StreamingRuntimeOwnerToken &owner,
                                                                     const WorldEntityReferenceFixupRequest &request,
                                                                     std::span<const WorldEntityReferenceBinding> activeMappings);

        /**
         * @brief Resolves one retained soft reference after its target cell activates.
         * @param owner Exact current owner lifetime.
         * @param request Exact retained request and revision.
         * @param target Exact target mapping from the activating cell.
         * @return Resolved activation result or typed invalid/stale/lifecycle failure.
         * @post Failure leaves the retained request unchanged.
         */
        [[nodiscard]] Result<WorldEntityReferenceFixupResult> Activate(const StreamingRuntimeOwnerToken &owner,
                                                                       const WorldEntityReferenceFixupRequest &request,
                                                                       const WorldEntityReferenceBinding &target);

        /** @brief Cancels one retained soft reference without fabricating a target. @return Typed cancellation result or failure. */
        [[nodiscard]] Result<WorldEntityReferenceFixupResult> Cancel(const StreamingRuntimeOwnerToken &owner,
                                                                     const WorldEntityReferenceFixupRequest &request);

        /** @brief Records an owner-approved terminal failure and removes one retained soft reference. @return Typed failure result. */
        [[nodiscard]] Result<WorldEntityReferenceFixupResult> Fail(const StreamingRuntimeOwnerToken &owner,
                                                                   const WorldEntityReferenceFixupRequest &request);

        /** @brief Closes new admission while retained soft references drain. @return Success or typed invalid/stale failure. */
        [[nodiscard]] Result<void> RequestCancellation(const StreamingRuntimeOwnerToken &owner) noexcept;

        /** @brief Begins terminal shutdown; repeated shutdown is idempotent. @return Success or typed invalid/stale failure. */
        [[nodiscard]] Result<void> BeginShutdown(const StreamingRuntimeOwnerToken &owner) noexcept;

        /** @brief Returns the exact owner lifetime. @return Immutable owner token. */
        [[nodiscard]] const StreamingRuntimeOwnerToken &Owner() const noexcept;
        /** @brief Returns the current lifecycle, deriving Closed after all pending records drain. @return Lifecycle state. */
        [[nodiscard]] WorldEntityReferenceFixupLedgerLifecycle Lifecycle() const noexcept;
        /** @brief Returns canonical unresolved soft requests. @return Borrowed records valid until the next mutation. */
        [[nodiscard]] std::span<const WorldEntityReferenceFixupRequest> Pending() const noexcept;

    private:
        WorldEntityReferenceFixupLedger(const StreamingRuntimeOwnerToken &owner, WorldEntityReferenceFixupLimits limits,
                                        std::vector<WorldEntityReferenceFixupRequest> pending) noexcept;

        StreamingRuntimeOwnerToken owner_{};
        WorldEntityReferenceFixupLimits limits_{};
        std::vector<WorldEntityReferenceFixupRequest> pending_;
        WorldEntityReferenceFixupLedgerLifecycle lifecycle_{WorldEntityReferenceFixupLedgerLifecycle::Active};
    };

    /** @brief Advances a fixup revision without wrapping. @param current Current valid revision. @return Successor or exhaustion. */
    [[nodiscard]] Result<WorldEntityReferenceFixupRevision> NextWorldEntityReferenceFixupRevision(
        WorldEntityReferenceFixupRevision current);
}  // namespace Horo::WorldStreaming
