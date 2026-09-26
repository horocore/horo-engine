#include "PlatformServicesFrontendTestSupport.h"

namespace Horo::PlatformServices {
    namespace {
        CloudBlobWriteRequest MutationWrite(const PlatformSubjectHandle &subject) {
            const auto key = CloudSaveObjectKey::Copy(std::array{std::byte{4}});
            const std::array bytes{std::byte{1}, std::byte{2}};
            const auto owned = CloudBlobOwnedBytes::Copy(bytes);
            REQUIRE(key.HasValue());
            REQUIRE(owned.HasValue());
            CloudMutationId mutation;
            mutation.bytes.back() = std::byte{1};
            return {.subject = subject,
                    .key = key.Value(),
                    .bytes = owned.Value(),
                    .expectedDigest = ComputeSha256(bytes),
                    .precondition = CloudCreateIfAbsent{},
                    .mutation = mutation};
        }

        CloudMutationCapability MutationFacts() {
            return {.atomicity = CloudMutationAtomicity::ConditionalAtomicObject,
                    .createIfAbsent = true,
                    .replaceIfRevision = true,
                    .deleteIfRevision = true,
                    .durableMutationDedupe = true,
                    .maxNamespaceBytes = 32,
                    .maxObjectCount = 4,
                    .maxConcurrentMutations = 2};
        }
    }  // namespace

    TEST_CASE("Platform Services frontend routes every accepted typed operation exactly once", "[platform-services][frontend]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto session = Session();
        const auto subject = *session.Subject();
        auto frontend = Frontend(backend, session);

        REQUIRE(frontend.UnlockAchievement({subject, {1}}).HasValue());
        auto scoreRequest = frontend.SubmitScore({subject, {2}, std::int64_t{-9}});
        REQUIRE(scoreRequest.HasValue());
        CHECK(frontend.RequestCancel(scoreRequest.Value()).HasValue());
        CHECK(backend->cancelCalls == 1);
        CHECK(backend->requests.Query(scoreRequest.Value()).Value().state == PlatformRequestState::Cancelling);
        REQUIRE(frontend.SubmitScore({subject, {2}, std::uint64_t{123}}).HasValue());
        REQUIRE(frontend.QueryRankedLeaderboard({subject, {2}, 12, 4}).HasValue());
        REQUIRE(frontend.QueryLeaderboardAroundSubject({subject, {2}, 1, 2}).HasValue());
        REQUIRE(frontend.QueryFriendsLeaderboard({subject, {2}, 8, 3}).HasValue());
        REQUIRE(frontend.WriteStat({subject, {3}, 17}).HasValue());
        REQUIRE(frontend.ReadCloudObject({subject, {4}}).HasValue());
        REQUIRE(frontend.ListCloudObjects({.subject = subject, .pageSize = 2}).HasValue());
        const auto key = CloudSaveObjectKey::Copy(std::array{std::byte{4}});
        REQUIRE(key.HasValue());
        REQUIRE(frontend.ReadCloudObject({.subject = subject, .key = key.Value(), .maximumBytes = 4}).HasValue());
        REQUIRE(frontend.SetPresence({subject, {5}, "busy"}).HasValue());
        REQUIRE(frontend.ClearPresence(subject).HasValue());
        REQUIRE(frontend.QueryFriends({subject, 4}).HasValue());
        REQUIRE(frontend.QueryCurrentSession().HasValue());

        CHECK(backend->TotalCalls() == 14);
        CHECK(backend->calls[static_cast<std::size_t>(PlatformServiceKind::LeaderboardsAndStats)] == 6);
        CHECK(backend->calls[static_cast<std::size_t>(PlatformServiceKind::Cloud)] == 3);
        CHECK(backend->calls[static_cast<std::size_t>(PlatformServiceKind::Presence)] == 2);
        REQUIRE(backend->lastRankedQuery);
        CHECK(backend->lastRankedQuery->startIndex == 12);
        CHECK(backend->lastRankedQuery->pageSize == 4);
        REQUIRE(backend->lastAroundSubjectQuery);
        CHECK(backend->lastAroundSubjectQuery->entriesBefore == 1);
        CHECK(backend->lastAroundSubjectQuery->entriesAfter == 2);
        REQUIRE(backend->lastFriendsLeaderboardQuery);
        CHECK(backend->lastFriendsLeaderboardQuery->startIndex == 8);
        CHECK(backend->lastFriendsLeaderboardQuery->pageSize == 3);
    }

