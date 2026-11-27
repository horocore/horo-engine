#pragma once

/**
 * @file PlatformFriendsCoordinator.h
 * @brief Bounded read-only friends publication, retention and session/access fencing.
 */

#include "Horo/PlatformServices/PlatformServicesBackend.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Horo::PlatformServices {
    /** @brief Finite in-memory presentation policy; no durable social cache is supported. */
    struct PlatformFriendsCoordinatorConfig final {
        std::uint32_t maximumPageEntries{128};
        std::size_t maximumDisplayNameBytes{256};
        std::size_t maximumPageBytes{32'768};
        std::chrono::milliseconds retention{30'000};
    };

    /** @brief Exact provider-query token captured at admission, containing no native identity. */
    struct PlatformFriendsQueryIntent final {
        FriendsQuery query;
        PlatformAccessPolicyRevision accessRevision;
        std::uint64_t sequence{};
    };

    /** @brief Stable failures from the bounded social publication boundary. */
    namespace FriendsCoordinatorErrors {
        /** @brief Provider composition or finite presentation policy is malformed. */
        extern const ErrorCodeDescriptor InvalidConfiguration;
        /** @brief Query size or offset exceeds the admitted finite bounds. */
        extern const ErrorCodeDescriptor InvalidQuery;
        /** @brief A complete provider page contains malformed, stale or oversized presentation. */
        extern const ErrorCodeDescriptor InvalidPage;
        /** @brief The coordinator already owns a pending query. */
        extern const ErrorCodeDescriptor Busy;
        /** @brief A query token has been invalidated or does not match the pending envelope. */
        extern const ErrorCodeDescriptor StaleQuery;
        /** @brief The coordinator has closed admission and discarded retained presentation. */
        extern const ErrorCodeDescriptor Closed;
    }  // namespace FriendsCoordinatorErrors

    /**
     * @brief Owner-lane read-only friends cache and provider-result commit fence.
     * @details The composition owner forwards its published session snapshots before provider dispatch or presentation.
     * Adapters submit only through a valid query token and retain native callback ownership independently. Cancel, session/
     * access replacement and Close invalidate tokens; late results cannot replace the current cache. Presentation callers
     * must re-read through ReadPage before exposure and must not persist or log returned personal data.
     */
    class PlatformFriendsCoordinator final {
    public:
        /** @brief Validates exact capability/session composition and finite memory/retention limits.
         * @param capabilities Validated provider capability snapshot after composition policy projection.
         * @param session Published immutable session authority.
         * @param config Bounded presentation retention policy.
         * @return Coordinator or typed invalid configuration. */
        [[nodiscard]] static Result<PlatformFriendsCoordinator> Create(PlatformServiceCapabilitySnapshot capabilities,
                                                                       PlatformSessionSnapshot session,
                                                                       PlatformFriendsCoordinatorConfig config = {});
        PlatformFriendsCoordinator(const PlatformFriendsCoordinator &) = delete;
        PlatformFriendsCoordinator &operator=(const PlatformFriendsCoordinator &) = delete;
        /** @brief Transfers the coordinator and closes admission on the moved-from value.
         * @param other Owner whose state is transferred. */
        PlatformFriendsCoordinator(PlatformFriendsCoordinator &&other) noexcept;
        PlatformFriendsCoordinator &operator=(PlatformFriendsCoordinator &&) = delete;

        /** @brief Captures one bounded query after capability and access admission.
         * @param query Exact subject and finite page bound. @param revision Expected current access revision.
         * @return Query token or typed lifecycle/capability/access failure; rejection creates no provider work. */
        [[nodiscard]] Result<PlatformFriendsQueryIntent> BeginQuery(FriendsQuery query, PlatformAccessPolicyRevision revision);
        /** @brief Commits a completely validated page or provider failure once.
         * @param query Exact admitted token. @param result Owned provider page or normalized failure.
         * @param now Monotonic owner-lane completion time used for retention.
         * @return Success only for approved bounded data, or typed stale/malformed/provider failure. */
        [[nodiscard]] Result<void> Complete(const PlatformFriendsQueryIntent &query, Result<FriendsPage> result,
                                            std::chrono::steady_clock::time_point now);
        /** @brief Invalidates one query token before requesting native cancellation externally.
         * @param query Exact token. @return Success or stale/closed failure. */
        [[nodiscard]] Result<void> Cancel(const PlatformFriendsQueryIntent &query);
        /** @brief Revalidates authority and retention before returning a presentation copy.
         * @param subject Current requesting subject. @param revision Current expected access revision.
         * @param now Monotonic presentation time. @return Cached page, no retained page, or typed denial. */
        [[nodiscard]] Result<std::optional<FriendsPage>> ReadPage(PlatformSubjectHandle subject, PlatformAccessPolicyRevision revision,
                                                                  std::chrono::steady_clock::time_point now);
        /** @brief Invalidates all retained personal data and queries before replacing session/access authority.
         * @param session New published immutable authority. @return Success or stale/invalid/closed failure. */
        [[nodiscard]] Result<void> UpdateSession(PlatformSessionSnapshot session);
        /** @brief Idempotently closes admission and releases retained presentation. @return Success. */
        [[nodiscard]] Result<void> Close() noexcept;

    private:
        /** @brief Stores validated composition inputs without invoking a provider.
         * @param capability Exact Friends capability. @param session Published authority. @param config Finite retention policy. */
        PlatformFriendsCoordinator(PlatformServiceCapability capability, PlatformSessionSnapshot session,
                                   PlatformFriendsCoordinatorConfig config);
        /** @brief Checks closed/capability/session/access admission in that order.
         * @param subject Captured requester. @param revision Expected access policy. @return Success or typed rejection. */
        [[nodiscard]] Result<void> ValidateAccess(PlatformSubjectHandle subject, PlatformAccessPolicyRevision revision) const;
        /** @brief Compares a complete query envelope with pending authority.
         * @param query Provider token. @return True only for the current pending query. */
        [[nodiscard]] bool IsPending(const PlatformFriendsQueryIntent &query) const noexcept;
        /** @brief Validates every page row before any presentation is retained.
         * @param query Admitted bounds. @param page Complete provider result. @return True for a bounded approved page. */
        [[nodiscard]] bool IsValidPage(const PlatformFriendsQueryIntent &query, const FriendsPage &page) const noexcept;
        PlatformServiceCapability capability_;
        PlatformSessionSnapshot session_;
        PlatformFriendsCoordinatorConfig config_;
        std::optional<PlatformFriendsQueryIntent> pending_;
        std::optional<FriendsPage> page_;
        std::optional<std::chrono::steady_clock::time_point> capturedAt_;
        std::uint64_t sequence_{};
        bool closed_{};
    };
}  // namespace Horo::PlatformServices
