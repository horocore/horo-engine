#include "Horo/PlatformServices/PlatformServicesBackend.h"
#include "PlatformServicesTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace Horo::PlatformServices {
    using TestSupport::AvailableCapabilities;

    namespace {
        template <typename T> Result<PlatformRequestHandle<T>> Unavailable() {
            return Result<PlatformRequestHandle<T>>::Failure(MakeError(BackendErrors::ServiceUnavailable));
        }

        class TestBackend final : public IPlatformServicesBackend {
        public:
            PlatformServiceCapabilitySnapshot snapshot;
            bool inspectFails{};
            bool activateFails{};
            bool activated{};
            std::uint32_t shutdownCalls{};

            Result<PlatformServiceCapabilitySnapshot> InspectCapabilities() const override {
                if (inspectFails)
                    return Result<PlatformServiceCapabilitySnapshot>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));
                return Result<PlatformServiceCapabilitySnapshot>::Success(snapshot);
            }

            Result<void> Activate(const PlatformServicesBackendConfig &) override {
                activated = true;
                if (activateFails)
                    return Result<void>::Failure(MakeError(BackendErrors::ServiceUnavailable));
                return Result<void>::Success();
            }

            Result<void> RequestCancel(PlatformRequestId, PlatformRequestGeneration) override {
                return Result<void>::Success();
            }

            Result<void> Shutdown() override {
                ++shutdownCalls;
                return Result<void>::Success();
            }

            Result<PlatformRequestHandle<void>> UnlockAchievement(AchievementUnlockRequest) override {
                return Unavailable<void>();
            }

            Result<PlatformRequestHandle<void>> SubmitScore(LeaderboardScoreRequest) override {
                return Unavailable<void>();
            }

            Result<PlatformRequestHandle<LeaderboardEntriesPage>> QueryRankedLeaderboard(LeaderboardRankedQuery) override {
                return Unavailable<LeaderboardEntriesPage>();
            }

            Result<PlatformRequestHandle<LeaderboardAroundSubjectResult>> QueryLeaderboardAroundSubject(
                LeaderboardAroundSubjectQuery) override {
                return Unavailable<LeaderboardAroundSubjectResult>();
            }

            Result<PlatformRequestHandle<LeaderboardEntriesPage>> QueryFriendsLeaderboard(LeaderboardFriendsQuery) override {
                return Unavailable<LeaderboardEntriesPage>();
            }

            Result<PlatformRequestHandle<void>> WriteStat(StatWriteRequest) override {
                return Unavailable<void>();
            }

            Result<PlatformRequestHandle<CloudReadResult>> ReadCloudObject(CloudReadRequest) override {
                return Unavailable<CloudReadResult>();
            }

            Result<PlatformRequestHandle<void>> WriteCloudObject(CloudWriteRequest) override {
                return Unavailable<void>();
            }

            Result<PlatformRequestHandle<void>> SetPresence(PresenceUpdateRequest) override {
                return Unavailable<void>();
            }

            Result<PlatformRequestHandle<void>> ClearPresence(PlatformSubjectHandle) override {
                return Unavailable<void>();
            }

            Result<PlatformRequestHandle<FriendsPage>> QueryFriends(FriendsQuery) override {
                return Unavailable<FriendsPage>();
            }

            Result<PlatformRequestHandle<PlatformSessionSnapshot>> QueryCurrentSession() override {
                return Unavailable<PlatformSessionSnapshot>();
            }
        };

        PlatformServiceCapabilitySnapshot Snapshot() {
            return AvailableCapabilities();
        }
    }  // namespace

    TEST_CASE("Platform service identities and interfaces stay typed and backend-neutral", "[platform-services][backend]") {
        static_assert(std::is_abstract_v<IPlatformServicesBackend>);
        static_assert(std::has_virtual_destructor_v<IPlatformServicesBackend>);
        static_assert(!std::is_same_v<AchievementId, LeaderboardId>);
        static_assert(!std::is_same_v<CloudObjectId, PresenceStatusId>);
        REQUIRE_FALSE(AchievementId{}.IsValid());
        REQUIRE(AchievementId{1}.IsValid());
        REQUIRE_FALSE(PlatformSubjectHandle{}.IsValid());
    }

    TEST_CASE("Capability snapshots require one coherent entry for every service", "[platform-services][backend][capability]") {
        PlatformServicesBackendConfig config;
        auto snapshot = Snapshot();
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasValue());

        snapshot.services[1].service = snapshot.services[0].service;
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).ErrorValue().code.Value() ==
              BackendErrors::InvalidCapabilitySnapshot.code.Value());
        snapshot = Snapshot();
        snapshot.services[0].service = static_cast<PlatformServiceKind>(255);
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.services[0].availability = static_cast<PlatformServiceAvailability>(255);
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.services[0].binding = PlatformServiceBindingId{};
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
    }

    TEST_CASE("Available and unavailable capability evidence cannot contradict itself", "[platform-services][backend][capability]") {
        PlatformServicesBackendConfig config;
        auto snapshot = Snapshot();
        snapshot.services[0].binding.reset();
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.services[0].unavailableReason = PlatformServiceUnavailableReason::ServiceUnsupported;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());

        snapshot = Snapshot();
        auto &friends = snapshot.services[static_cast<std::size_t>(PlatformServiceKind::Friends)];
        friends.availability = PlatformServiceAvailability::Unavailable;
        friends.binding.reset();
        friends.unavailableReason = PlatformServiceUnavailableReason::HostPolicyDenied;
        friends.limits = {};
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasValue());
        friends.binding = PlatformServiceBindingId{9};
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        friends.binding.reset();
        friends.unavailableReason.reset();
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());

        snapshot = Snapshot();
        snapshot.services[static_cast<std::size_t>(PlatformServiceKind::Cloud)].leaderboardQueries.ranked = true;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());

        snapshot = Snapshot();
        snapshot.services[static_cast<std::size_t>(PlatformServiceKind::LeaderboardsAndStats)].limits.maxPageEntries = 0;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());

        snapshot = Snapshot();
        snapshot.services[static_cast<std::size_t>(PlatformServiceKind::LeaderboardsAndStats)].leaderboardQueries = {};
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasValue());
    }

    TEST_CASE("Capability validation rejects incompatible versions identities limits and required absence",
              "[platform-services][backend][capability]") {
        PlatformServicesBackendConfig config;
        auto snapshot = Snapshot();
        snapshot.interfaceVersion.major = PlatformServicesBackendInterfaceMajor + 1;
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).ErrorValue().code.Value() ==
              BackendErrors::IncompatibleInterfaceVersion.code.Value());
        snapshot = Snapshot();
        snapshot.interfaceVersion.minor = PlatformServicesBackendInterfaceMinor + 1;
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).ErrorValue().code.Value() ==
              BackendErrors::IncompatibleInterfaceVersion.code.Value());
        snapshot = Snapshot();
        snapshot.provider = {};
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.providerGeneration = {};
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.services[0].limits.maxConcurrentRequests = 0;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.services[0].limits.maxPayloadBytes = (1ULL << 40U) + 1U;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());

        snapshot = Snapshot();
        const auto cloudIndex = static_cast<std::size_t>(PlatformServiceKind::Cloud);
        snapshot.services[cloudIndex].availability = PlatformServiceAvailability::Unavailable;
        snapshot.services[cloudIndex].binding.reset();
        snapshot.services[cloudIndex].unavailableReason = PlatformServiceUnavailableReason::ServiceUnsupported;
        snapshot.services[cloudIndex].limits = {};
        config.requiredServices[cloudIndex] = true;
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).ErrorValue().code.Value() ==
              BackendErrors::RequiredServiceUnavailable.code.Value());
    }

    TEST_CASE("Candidate validation precedes activation and preserves typed unsupported behavior",
              "[platform-services][backend][lifecycle]") {
        PlatformServicesBackendConfig config;
        TestBackend backend;
        backend.snapshot = Snapshot();
        backend.snapshot.interfaceVersion.major = 99;
        REQUIRE(ActivatePlatformServicesBackend(backend, config).HasError());
        CHECK_FALSE(backend.activated);

        backend.inspectFails = false;
        backend.activateFails = true;
        backend.snapshot = Snapshot();
        REQUIRE(ActivatePlatformServicesBackend(backend, config).HasError());
        CHECK(backend.activated);
        CHECK(backend.shutdownCalls == 1);

        backend.activateFails = false;
        backend.snapshot = Snapshot();
        const auto activated = ActivatePlatformServicesBackend(backend, config);
        REQUIRE(activated.HasValue());
        CHECK(backend.activated);
        CHECK(activated.Value().providerGeneration == PlatformProviderGeneration{7});
        const auto unsupported = backend.UnlockAchievement({{}, {5}});
        REQUIRE(unsupported.HasError());
        CHECK(unsupported.ErrorValue().code.Value() == BackendErrors::ServiceUnavailable.code.Value());

        backend.inspectFails = true;
        backend.activated = false;
        REQUIRE(ActivatePlatformServicesBackend(backend, config).HasError());
        CHECK_FALSE(backend.activated);
    }

    TEST_CASE("Explicit Null backend reports unavailable capabilities and rejects every remote operation",
              "[platform-services][backend][null]") {
        NullPlatformServicesBackend backend({7});
        const auto inspected = backend.InspectCapabilities();
        REQUIRE(inspected.HasValue());

        PlatformServicesBackendConfig config;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(inspected.Value(), config).HasValue());
        CHECK(backend.Activate(config).HasValue());
        for (std::size_t index = 0; index < inspected.Value().services.size(); ++index) {
            const auto &capability = inspected.Value().services[index];
            CHECK(capability.service == static_cast<PlatformServiceKind>(index));
            CHECK(capability.availability == PlatformServiceAvailability::Unavailable);
            CHECK(capability.unavailableReason == PlatformServiceUnavailableReason::NullProviderSelected);
            CHECK_FALSE(capability.binding.has_value());
            CHECK(capability.limits.maxConcurrentRequests == 0);
            CHECK(capability.limits.maxPageEntries == 0);
            CHECK(capability.limits.maxPayloadBytes == 0);
        }

        const auto expectNullFailure = [](const auto &result) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == "platform.provider.null");
        };
        expectNullFailure(backend.UnlockAchievement({}));
        expectNullFailure(backend.SubmitScore({}));
        expectNullFailure(backend.QueryRankedLeaderboard({}));
        expectNullFailure(backend.QueryLeaderboardAroundSubject({}));
        expectNullFailure(backend.QueryFriendsLeaderboard({}));
        expectNullFailure(backend.WriteStat({}));
        expectNullFailure(backend.ReadCloudObject({}));
        expectNullFailure(backend.WriteCloudObject({}));
        expectNullFailure(backend.SetPresence({}));
        expectNullFailure(backend.ClearPresence({}));
        expectNullFailure(backend.QueryFriends({}));
        expectNullFailure(backend.QueryCurrentSession());

        CHECK(backend.RequestCancel({}, {}).HasValue());
        CHECK(backend.Shutdown().HasValue());
        CHECK(backend.Shutdown().HasValue());

        config.requiredServices[static_cast<std::size_t>(PlatformServiceKind::Cloud)] = true;
        const auto rejectedActivation = backend.Activate(config);
        REQUIRE(rejectedActivation.HasError());
        CHECK(rejectedActivation.ErrorValue().code.Value() == BackendErrors::RequiredServiceUnavailable.code.Value());
    }

    TEST_CASE("Explicit Null backend rejects an invalid provider generation during inert inspection",
              "[platform-services][backend][null]") {
        NullPlatformServicesBackend backend({});
        const auto inspected = backend.InspectCapabilities();
        REQUIRE(inspected.HasError());
        CHECK(inspected.ErrorValue().code.Value() == BackendErrors::InvalidCapabilitySnapshot.code.Value());
    }

    TEST_CASE("Leaderboard page results preserve score ordering and competition ties", "[platform-services][backend][leaderboard]") {
        const LeaderboardRankedQuery query{.leaderboard = {2}, .startIndex = 4, .pageSize = 4};
        LeaderboardEntriesPage page{.startIndex = 4,
                                    .entries = {{.rank = 4, .score = std::int64_t{100}},
                                                {.rank = 4, .score = std::int64_t{100}},
                                                {.rank = 7, .score = std::int64_t{80}}},
                                    .hasMore = true};
        REQUIRE(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                    .HasValue());
        page.entries[1].rank = 5;
        CHECK(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                  .ErrorValue()
                  .code.Value() == LeaderboardErrors::InvalidResult.code.Value());
        page.entries = {{.rank = 5, .score = std::int64_t{80}}, {.rank = 6, .score = std::int64_t{100}}};
        CHECK(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                  .HasError());
        CHECK(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::LowestFirst)
                  .HasValue());

        page.entries = {{.rank = 4, .score = std::int64_t{100}},
                        {.rank = 4, .score = std::int64_t{100}},
                        {.rank = 8, .score = std::int64_t{80}}};
        CHECK(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                  .HasError());
        page.entries.front().rank = 6;
        page.entries[1].rank = 6;
        page.entries[2].rank = 7;
        CHECK(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                  .HasError());
    }

    TEST_CASE("Leaderboard page validation rejects invalid offsets and bounds", "[platform-services][backend][leaderboard]") {
        const LeaderboardRankedQuery query{.leaderboard = {2}, .startIndex = 4, .pageSize = 4};
        LeaderboardEntriesPage page;
        page = {.startIndex = 3, .entries = {{.rank = 4, .score = std::int64_t{80}}}, .hasMore = false};
        CHECK(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::LowestFirst)
                  .HasError());
        page = {.startIndex = 4, .entries = {}, .hasMore = true};
        CHECK(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                  .HasError());
        page = {.startIndex = 4,
                .entries = {{.rank = 1, .score = std::int64_t{100}},
                            {.rank = 1, .score = std::int64_t{100}},
                            {.rank = 3, .score = std::int64_t{80}},
                            {.rank = 4, .score = std::int64_t{70}},
                            {.rank = 5, .score = std::int64_t{60}}},
                .hasMore = false};
        CHECK(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                  .HasError());
    }

    TEST_CASE("Leaderboard pages preserve authored score kinds", "[platform-services][backend][leaderboard]") {
        const LeaderboardRankedQuery query{.leaderboard = {2}, .startIndex = 4, .pageSize = 4};
        LeaderboardEntriesPage page;
        page = {.startIndex = 4,
                .entries = {{.rank = 5, .score = std::uint64_t{100}}, {.rank = 6, .score = std::uint64_t{80}}},
                .hasMore = false};
        CHECK(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::UnsignedInteger64, LeaderboardOrdering::HighestFirst)
                  .HasValue());
        CHECK(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                  .HasError());
        page.entries[1].score = std::int64_t{80};
        CHECK(ValidateLeaderboardEntriesPage(page, query, ProgressionValueKind::UnsignedInteger64, LeaderboardOrdering::HighestFirst)
                  .HasError());

        const LeaderboardFriendsQuery friendsQuery{.leaderboard = {2}, .startIndex = 7, .pageSize = 2};
        page = {.startIndex = 7,
                .entries = {{.rank = 8, .score = std::int64_t{50}}, {.rank = 10, .score = std::int64_t{40}}},
                .hasMore = false};
        CHECK(ValidateLeaderboardEntriesPage(page, friendsQuery, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                  .HasValue());
        page.startIndex = 8;
        CHECK(ValidateLeaderboardEntriesPage(page, friendsQuery, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                  .HasError());
    }

    TEST_CASE("Around-subject results cannot exceed or contradict their bounded window", "[platform-services][backend][leaderboard]") {
        const LeaderboardAroundSubjectQuery query{.leaderboard = {2}, .entriesBefore = 1, .entriesAfter = 2};
        LeaderboardAroundSubjectResult result{.entries = {{.rank = 4, .score = std::int64_t{80}},
                                                          {.rank = 5, .score = std::int64_t{70}},
                                                          {.rank = 6, .score = std::int64_t{60}}},
                                              .subjectEntryIndex = 1,
                                              .hasEarlier = true,
                                              .hasLater = false};
        REQUIRE(
            ValidateLeaderboardAroundSubjectResult(result, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                .HasValue());

        result.subjectEntryIndex = 0;
        CHECK(
            ValidateLeaderboardAroundSubjectResult(result, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                .HasError());
        result = {.entries = {{.rank = 4, .score = std::int64_t{80}}},
                  .subjectEntryIndex = std::nullopt,
                  .hasEarlier = false,
                  .hasLater = false};
        CHECK(
            ValidateLeaderboardAroundSubjectResult(result, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                .HasError());
        result = {.entries = {{.rank = 1, .score = std::int64_t{100}},
                              {.rank = 1, .score = std::int64_t{100}},
                              {.rank = 3, .score = std::int64_t{80}},
                              {.rank = 4, .score = std::int64_t{70}},
                              {.rank = 5, .score = std::int64_t{60}}},
                  .subjectEntryIndex = 2,
                  .hasEarlier = false,
                  .hasLater = false};
        CHECK(
            ValidateLeaderboardAroundSubjectResult(result, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                .HasError());
        result = {.entries = {{.rank = 4, .score = std::uint64_t{80}}}, .subjectEntryIndex = 0, .hasEarlier = false, .hasLater = false};
        CHECK(
            ValidateLeaderboardAroundSubjectResult(result, query, ProgressionValueKind::SignedInteger64, LeaderboardOrdering::HighestFirst)
                .HasError());
    }
}  // namespace Horo::PlatformServices
