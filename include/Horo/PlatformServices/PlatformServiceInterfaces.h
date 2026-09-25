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
#include <string>
#include <vector>

namespace Horo::PlatformServices {
    /** @brief Immutable owner allowed to accept one authored progression fact. */
    enum class ProgressionAuthorityMode : std::uint8_t {
        LocalProduct,
        AuthorityServer
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
        std::int64_t score{};
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
