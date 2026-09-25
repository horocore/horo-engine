#include "Horo/PlatformServices/PlatformServicesBackend.h"

#include <limits>
#include <span>
#include <type_traits>

namespace Horo::PlatformServices {
    namespace {
        constexpr std::uint32_t MaxConcurrentRequests = 1U << 20U;
        constexpr std::uint32_t MaxPageEntries = 1U << 20U;
        constexpr std::uint64_t MaxPayloadBytes = 1ULL << 40U;

        [[nodiscard]] constexpr bool IsKnown(const PlatformServiceKind value) noexcept {
            return value < PlatformServiceKind::Count;
        }

        [[nodiscard]] constexpr bool IsKnown(const PlatformServiceAvailability value) noexcept {
            return value == PlatformServiceAvailability::Available || value == PlatformServiceAvailability::Unavailable;
        }

        [[nodiscard]] constexpr bool IsKnown(const PlatformServiceUnavailableReason value) noexcept {
            return value <= PlatformServiceUnavailableReason::ProviderInitializationFailed;
        }

        [[nodiscard]] bool ValidateLimits(const PlatformServiceLimits &limits, const bool available) noexcept {
            if (limits.maxConcurrentRequests > MaxConcurrentRequests || limits.maxPageEntries > MaxPageEntries ||
                limits.maxPayloadBytes > MaxPayloadBytes)
                return false;
            return !available || limits.maxConcurrentRequests != 0;
        }

        [[nodiscard]] bool ValidateCapability(const PlatformServiceCapability &capability) noexcept {
            if (!IsKnown(capability.service) || !IsKnown(capability.availability))
                return false;
            const bool available = capability.availability == PlatformServiceAvailability::Available;
            if (!ValidateLimits(capability.limits, available))
                return false;
            if (const bool leaderboardService = capability.service == PlatformServiceKind::LeaderboardsAndStats;
                (!leaderboardService || !available) && capability.leaderboardQueries.HasAny())
                return false;
            if (capability.leaderboardQueries.HasAny() && capability.limits.maxPageEntries == 0)
                return false;
            if (available)
                return capability.binding && capability.binding->IsValid() && !capability.unavailableReason;
            return !capability.binding && capability.unavailableReason && IsKnown(*capability.unavailableReason);
        }

        [[nodiscard]] constexpr bool IsKnown(const LeaderboardOrdering ordering) noexcept {
            return ordering == LeaderboardOrdering::HighestFirst || ordering == LeaderboardOrdering::LowestFirst;
        }

        [[nodiscard]] constexpr bool IsKnown(const ProgressionValueKind valueKind) noexcept {
            return valueKind == ProgressionValueKind::SignedInteger64 || valueKind == ProgressionValueKind::UnsignedInteger64;
        }

        [[nodiscard]] bool HasExpectedScoreKind(const LeaderboardScoreValue &score, const ProgressionValueKind valueKind) noexcept {
            if (score.valueless_by_exception())
                return false;
            switch (valueKind) {
                case ProgressionValueKind::SignedInteger64:
                    return std::holds_alternative<std::int64_t>(score);
                case ProgressionValueKind::UnsignedInteger64:
                    return std::holds_alternative<std::uint64_t>(score);
            }
            return false;
        }

        [[nodiscard]] bool HasValidLeaderboardPair(const LeaderboardEntry &entry, const LeaderboardEntry &previous,
                                                   const LeaderboardOrdering ordering) noexcept {
            if (entry.score.index() != previous.score.index())
                return false;
            const bool equalScores = std::visit([]<typename Left, typename Right>(const Left left, const Right right) {
                if constexpr (std::is_same_v<Left, Right>)
                    return left == right;
                return false;
            }, entry.score, previous.score);
            if (const bool scoreOrdered = std::visit(
                    [ordering]<typename Current, typename Prior>(const Current current, const Prior prior) {
                if constexpr (std::is_same_v<Current, Prior>)
                    return ordering == LeaderboardOrdering::HighestFirst ? current <= prior : current >= prior;
                return false;
            }, entry.score, previous.score);
                !scoreOrdered)
                return false;
            return equalScores ? entry.rank == previous.rank : entry.rank > previous.rank;
        }