    TEST_CASE("Platform Services frontend rejects malformed identities and bounds before dispatch",
              "[platform-services][frontend][validation]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto session = Session();
        const auto subject = *session.Subject();
        auto frontend = Frontend(backend, session);

        RequireError(frontend.UnlockAchievement({subject, {}}), FrontendErrors::InvalidRequest);
        RequireError(frontend.SubmitScore({subject, {}, std::int64_t{0}}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryRankedLeaderboard({subject, {}, 0, 1}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryLeaderboardAroundSubject({subject, {}, 0, 0}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryFriendsLeaderboard({subject, {}, 0, 1}), FrontendErrors::InvalidRequest);
        RequireError(frontend.WriteStat({subject, {}, 0}), FrontendErrors::InvalidRequest);
        RequireError(frontend.ReadCloudObject(CloudReadRequest{subject, {}}), FrontendErrors::InvalidRequest);
        RequireError(frontend.ListCloudObjects({.subject = subject, .pageSize = 0}), FrontendErrors::InvalidRequest);
        RequireError(frontend.ReadCloudObject({.subject = subject, .key = {}, .maximumBytes = 4}), FrontendErrors::InvalidRequest);
        const auto key = CloudSaveObjectKey::Copy(std::array{std::byte{1}});
        REQUIRE(key.HasValue());
        RequireError(frontend.ReadCloudObject({.subject = subject, .key = key.Value(), .maximumBytes = 5}), FrontendErrors::InvalidRequest);
        RequireError(frontend.SetPresence({subject, {1}, "12345"}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryFriends({subject, 0}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryFriends({subject, 5}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryRankedLeaderboard({subject, {2}, 0, 0}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryRankedLeaderboard({subject, {2}, 0, 5}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryRankedLeaderboard({subject, {2}, std::numeric_limits<std::uint32_t>::max(), 1}),
                     FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryLeaderboardAroundSubject({subject, {2}, 2, 2}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryLeaderboardAroundSubject({subject, {2}, std::numeric_limits<std::uint32_t>::max(), 1}),
                     FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryFriendsLeaderboard({subject, {2}, 0, 0}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryFriendsLeaderboard({subject, {2}, 0, 5}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryFriendsLeaderboard({subject, {2}, std::numeric_limits<std::uint32_t>::max(), 1}),
                     FrontendErrors::InvalidRequest);
        const PlatformRequestHandle<void> invalidHandle;
        RequireError(frontend.RequestCancel(invalidHandle), FrontendErrors::InvalidRequest);
        CHECK(backend->TotalCalls() == 0);
        CHECK(backend->cancelCalls == 0);
        CHECK(backend->requests.RecordCount() == 0);
    }

    TEST_CASE("Platform Services frontend rejects stale session handles", "[platform-services][frontend][admission]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto oldSession = Session({7}, {4}, PlatformSessionPhase::Active, PlatformSessionAccessState::Granted, std::byte{2});
        const auto currentSession = Session();
        auto frontend = Frontend(backend, currentSession);
        RequireError(frontend.UnlockAchievement({*oldSession.Subject(), {1}}), PlatformSessionErrors::StaleSession);
        RequireError(frontend.QueryRankedLeaderboard({*oldSession.Subject(), {2}, 0, 1}), PlatformSessionErrors::StaleSession);
        CHECK(backend->TotalCalls() == 0);
    }

    TEST_CASE("Platform Services frontend rejects denied session access", "[platform-services][frontend][admission]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto denied = Session({7}, {5}, PlatformSessionPhase::Active, PlatformSessionAccessState::Denied);
        auto frontend = Frontend(backend, denied);
        RequireError(frontend.QueryFriends({*denied.Subject(), 1}), PlatformSessionErrors::AccessDenied);
        RequireError(frontend.QueryFriendsLeaderboard({*denied.Subject(), {2}, 0, 1}), PlatformSessionErrors::AccessDenied);
        CHECK(backend->TotalCalls() == 0);
    }

    TEST_CASE("Friends leaderboard requires the separate friends consent grant", "[platform-services][frontend][admission]") {
        auto backend = std::make_shared<RoutingBackend>();
        PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Active,
                                           .generation = {5},
                                           .providerGeneration = {7},
                                           .accessRevision = {3}};
        PlatformSubjectNonce nonce;
        nonce.bytes.back() = std::byte{1};
        candidate.subjectNonce = nonce;
        candidate.capabilities.services.fill(PlatformSessionAccessState::Granted);
        candidate.capabilities.services[static_cast<std::size_t>(PlatformServiceKind::Friends)] =
            PlatformSessionAccessState::ConsentRequired;
        auto built = BuildPlatformSessionSnapshot(candidate);
        REQUIRE(built.HasValue());
        const auto session = std::move(built).Value();
        auto frontend = Frontend(backend, session);
        RequireError(frontend.QueryFriendsLeaderboard({*session.Subject(), {2}, 0, 1}), PlatformSessionErrors::ConsentRequired);
        CHECK(backend->TotalCalls() == 0);
    }

    TEST_CASE("Platform Services frontend rejects subject operations without an active session",
              "[platform-services][frontend][admission]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto active = Session();
        auto frontend = Frontend(backend, Session({7}, {5}, PlatformSessionPhase::NoSubject));
        RequireError(frontend.ClearPresence(*active.Subject()), PlatformSessionErrors::NoSubject);
        CHECK(backend->TotalCalls() == 0);
    }

    TEST_CASE("Friends leaderboard honors leaderboard and Friends operation policies", "[platform-services][frontend][admission]") {
        SECTION("leaderboard policy") {
            auto backend = std::make_shared<RoutingBackend>();
            const auto session = Session();
            PlatformServicesOperationPolicy policy;
            policy.deniedServices[static_cast<std::size_t>(PlatformServiceKind::LeaderboardsAndStats)] = true;
            auto frontend = Frontend(backend, session, policy);
            RequireError(frontend.QueryFriendsLeaderboard({*session.Subject(), {2}, 0, 1}), FrontendErrors::OperationDenied);
            CHECK(backend->TotalCalls() == 0);
        }
        SECTION("Friends policy") {
            auto backend = std::make_shared<RoutingBackend>();
            const auto session = Session();
            PlatformServicesOperationPolicy policy;
            policy.deniedServices[static_cast<std::size_t>(PlatformServiceKind::Friends)] = true;
            auto frontend = Frontend(backend, session, policy);
            RequireError(frontend.QueryFriendsLeaderboard({*session.Subject(), {2}, 0, 1}), FrontendErrors::OperationDenied);
            CHECK(backend->TotalCalls() == 0);
        }
    }

    TEST_CASE("Platform Services frontend distinguishes unavailable and Null capabilities before dispatch",
              "[platform-services][frontend][capability]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto session = Session();
        backend->snapshot = Capabilities();
        auto &friends = backend->snapshot.services[static_cast<std::size_t>(PlatformServiceKind::Friends)];
        friends.availability = PlatformServiceAvailability::Unavailable;
        friends.limits = {};
        friends.binding.reset();
        friends.unavailableReason = PlatformServiceUnavailableReason::ServiceUnsupported;
        auto created = PlatformServicesFrontend::Create(backend, backend->snapshot, session);
        REQUIRE(created.HasValue());
        auto frontend = std::move(created).Value();
        RequireError(frontend.QueryFriends({*session.Subject(), 1}), BackendErrors::ServiceUnavailable);
        CHECK(backend->TotalCalls() == 0);

        backend = std::make_shared<RoutingBackend>();
        backend->snapshot = Capabilities();
        auto &cloud = backend->snapshot.services[static_cast<std::size_t>(PlatformServiceKind::Cloud)];
        cloud.availability = PlatformServiceAvailability::Unavailable;
        cloud.limits = {};
        cloud.binding.reset();
        cloud.unavailableReason = PlatformServiceUnavailableReason::NullProviderSelected;
        auto nullCreated = PlatformServicesFrontend::Create(backend, backend->snapshot, session);
        REQUIRE(nullCreated.HasValue());
        auto nullFrontend = std::move(nullCreated).Value();
        RequireError(nullFrontend.ReadCloudObject({*session.Subject(), {1}}), FrontendErrors::NullProvider);
        CHECK(backend->TotalCalls() == 0);
    }

    TEST_CASE("Platform Services frontend rejects only the explicitly unsupported leaderboard query kinds",
              "[platform-services][frontend][capability]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto session = Session();
        backend->snapshot = Capabilities();
        backend->snapshot.services[static_cast<std::size_t>(PlatformServiceKind::LeaderboardsAndStats)].leaderboardQueries = {};
        auto created = PlatformServicesFrontend::Create(backend, backend->snapshot, session);
        REQUIRE(created.HasValue());
        auto frontend = std::move(created).Value();

        RequireError(frontend.QueryRankedLeaderboard({*session.Subject(), {2}, 0, 1}), BackendErrors::UnsupportedOperation);
        RequireError(frontend.QueryLeaderboardAroundSubject({*session.Subject(), {2}, 0, 0}), BackendErrors::UnsupportedOperation);
        RequireError(frontend.QueryFriendsLeaderboard({*session.Subject(), {2}, 0, 1}), BackendErrors::UnsupportedOperation);
        REQUIRE(frontend.SubmitScore({*session.Subject(), {2}, std::int64_t{10}}).HasValue());
        CHECK(backend->TotalCalls() == 1);
        CHECK(backend->requests.RecordCount() == 1);
    }

    TEST_CASE("Platform Services frontend rejects invalid or stale composition without request side effects",
              "[platform-services][frontend][composition]") {
        const auto session = Session();
        auto backend = std::make_shared<RoutingBackend>();
        backend->snapshot = Capabilities();

        RequireError(PlatformServicesFrontend::Create({}, backend->snapshot, session), FrontendErrors::InvalidComposition);
        auto invalid = backend->snapshot;
        invalid.services[0].service = static_cast<PlatformServiceKind>(255);
        RequireError(PlatformServicesFrontend::Create(backend, invalid, session), FrontendErrors::InvalidComposition);
        invalid = backend->snapshot;
        invalid.services[0].limits.maxConcurrentRequests = 0;
        RequireError(PlatformServicesFrontend::Create(backend, invalid, session), FrontendErrors::InvalidComposition);
        RequireError(PlatformServicesFrontend::Create(backend, backend->snapshot, Session({8})), FrontendErrors::InvalidComposition);
        CHECK(backend->inspectCalls == 0);

        invalid = backend->snapshot;
        invalid.services.back().service = invalid.services.front().service;
        RequireError(PlatformServicesFrontend::Create(backend, invalid, session), FrontendErrors::InvalidComposition);
        CHECK(backend->inspectCalls == 0);

        invalid = backend->snapshot;
        invalid.services[0].limits.maxPayloadBytes = 3;
        RequireError(PlatformServicesFrontend::Create(backend, invalid, session), FrontendErrors::InvalidComposition);
        CHECK(backend->inspectCalls == 1);

        backend->snapshot.services[1].service = backend->snapshot.services[0].service;
        RequireError(PlatformServicesFrontend::Create(backend, Capabilities(), session), FrontendErrors::InvalidComposition);
        CHECK(backend->inspectCalls == 2);
        CHECK(backend->TotalCalls() == 0);
        CHECK(backend->requests.RecordCount() == 0);
    }

    TEST_CASE("Platform Services frontend exposes only bounded public service limits", "[platform-services][frontend][capability]") {
        auto backend = std::make_shared<RoutingBackend>();
        auto frontend = Frontend(backend, Session());
        const auto limits = frontend.ServiceLimits(PlatformServiceKind::Cloud);
        REQUIRE(limits.HasValue());
        CHECK(limits.Value().maxPayloadBytes == 4);
        RequireError(frontend.ServiceLimits(static_cast<PlatformServiceKind>(255)), FrontendErrors::InvalidRequest);
    }

    TEST_CASE("Platform Services frontend admits only declared conditional cloud mutations", "[platform-services][frontend][cloud]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto session = Session();
        const auto subject = *session.Subject();
        auto frontend = Frontend(backend, session);
        RequireError(frontend.WriteCloudObject(MutationWrite(subject)), CloudObjectErrors::UnsupportedCapability);
        CHECK(backend->TotalCalls() == 0);

        REQUIRE(frontend.Close().HasValue());
        backend = std::make_shared<RoutingBackend>();
        backend->snapshot = Capabilities();
        backend->snapshot.services[static_cast<std::size_t>(PlatformServiceKind::Cloud)].cloudMutation = MutationFacts();
        auto created = PlatformServicesFrontend::Create(backend, backend->snapshot, session);
        REQUIRE(created.HasValue());
        auto enabled = std::move(created).Value();
        REQUIRE(enabled.WriteCloudObject(MutationWrite(subject)).HasValue());
        auto write = MutationWrite(subject);
        const auto revision = ProviderObjectRevision::Copy(std::array{std::byte{5}});
        REQUIRE(revision.HasValue());
        CloudBlobDeleteRequest remove{.subject = subject,
                                      .key = write.key,
                                      .expectedRevision = revision.Value(),
                                      .mutation = write.mutation};
        REQUIRE(enabled.DeleteCloudObject(remove).HasValue());
        REQUIRE(enabled.QueryCloudQuota(subject).HasValue());
        CHECK(backend->calls[static_cast<std::size_t>(PlatformServiceKind::Cloud)] == 3);
        write.mutation = {};
        RequireError(enabled.WriteCloudObject(write), CloudObjectErrors::InvalidRequest);
        CHECK(backend->calls[static_cast<std::size_t>(PlatformServiceKind::Cloud)] == 3);
        REQUIRE(enabled.Close().HasValue());
        RequireError(enabled.DeleteCloudObject(remove), FrontendErrors::Unavailable);
    }

    TEST_CASE("Platform Services frontend closes admission before one idempotent backend shutdown",
              "[platform-services][frontend][lifecycle]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto session = Session();
        auto frontend = Frontend(backend, session);
        auto scoreRequest = frontend.SubmitScore({*session.Subject(), {2}, std::int64_t{10}});
        REQUIRE(scoreRequest.HasValue());
        CHECK(frontend.IsOpen());
        REQUIRE(frontend.Close().HasValue());
        CHECK_FALSE(frontend.IsOpen());
        REQUIRE(frontend.Close().HasValue());
        CHECK(backend->shutdownCalls == 1);
        RequireError(frontend.QueryCurrentSession(), FrontendErrors::Unavailable);
        RequireError(frontend.QueryRankedLeaderboard({*session.Subject(), {2}, 0, 1}), FrontendErrors::Unavailable);
        RequireError(frontend.QueryLeaderboardAroundSubject({*session.Subject(), {2}, 0, 0}), FrontendErrors::Unavailable);
        RequireError(frontend.QueryFriendsLeaderboard({*session.Subject(), {2}, 0, 1}), FrontendErrors::Unavailable);
        RequireError(frontend.RequestCancel(scoreRequest.Value()), FrontendErrors::Unavailable);
        CHECK(backend->TotalCalls() == 1);
        CHECK(backend->cancelCalls == 0);

        backend = std::make_shared<RoutingBackend>();
        backend->shutdownFails = true;
        auto failingFrontend = Frontend(backend, session);
        RequireError(failingFrontend.Close(), BackendErrors::ServiceUnavailable);
        RequireError(failingFrontend.Close(), BackendErrors::ServiceUnavailable);
        CHECK(backend->shutdownCalls == 1);

        backend = std::make_shared<RoutingBackend>();
        backend->shutdownThrows = true;
        auto throwingFrontend = Frontend(backend, session);
        RequireError(throwingFrontend.Close(), BackendErrors::ServiceUnavailable);
        RequireError(throwingFrontend.Close(), BackendErrors::ServiceUnavailable);
        CHECK(backend->shutdownCalls == 1);

        backend = std::make_shared<RoutingBackend>();
        backend->shutdownThrowsNonStandard = true;
        auto nonStandardThrowingFrontend = Frontend(backend, session);
        RequireError(nonStandardThrowingFrontend.Close(), BackendErrors::ServiceUnavailable);
        RequireError(nonStandardThrowingFrontend.Close(), BackendErrors::ServiceUnavailable);
        CHECK(backend->shutdownCalls == 1);

        backend = std::make_shared<RoutingBackend>();
        backend->shutdownThrows = true;
        CHECK_NOTHROW(Frontend(backend, session));
        CHECK(backend->shutdownCalls == 1);
    }

    TEST_CASE("Platform Services frontend rejects malformed successful dispatch evidence", "[platform-services][frontend][dispatch]") {
        auto backend = std::make_shared<RoutingBackend>();
        backend->malformedHandle = true;
        const auto session = Session();
        auto frontend = Frontend(backend, session);
        RequireError(frontend.QueryCurrentSession(), FrontendErrors::InvalidDispatchResult);
        CHECK(backend->TotalCalls() == 1);
        CHECK(backend->requests.RecordCount() == 0);
    }

}  // namespace Horo::PlatformServices
