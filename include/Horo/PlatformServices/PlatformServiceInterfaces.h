#pragma once

/**
 * @file PlatformServiceInterfaces.h
 * @brief Backend-neutral Platform Services identities, requests, results, and narrow service interfaces.
 */

#include "Horo/PlatformServices/PlatformCloudObjects.h"
#include "Horo/PlatformServices/PlatformRequest.h"
#include "Horo/PlatformServices/PlatformUserSession.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Horo::PlatformServices {
    /** @brief Immutable owner allowed to accept one authored progression fact. */
    enum class ProgressionAuthorityMode : std::uint8_t {
        LocalProduct,
        AuthorityServer
    };

    /** @brief Typed numeric representation shared by progression definitions and score values. */
    enum class ProgressionValueKind : std::uint8_t {
        SignedInteger64,
        UnsignedInteger64
    };

    /** @brief One signed or unsigned 64-bit leaderboard score value. */
    using LeaderboardScoreValue = std::variant<std::int64_t, std::uint64_t>;

    /** @brief Authored score direction used by one leaderboard definition. */
    enum class LeaderboardOrdering : std::uint8_t {
        HighestFirst,
        LowestFirst
    };

    /** @brief A leaderboard read supported by a provider capability snapshot. */
    enum class LeaderboardQueryKind : std::uint8_t {
        Ranked,
        AroundSubject,
        Friends
    };

    /** @brief Explicit per-query-kind support; absence never selects another query or provider. */
    struct LeaderboardQueryCapabilities final {
        bool ranked{};        /**< The provider supports ordered ranked pages. */
        bool aroundSubject{}; /**< The provider supports windows around the authenticated subject. */
        bool friends{};       /**< The provider supports consent-gated friends pages. */

        /** @brief Returns whether one known query kind is supported. @param kind Query kind. @return Capability fact. */
        [[nodiscard]] constexpr bool Supports(const LeaderboardQueryKind kind) const noexcept {
            switch (kind) {
                case LeaderboardQueryKind::Ranked:
                    return ranked;
                case LeaderboardQueryKind::AroundSubject:
                    return aroundSubject;
                case LeaderboardQueryKind::Friends:
                    return friends;
            }
            return false;
        }

        /** @brief Reports whether any query kind is enabled. @return Capability fact. */
        [[nodiscard]] constexpr bool HasAny() const noexcept {
            return ranked || aroundSubject || friends;
        }

        [[nodiscard]] constexpr bool operator==(const LeaderboardQueryCapabilities &) const noexcept = default;
    };

    /** @brief Strong nonzero project-authored identity. */
    template <typename Tag> struct PlatformStableId final {
        std::uint64_t value{}; /**< Zero is reserved and invalid. */

        /** @brief Checks representation. @return Whether the ID is nonzero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformStableId &) const noexcept = default;
    };

    struct AchievementIdTag;
    struct LeaderboardIdTag;
    struct StatIdTag;
    struct CloudObjectIdTag;
    struct PresenceStatusIdTag;
    using AchievementId = PlatformStableId<AchievementIdTag>;
    using LeaderboardId = PlatformStableId<LeaderboardIdTag>;
    using StatId = PlatformStableId<StatIdTag>;
    using CloudObjectId = PlatformStableId<CloudObjectIdTag>;
    using PresenceStatusId = PlatformStableId<PresenceStatusIdTag>;

    /** @brief Typed achievement unlock intent. */
    struct AchievementUnlockRequest final {
        PlatformSubjectHandle subject;
        AchievementId achievement;
    };

    /** @brief Typed leaderboard score submission intent. */
    struct LeaderboardScoreRequest final {
        PlatformSubjectHandle subject;
        LeaderboardId leaderboard;
        LeaderboardScoreValue score{}; /**< Exact signedness declared by the authored definition. */
    };

    /** @brief One ranked leaderboard page at an explicit zero-based offset and finite size. */
    struct LeaderboardRankedQuery final {
        PlatformSubjectHandle subject;
        LeaderboardId leaderboard;
        std::uint32_t startIndex{}; /**< Zero-based ordinal offset. */
        std::uint32_t pageSize{};   /**< Required finite nonzero result limit. */
    };

    /** @brief One bounded window around the authenticated subject's own leaderboard entry. */
    struct LeaderboardAroundSubjectQuery final {
        PlatformSubjectHandle subject;
        LeaderboardId leaderboard;
        std::uint32_t entriesBefore{}; /**< Maximum rows preceding the current subject. */
        std::uint32_t entriesAfter{};  /**< Maximum rows following the current subject. */
    };

    /** @brief One explicitly bounded page of the authenticated subject's friends on a leaderboard. */
    struct LeaderboardFriendsQuery final {
        PlatformSubjectHandle subject;
        LeaderboardId leaderboard;
        std::uint32_t startIndex{}; /**< Zero-based ordinal offset among friends. */
        std::uint32_t pageSize{};   /**< Required finite nonzero result limit. */
    };

    /** @brief Typed persistent-stat replacement intent. */
    struct StatWriteRequest final {
        PlatformSubjectHandle subject;
        StatId stat;
        std::int64_t value{};
    };

    /** @brief Typed cloud object read address; it is not a filesystem path. */
    struct CloudReadRequest final {
        PlatformSubjectHandle subject;
        CloudObjectId object;
    };

    /** @brief Bounded presence publication intent. */
    struct PresenceUpdateRequest final {
        PlatformSubjectHandle subject;
        PresenceStatusId status;
        std::string detail;
    };

    /** @brief Bounded friends-page query. */
    struct FriendsQuery final {
        PlatformSubjectHandle subject;
        std::uint32_t pageSize{};
    };

    /** @brief Immutable owned cloud object payload. */
    struct CloudReadResult final {
        CloudObjectId object;
        std::vector<std::byte> bytes;
    };

    /** @brief Privacy-safe friend presentation; display text is never durable identity. */
    struct FriendPresentation final {
        PlatformSubjectHandle subject;
        std::string displayName;
    };

    /** @brief Owned bounded friends page. */
    struct FriendsPage final {
        std::vector<FriendPresentation> entries;
        bool hasMore{};
    };

    /** @brief Ranked score row with no provider or account identifier. */
    struct LeaderboardEntry final {
        std::uint64_t rank{};          /**< One-based competition rank; equal scores share a rank. */
        LeaderboardScoreValue score{}; /**< Exact signedness declared by the leaderboard definition. */
    };

    /** @brief One finite ranked or friends page; startIndex is a zero-based ordinal offset. */
    struct LeaderboardEntriesPage final {
        std::uint32_t startIndex{};            /**< Exact zero-based offset from the admitted query. */
        std::vector<LeaderboardEntry> entries; /**< Bounded, score-ordered result rows. */
        bool hasMore{};                        /**< Whether another row exists at the next offset. */
    };

    /** @brief One finite window around the authenticated subject's own entry. */
    struct LeaderboardAroundSubjectResult final {
        std::vector<LeaderboardEntry> entries;
        std::optional<std::uint32_t> subjectEntryIndex; /**< Index within entries, absent when the subject is unranked. */
        bool hasEarlier{};
        bool hasLater{};
    };

    /** @brief Stable failures for malformed leaderboard query results. */
    namespace LeaderboardErrors {
        extern const ErrorCodeDescriptor InvalidResult;
    }  // namespace LeaderboardErrors

    /**
     * @brief Validates a page's offset, finite count, score direction, and tie ranks.
     * @param page Backend result to validate before request completion is published.
     * @param query Exact admitted ranked or friends query.
     * @param valueKind Authored score representation from the captured leaderboard definition.
     * @param ordering Captured immutable ordering from the leaderboard definition.
     * @return Success for a bounded, correctly ordered page or a typed failure.
     */
    [[nodiscard]] Result<void> ValidateLeaderboardEntriesPage(const LeaderboardEntriesPage &page, const LeaderboardRankedQuery &query,
                                                              ProgressionValueKind valueKind, LeaderboardOrdering ordering);

    /**
     * @brief Validates a friends-page offset, finite count, score direction, and tie ranks.
     * @param page Backend result to validate before request completion is published.
     * @param query Exact admitted friends query.
     * @param valueKind Authored score representation from the captured leaderboard definition.
     * @param ordering Captured immutable ordering from the leaderboard definition.
     * @return Success for a bounded, correctly ordered page or a typed failure.
     */
    [[nodiscard]] Result<void> ValidateLeaderboardEntriesPage(const LeaderboardEntriesPage &page, const LeaderboardFriendsQuery &query,
                                                              ProgressionValueKind valueKind, LeaderboardOrdering ordering);

    /**
     * @brief Validates one around-subject result against its explicit entry window.
     * @param result Backend result to validate before request completion is published.
     * @param query Exact admitted around-subject query.
     * @param valueKind Authored score representation from the captured leaderboard definition.
     * @param ordering Captured immutable ordering from the leaderboard definition.
     * @return Success for a bounded, correctly ordered window or a typed failure.
     */
    [[nodiscard]] Result<void> ValidateLeaderboardAroundSubjectResult(const LeaderboardAroundSubjectResult &result,
                                                                      const LeaderboardAroundSubjectQuery &query,
                                                                      ProgressionValueKind valueKind, LeaderboardOrdering ordering);

    /** @brief Achievement request surface. Unsupported implementations return a typed failure. */
    class IAchievementService {
    public:
        virtual ~IAchievementService() = default;
        /** @brief Submits one typed unlock intent. @param request Owned semantic request. @return Admitted request handle or typed failure.
         */
        [[nodiscard]] virtual Result<PlatformRequestHandle<void>> UnlockAchievement(AchievementUnlockRequest request) = 0;
    };

    /** @brief Leaderboard and persistent-stat request surface. */
    class ILeaderboardStatService {
    public:
        virtual ~ILeaderboardStatService() = default;
        /** @brief Submits one score. @param request Owned score intent. @return Admitted request handle or typed failure. */
        [[nodiscard]] virtual Result<PlatformRequestHandle<void>> SubmitScore(LeaderboardScoreRequest request) = 0;
        /** @brief Queries one bounded ranked page. @param query Explicit offset and page size. @return Admitted page request or typed
         * failure. */
        [[nodiscard]] virtual Result<PlatformRequestHandle<LeaderboardEntriesPage>> QueryRankedLeaderboard(
            LeaderboardRankedQuery query) = 0;
        /** @brief Queries a bounded window around the current subject. @param query Explicit before/after limits. @return Admitted window
         * request or typed failure. */
        [[nodiscard]] virtual Result<PlatformRequestHandle<LeaderboardAroundSubjectResult>> QueryLeaderboardAroundSubject(
            LeaderboardAroundSubjectQuery query) = 0;
        /** @brief Queries one bounded page of the current subject's friends. @param query Explicit offset and page size. @return Admitted
         * page request or typed failure. */
        [[nodiscard]] virtual Result<PlatformRequestHandle<LeaderboardEntriesPage>> QueryFriendsLeaderboard(
            LeaderboardFriendsQuery query) = 0;
        /** @brief Writes one stat value. @param request Owned stat intent. @return Admitted request handle or typed failure. */
        [[nodiscard]] virtual Result<PlatformRequestHandle<void>> WriteStat(StatWriteRequest request) = 0;
    };

    /** @brief Opaque cloud-object transport surface; it owns no save format or filesystem path. */
    class ICloudService {
    public:
        virtual ~ICloudService() = default;
        /** @brief Reads one opaque object. @param request Typed object address. @return Admitted typed request or failure. */
        [[nodiscard]] virtual Result<PlatformRequestHandle<CloudReadResult>> ReadCloudObject(CloudReadRequest request) = 0;

        /**
         * @brief Lists bounded opaque object metadata in the captured authenticated session partition.
         * @param request Opaque prefix/cursor and finite page bound.
         * @return Admitted metadata request or UnsupportedCapability.
         * @note The default keeps older providers source-compatible while they opt into PLS-005.2.
         */
        [[nodiscard]] virtual Result<PlatformRequestHandle<CloudObjectPage>> ListCloudObjects(CloudListRequest) {
            return Result<PlatformRequestHandle<CloudObjectPage>>::Failure(MakeError(CloudObjectErrors::UnsupportedCapability));
        }

        /**
         * @brief Reads one complete bounded opaque object revision.
         * @param request Opaque key, captured session subject, and maximum result size.
         * @return Admitted complete read or UnsupportedCapability.
         * @note The result must pass ValidateCloudBlobReadCompletion before publication.
         */
        [[nodiscard]] virtual Result<PlatformRequestHandle<CloudBlobReadResult>> ReadCloudObject(CloudBlobReadRequest) {
            return Result<PlatformRequestHandle<CloudBlobReadResult>>::Failure(MakeError(CloudObjectErrors::UnsupportedCapability));
        }

        /**
         * @brief Executes one conditional atomic write at the selected provider's commit point.
         * @param request Complete exact intent retained through provider-operation retirement.
         * @return Admitted mutation or UnsupportedCapability.
         * @pre Admission requires advertised ConditionalAtomicObject and durable mutation-ID deduplication.
         * @post The provider compares absence or exact revision and publishes all bytes at one indivisible commit point. A competing
         *       writer receives AlreadyExists or PreconditionFailed without changing the visible object. Native multipart staging is
         *       never visible at the public key. An exact mutation-ID replay returns the original semantic outcome and commit evidence
         *       without another commit; changed intent or operation with that ID returns IdempotencyConflict. These guarantees must be
         *       implemented by the provider's native conditional and durable deduplication facilities, never a frontend read or mutex.
         *       ValidateCloudWriteCompletion must pass before a success enters the request store. Cancel, timeout, and shutdown after
         *       commit may have begun retain an unknown remote outcome for coordinator reconciliation, never a fabricated success.
         */
        [[nodiscard]] virtual Result<PlatformRequestHandle<CloudMutationResult>> WriteCloudObject(CloudBlobWriteRequest) {
            return Result<PlatformRequestHandle<CloudMutationResult>>::Failure(MakeError(CloudObjectErrors::UnsupportedCapability));
        }

        /**
         * @brief Executes one atomic revision-matched delete at the selected provider's commit point.
         * @param request Exact key, revision and durable mutation identity.
         * @return Admitted mutation or UnsupportedCapability.
         * @post A stale revision returns PreconditionFailed without deleting a newer object. Exact replay returns the original outcome
         *       and evidence without a second commit; cross-operation ID reuse returns IdempotencyConflict. ValidateCloudDeleteCompletion
         *       must pass before success publication. Cancellation and shutdown preserve ambiguous remote outcomes for reconciliation.
         */
        [[nodiscard]] virtual Result<PlatformRequestHandle<CloudMutationResult>> DeleteCloudObject(CloudBlobDeleteRequest) {
            return Result<PlatformRequestHandle<CloudMutationResult>>::Failure(MakeError(CloudObjectErrors::UnsupportedCapability));
        }

        /** @brief Requests advisory quota usage for one authenticated session. @param subject Current subject capability.
         * @return Admitted observation or UnsupportedCapability. */
        [[nodiscard]] virtual Result<PlatformRequestHandle<CloudQuotaObservation>> QueryCloudQuota(PlatformSubjectHandle) {
            return Result<PlatformRequestHandle<CloudQuotaObservation>>::Failure(MakeError(CloudObjectErrors::UnsupportedCapability));
        }
    };

    /** @brief Best-effort presence publication surface. */
    class IPresenceService {
    public:
        virtual ~IPresenceService() = default;
        /** @brief Publishes bounded presence. @param request Owned state. @return Admitted request or typed failure. */
        [[nodiscard]] virtual Result<PlatformRequestHandle<void>> SetPresence(PresenceUpdateRequest request) = 0;
        /** @brief Clears presence for a subject. @param subject Current subject capability. @return Admitted request or typed failure. */
        [[nodiscard]] virtual Result<PlatformRequestHandle<void>> ClearPresence(PlatformSubjectHandle subject) = 0;
    };

    /** @brief Consent-gated read-only social graph surface. */
    class IFriendsService {
    public:
        virtual ~IFriendsService() = default;
        /** @brief Queries one bounded page. @param query Current subject and bound. @return Admitted typed request or failure. */
        [[nodiscard]] virtual Result<PlatformRequestHandle<FriendsPage>> QueryFriends(FriendsQuery query) = 0;
    };

    /** @brief Provider-session observation surface. */
    class ISessionService {
    public:
        virtual ~ISessionService() = default;
        /** @brief Queries the current session. @return Admitted typed request or failure. */
        [[nodiscard]] virtual Result<PlatformRequestHandle<PlatformSessionSnapshot>> QueryCurrentSession() = 0;
    };
}  // namespace Horo::PlatformServices