        [[nodiscard]] bool HasValidLeaderboardOrder(const std::span<const LeaderboardEntry> entries, const ProgressionValueKind valueKind,
                                                    const LeaderboardOrdering ordering) noexcept {
            if (!IsKnown(valueKind) || !IsKnown(ordering))
                return false;
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto &entry = entries[index];
                if (entry.rank == 0 || !HasExpectedScoreKind(entry.score, valueKind))
                    return false;
                if (index != 0 && !HasValidLeaderboardPair(entry, entries[index - 1], ordering))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool HasValidLeaderboardPage(const LeaderboardEntriesPage &page, const std::uint32_t startIndex,
                                                   const std::uint32_t pageSize, const ProgressionValueKind valueKind,
                                                   const LeaderboardOrdering ordering) {
            return page.startIndex == startIndex && pageSize != 0 && pageSize <= MaxPageEntries &&
                   startIndex <= std::numeric_limits<std::uint32_t>::max() - pageSize && page.entries.size() <= pageSize &&
                   (!page.hasMore || !page.entries.empty()) && HasValidLeaderboardOrder(page.entries, valueKind, ordering);
        }

        [[nodiscard]] bool HasValidRankedPagePositions(const LeaderboardEntriesPage &page) noexcept {
            if (page.entries.empty())
                return true;
            if (page.entries.front().rank > static_cast<std::uint64_t>(page.startIndex) + 1U)
                return false;
            for (std::size_t index = 1; index < page.entries.size(); ++index) {
                if (page.entries[index].rank != page.entries[index - 1].rank &&
                    page.entries[index].rank != static_cast<std::uint64_t>(page.startIndex) + index + 1U)
                    return false;
            }
            return true;
        }
    }  // namespace

    namespace BackendErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.backend"};
            const ErrorDomainId FrontendDomain{"horo.platform.frontend"};
        }  // namespace

        const ErrorCodeDescriptor InvalidCapabilitySnapshot{Domain,
                                                            ErrorCode{"platform.backend.invalid_capabilities"},
                                                            ErrorSeverity::Error,
                                                            "Platform provider capabilities are invalid.",
                                                            "Reject the candidate and correct its complete capability snapshot.",
                                                            false,
                                                            false};
        const ErrorCodeDescriptor IncompatibleInterfaceVersion{Domain,
                                                               ErrorCode{"platform.backend.incompatible_version"},
                                                               ErrorSeverity::Error,
                                                               "Platform provider interface is incompatible.",
                                                               "Install a provider implementing the supported Horo interface version.",
                                                               false,
                                                               false};
        const ErrorCodeDescriptor RequiredServiceUnavailable{Domain,
                                                             ErrorCode{"platform.capability.required_unavailable"},
                                                             ErrorSeverity::Error,
                                                             "A required platform service is unavailable.",
                                                             "Select a provider satisfying the frozen product policy.",
                                                             false,
                                                             true};
        const ErrorCodeDescriptor ServiceUnavailable{Domain,
                                                     ErrorCode{"platform.capability.unavailable"},
                                                     ErrorSeverity::Error,
                                                     "The selected provider does not expose this platform service.",
                                                     "Disable optional use or select a compatible provider.",
                                                     false,
                                                     true};
        const ErrorCodeDescriptor NullProvider{FrontendDomain,
                                               ErrorCode{"platform.provider.null"},
                                               ErrorSeverity::Error,
                                               "The Null platform provider cannot accept remote service work.",
                                               "Select an available provider or explicitly suppress the optional intent before submission.",
                                               false,
                                               false};
        const ErrorCodeDescriptor UnsupportedOperation{Domain,
                                                       ErrorCode{"platform.capability.operation_unsupported"},
                                                       ErrorSeverity::Error,
                                                       "The selected provider does not support this platform operation.",
                                                       "Use only query kinds explicitly advertised by the selected provider.",
                                                       false,
                                                       true};
    }  // namespace BackendErrors

    namespace LeaderboardErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.leaderboard"};
        }

        const ErrorCodeDescriptor InvalidResult{Domain,
                                                ErrorCode{"platform.leaderboard.invalid_result"},
                                                ErrorSeverity::Error,
                                                "The provider returned a malformed leaderboard result.",
                                                "Reject the result and preserve the admitted request's typed failure.",
                                                false,
                                                false};
    }  // namespace LeaderboardErrors

    /** @copydoc ValidatePlatformServiceCapabilitySnapshot */
    Result<void> ValidatePlatformServiceCapabilitySnapshot(const PlatformServiceCapabilitySnapshot &snapshot,
                                                           const PlatformServicesBackendConfig &config) {
        if (snapshot.interfaceVersion !=
            PlatformServicesBackendInterfaceVersion{PlatformServicesBackendInterfaceMajor, PlatformServicesBackendInterfaceMinor})
            return Result<void>::Failure(MakeError(BackendErrors::IncompatibleInterfaceVersion));
        if (!snapshot.provider.IsValid() || !snapshot.providerGeneration.IsValid())
            return Result<void>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));

        std::array<bool, static_cast<std::size_t>(PlatformServiceKind::Count)> seen{};
        for (const auto &capability : snapshot.services) {
            if (!ValidateCapability(capability))
                return Result<void>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));
            const auto index = static_cast<std::size_t>(capability.service);
            if (seen[index])
                return Result<void>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));
            seen[index] = true;
            const bool available = capability.availability == PlatformServiceAvailability::Available;
            if (config.requiredServices[index] && !available)
                return Result<void>::Failure(MakeError(BackendErrors::RequiredServiceUnavailable));
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateLeaderboardEntriesPage */
    Result<void> ValidateLeaderboardEntriesPage(const LeaderboardEntriesPage &page, const LeaderboardRankedQuery &query,
                                                const ProgressionValueKind valueKind, const LeaderboardOrdering ordering) {
        if (!query.leaderboard.IsValid() || !HasValidLeaderboardPage(page, query.startIndex, query.pageSize, valueKind, ordering) ||
            !HasValidRankedPagePositions(page))
            return Result<void>::Failure(MakeError(LeaderboardErrors::InvalidResult));
        return Result<void>::Success();
    }

    /** @copydoc ValidateLeaderboardEntriesPage */
    Result<void> ValidateLeaderboardEntriesPage(const LeaderboardEntriesPage &page, const LeaderboardFriendsQuery &query,
                                                const ProgressionValueKind valueKind, const LeaderboardOrdering ordering) {
        if (!query.leaderboard.IsValid() || !HasValidLeaderboardPage(page, query.startIndex, query.pageSize, valueKind, ordering))
            return Result<void>::Failure(MakeError(LeaderboardErrors::InvalidResult));
        return Result<void>::Success();
    }

    /** @copydoc ValidateLeaderboardAroundSubjectResult */
    Result<void> ValidateLeaderboardAroundSubjectResult(const LeaderboardAroundSubjectResult &result,
                                                        const LeaderboardAroundSubjectQuery &query, const ProgressionValueKind valueKind,
                                                        const LeaderboardOrdering ordering) {
        if (const auto entryLimit = static_cast<std::uint64_t>(query.entriesBefore) + query.entriesAfter + 1U;
            !query.leaderboard.IsValid() || entryLimit > MaxPageEntries || result.entries.size() > entryLimit ||
            !HasValidLeaderboardOrder(result.entries, valueKind, ordering))
            return Result<void>::Failure(MakeError(LeaderboardErrors::InvalidResult));
        if (!result.subjectEntryIndex.has_value()) {
            if (!result.entries.empty() || result.hasEarlier || result.hasLater)
                return Result<void>::Failure(MakeError(LeaderboardErrors::InvalidResult));
            return Result<void>::Success();
        }
        if (const auto subjectIndex = static_cast<std::size_t>(*result.subjectEntryIndex);
            subjectIndex >= result.entries.size() || subjectIndex > query.entriesBefore ||
            result.entries.size() - subjectIndex - 1U > query.entriesAfter || (result.hasEarlier && subjectIndex < query.entriesBefore) ||
            (result.hasLater && result.entries.size() - subjectIndex - 1U < query.entriesAfter))
            return Result<void>::Failure(MakeError(LeaderboardErrors::InvalidResult));
        return Result<void>::Success();
    }

    /** @copydoc ActivatePlatformServicesBackend */
    Result<PlatformServiceCapabilitySnapshot> ActivatePlatformServicesBackend(IPlatformServicesBackend &backend,
                                                                              const PlatformServicesBackendConfig &config) {
        auto inspected = backend.InspectCapabilities();
        if (inspected.HasError())
            return Result<PlatformServiceCapabilitySnapshot>::Failure(inspected.ErrorValue());
        auto snapshot = std::move(inspected).Value();
        if (const auto validated = ValidatePlatformServiceCapabilitySnapshot(snapshot, config); validated.HasError())
            return Result<PlatformServiceCapabilitySnapshot>::Failure(validated.ErrorValue());
        if (const auto activated = backend.Activate(config); activated.HasError()) {
            static_cast<void>(backend.Shutdown());
            return Result<PlatformServiceCapabilitySnapshot>::Failure(activated.ErrorValue());
        }
        return Result<PlatformServiceCapabilitySnapshot>::Success(std::move(snapshot));
    }
}  // namespace Horo::PlatformServices
