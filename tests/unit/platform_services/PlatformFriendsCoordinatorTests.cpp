#include "Horo/PlatformServices/PlatformFriendsCoordinator.h"
#include "Horo/PlatformServices/PlatformServicesFrontend.h"
#include "PlatformServicesMockBackend.h"
#include "PlatformServicesTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <type_traits>

namespace Horo::PlatformServices {
    namespace {
        using TestSupport::RequireError;
        using namespace std::chrono_literals;

        PlatformSessionSnapshot FriendsSession(PlatformSessionAccessState access = PlatformSessionAccessState::Granted,
                                               std::uint64_t generation = 1, std::uint64_t revision = 1) {
            PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Active,
                                               .generation = {generation},
                                               .providerGeneration = {7},
                                               .accessRevision = {revision}};
            candidate.capabilities.services.fill(PlatformSessionAccessState::Granted);
            candidate.capabilities.services[static_cast<std::size_t>(PlatformServiceKind::Friends)] = access;
            PlatformSubjectNonce nonce;
            nonce.bytes.back() = std::byte{1};
            candidate.subjectNonce = nonce;
            auto result = BuildPlatformSessionSnapshot(candidate);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        PlatformServiceCapability FriendsCapability() {
            return TestSupport::AvailableCapabilities().services[static_cast<std::size_t>(PlatformServiceKind::Friends)];
        }

        PlatformFriendsCoordinator FriendsCoordinator(PlatformSessionSnapshot session,
                                                      PlatformServiceCapability capability = FriendsCapability()) {
            auto capabilities = TestSupport::AvailableCapabilities();
            capabilities.services[static_cast<std::size_t>(PlatformServiceKind::Friends)] = std::move(capability);
            auto result = PlatformFriendsCoordinator::Create(std::move(capabilities), std::move(session),
                                                             {.maximumPageEntries = 2,
                                                              .maximumDisplayNameBytes = 8,
                                                              .maximumPageBytes = 12,
                                                              .retention = 10ms});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        FriendsPage ApprovedPage(const PlatformSessionSnapshot &session) {
            PlatformSubjectNonce nonce;
            nonce.bytes.front() = std::byte{9};
            auto handle = BuildPlatformSocialSubjectHandle(session, nonce);
            REQUIRE(handle.HasValue());
            return {.entries = {{handle.Value(), "Friend"}}};
        }

        PlatformFriendsQueryIntent Query(PlatformFriendsCoordinator &coordinator, const PlatformSessionSnapshot &session) {
            auto result = coordinator.BeginQuery({*session.Subject(), 2}, session.AccessRevision());
            REQUIRE(result.HasValue());
            return result.Value();
        }
    }  // namespace

    static_assert(!std::is_convertible_v<PlatformSocialSubjectHandle, PlatformSubjectHandle>);
    static_assert(!std::is_constructible_v<PlatformSocialSubjectHandle, PlatformSubjectNonce>);

    TEST_CASE("Friends composition rejects malformed limits and mismatched provider authority", "[platform-services][friends]") {
        const auto session = FriendsSession();
        RequireError(PlatformFriendsCoordinator::Create(TestSupport::AvailableCapabilities({8}), session),
                     FriendsCoordinatorErrors::InvalidConfiguration);
        auto capabilities = TestSupport::AvailableCapabilities();
        capabilities.services[0].service = PlatformServiceKind::Friends;
        RequireError(PlatformFriendsCoordinator::Create(capabilities, session), FriendsCoordinatorErrors::InvalidConfiguration);
        capabilities = TestSupport::AvailableCapabilities();
        for (const auto config :
             {PlatformFriendsCoordinatorConfig{.maximumPageEntries = 0}, PlatformFriendsCoordinatorConfig{.maximumDisplayNameBytes = 0},
              PlatformFriendsCoordinatorConfig{.maximumPageBytes = 0}, PlatformFriendsCoordinatorConfig{.retention = 0ms}})
            RequireError(PlatformFriendsCoordinator::Create(capabilities, session, config), FriendsCoordinatorErrors::InvalidConfiguration);
        RequireError(BuildPlatformSocialSubjectHandle(session, {}), PlatformSessionErrors::InvalidSnapshot);
        capabilities = TestSupport::AvailableCapabilities();
        capabilities.services[static_cast<std::size_t>(PlatformServiceKind::Friends)].limits.maxPageEntries = 0;
        RequireError(PlatformFriendsCoordinator::Create(capabilities, session), FriendsCoordinatorErrors::InvalidConfiguration);
    }

    TEST_CASE("Friends denial states never become an empty successful result", "[platform-services][friends][privacy]") {
        const std::array cases{std::pair{PlatformSessionAccessState::ConsentRequired, &PlatformSessionErrors::ConsentRequired},
                               std::pair{PlatformSessionAccessState::Denied, &PlatformSessionErrors::AccessDenied},
                               std::pair{PlatformSessionAccessState::Restricted, &PlatformSessionErrors::AccessRestricted},
                               std::pair{PlatformSessionAccessState::Revoked, &PlatformSessionErrors::AccessRevoked},
                               std::pair{PlatformSessionAccessState::Unavailable, &PlatformSessionErrors::AccessUnavailable}};
        for (const auto &[state, error] : cases) {
            const auto session = FriendsSession(state);
            auto coordinator = FriendsCoordinator(session);
            RequireError(coordinator.BeginQuery({*session.Subject(), 1}, session.AccessRevision()), *error);
            RequireError(coordinator.ReadPage(*session.Subject(), session.AccessRevision(), {}), *error);
            PlatformSubjectNonce nonce;
            nonce.bytes.front() = std::byte{1};
            RequireError(BuildPlatformSocialSubjectHandle(session, nonce), *error);
        }
        for (const auto reason :
             {PlatformServiceUnavailableReason::NullProviderSelected, PlatformServiceUnavailableReason::ServiceUnsupported}) {
            auto capability = FriendsCapability();
            capability.availability = PlatformServiceAvailability::Unavailable;
            capability.binding.reset();
            capability.limits = {};
            capability.unavailableReason = reason;
            const auto session = FriendsSession();
            auto coordinator = FriendsCoordinator(session, capability);
            RequireError(coordinator.BeginQuery({*session.Subject(), 1}, session.AccessRevision()),
                         reason == PlatformServiceUnavailableReason::NullProviderSelected ? BackendErrors::NullProvider
                                                                                          : BackendErrors::ServiceUnavailable);
        }
    }

    TEST_CASE("Friends page publication and retention are bounded and exactly once", "[platform-services][friends]") {
        const auto session = FriendsSession();
        auto coordinator = FriendsCoordinator(session);
        const auto query = Query(coordinator, session);
        RequireError(coordinator.BeginQuery(query.query, query.accessRevision), FriendsCoordinatorErrors::Busy);
        const auto now = std::chrono::steady_clock::time_point{} + 1s;
        REQUIRE(coordinator.Complete(query, Result<FriendsPage>::Success(ApprovedPage(session)), now).HasValue());
        RequireError(coordinator.Complete(query, Result<FriendsPage>::Success({}), now), FriendsCoordinatorErrors::StaleQuery);
        RequireError(coordinator.UpdateSession(FriendsSession(PlatformSessionAccessState::Denied)),
                     PlatformSessionErrors::StaleAccessPolicy);
        auto cached = coordinator.ReadPage(*session.Subject(), session.AccessRevision(), now + 9ms);
        REQUIRE(cached.HasValue());
        REQUIRE(cached.Value());
        REQUIRE(cached.Value()->entries.size() == 1);
        CHECK(cached.Value()->entries.front().subject.IsCurrent(session));
        CHECK_FALSE(coordinator.ReadPage(*session.Subject(), session.AccessRevision(), now + 10ms).Value());
        const auto next = Query(coordinator, session);
        REQUIRE(coordinator.Complete(next, Result<FriendsPage>::Success({}), now).HasValue());
        REQUIRE(coordinator.ReadPage(*session.Subject(), session.AccessRevision(), now).Value());
        CHECK(coordinator.ReadPage(*session.Subject(), session.AccessRevision(), now).Value()->entries.empty());
        CHECK_FALSE(coordinator.ReadPage(*session.Subject(), session.AccessRevision(), now - 1ms).Value());
    }

    TEST_CASE("Friends invalidates cache and late results before account or consent exposure", "[platform-services][friends][session]") {
        const auto session = FriendsSession();
        auto coordinator = FriendsCoordinator(session);
        const auto now = std::chrono::steady_clock::time_point{};
        const auto query = Query(coordinator, session);
        REQUIRE(coordinator.Complete(query, Result<FriendsPage>::Success(ApprovedPage(session)), now).HasValue());
        const auto late = Query(coordinator, session);
        const auto denied = FriendsSession(PlatformSessionAccessState::Restricted, 1, 2);
        REQUIRE(coordinator.UpdateSession(denied).HasValue());
        RequireError(coordinator.ReadPage(*session.Subject(), denied.AccessRevision(), now), PlatformSessionErrors::AccessRestricted);
        RequireError(coordinator.Complete(late, Result<FriendsPage>::Success(ApprovedPage(session)), now),
                     PlatformSessionErrors::StaleAccessPolicy);
        const auto restored = FriendsSession(PlatformSessionAccessState::Granted, 1, 3);
        REQUIRE(coordinator.UpdateSession(restored).HasValue());
        CHECK_FALSE(coordinator.ReadPage(*restored.Subject(), restored.AccessRevision(), now).Value());
        const auto switched = FriendsSession(PlatformSessionAccessState::Granted, 4, 6);
        REQUIRE(coordinator.UpdateSession(switched).HasValue());
        RequireError(coordinator.ReadPage(*session.Subject(), switched.AccessRevision(), now), PlatformSessionErrors::StaleSession);
        CHECK_FALSE(coordinator.ReadPage(*switched.Subject(), switched.AccessRevision(), now).Value());
        CHECK_FALSE(ApprovedPage(session).entries.front().subject.IsCurrent(switched));
        RequireError(coordinator.UpdateSession(session), PlatformSessionErrors::StaleSession);
    }

    TEST_CASE("Approved friends projections may redact display names", "[platform-services][friends][privacy]") {
        const auto session = FriendsSession();
        auto coordinator = FriendsCoordinator(session);
        auto page = ApprovedPage(session);
        page.entries.front().displayName.clear();
        const auto query = Query(coordinator, session);
        REQUIRE(coordinator.Complete(query, Result<FriendsPage>::Success(page), {}).HasValue());
        const auto retained = coordinator.ReadPage(*session.Subject(), session.AccessRevision(), {});
        REQUIRE(retained.HasValue());
        REQUIRE(retained.Value());
        REQUIRE(retained.Value()->entries.size() == 1);
        CHECK(retained.Value()->entries.front().displayName.empty());
    }

    TEST_CASE("Friends rejects malformed personal data atomically", "[platform-services][friends][privacy]") {
        const auto session = FriendsSession();
        auto coordinator = FriendsCoordinator(session);
        for (const unsigned variant : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}) {
            auto page = ApprovedPage(session);
            switch (variant) {
                case 0:
                    page.entries.front().displayName = std::string{"bad\0name", 8};
                    break;
                case 1:
                    page.entries.front().displayName = "\xC0\xAF";
                    break;
                case 2:
                    page.entries.front().displayName = "123456789";
                    break;
                case 3:
                    page.entries.push_back(page.entries.front());
                    break;
                case 4:
                    page.entries.front().subject = {};
                    break;
                case 5:
                    page.startIndex = 1;
                    break;
                case 6:
                    page.entries.clear();
                    page.hasMore = true;
                    break;
                case 7:
                    page.entries.front().subject =
                        ApprovedPage(FriendsSession(PlatformSessionAccessState::Granted, 2, 2)).entries.front().subject;
                    break;
            }
            const auto query = Query(coordinator, session);
            RequireError(coordinator.Complete(query, Result<FriendsPage>::Success(std::move(page)), {}),
                         FriendsCoordinatorErrors::InvalidPage);
            CHECK_FALSE(coordinator.ReadPage(*session.Subject(), session.AccessRevision(), {}).Value());
        }
        RequireError(coordinator.BeginQuery({*session.Subject(), 0}, session.AccessRevision()), FriendsCoordinatorErrors::InvalidQuery);
        RequireError(coordinator.BeginQuery({*session.Subject(), 3}, session.AccessRevision()), FriendsCoordinatorErrors::InvalidQuery);
        RequireError(coordinator.BeginQuery({*session.Subject(), 1, std::numeric_limits<std::uint32_t>::max()}, session.AccessRevision()),
                     FriendsCoordinatorErrors::InvalidQuery);
    }

    TEST_CASE("Friends cancellation failure and shutdown cannot publish late data", "[platform-services][friends][lifecycle]") {
        const auto session = FriendsSession();
        auto coordinator = FriendsCoordinator(session);
        const auto cancelled = Query(coordinator, session);
        REQUIRE(coordinator.Cancel(cancelled).HasValue());
        RequireError(coordinator.Complete(cancelled, Result<FriendsPage>::Success(ApprovedPage(session)), {}),
                     FriendsCoordinatorErrors::StaleQuery);
        for (const auto *error : {&RequestErrors::Cancelled, &RequestErrors::TimedOut, &BackendErrors::ServiceUnavailable}) {
            const auto failed = Query(coordinator, session);
            RequireError(coordinator.Complete(failed, Result<FriendsPage>::Failure(MakeError(*error)), {}), *error);
            CHECK_FALSE(coordinator.ReadPage(*session.Subject(), session.AccessRevision(), {}).Value());
        }
        const auto shutdown = Query(coordinator, session);
        REQUIRE(coordinator.Close().HasValue());
        REQUIRE(coordinator.Close().HasValue());
        RequireError(coordinator.Complete(shutdown, Result<FriendsPage>::Success(ApprovedPage(session)), {}),
                     FriendsCoordinatorErrors::Closed);
        RequireError(coordinator.ReadPage(*session.Subject(), session.AccessRevision(), {}), FriendsCoordinatorErrors::Closed);
    }

    TEST_CASE("Friends page offsets and aggregate personal data limits survive pagination", "[platform-services][friends]") {
        const auto session = FriendsSession();
        auto coordinator = FriendsCoordinator(session);
        auto admitted = coordinator.BeginQuery({*session.Subject(), 2, 2}, session.AccessRevision());
        REQUIRE(admitted.HasValue());
        auto page = ApprovedPage(session);
        page.startIndex = 2;
        page.hasMore = true;
        REQUIRE(coordinator.Complete(admitted.Value(), Result<FriendsPage>::Success(page), {}).HasValue());
        CHECK(coordinator.ReadPage(*session.Subject(), session.AccessRevision(), {}).Value()->startIndex == 2);
        PlatformSubjectNonce otherNonce;
        otherNonce.bytes.front() = std::byte{10};
        auto other = BuildPlatformSocialSubjectHandle(session, otherNonce);
        REQUIRE(other.HasValue());
        page.entries.front().displayName = "12345678";
        page.entries.push_back({other.Value(), "12345678"});
        admitted = coordinator.BeginQuery({*session.Subject(), 2, 2}, session.AccessRevision());
        REQUIRE(admitted.HasValue());
        RequireError(coordinator.Complete(admitted.Value(), Result<FriendsPage>::Success(page), {}), FriendsCoordinatorErrors::InvalidPage);
        page.entries.push_back(page.entries.front());
        admitted = coordinator.BeginQuery({*session.Subject(), 2, 2}, session.AccessRevision());
        REQUIRE(admitted.HasValue());
        RequireError(coordinator.Complete(admitted.Value(), Result<FriendsPage>::Success(page), {}), FriendsCoordinatorErrors::InvalidPage);
    }

    TEST_CASE("Scripted delayed friends results cannot cross session or access publication fences",
              "[platform-services][friends][qualification]") {
        using namespace TestSupport;
        const auto session = FriendsSession();
        auto backend = std::make_shared<MockPlatformServicesBackend>(PlatformProviderGeneration{7});
        REQUIRE(backend->Activate({}).HasValue());
        REQUIRE(backend->ExpectSequence({MockPlatformServicesOperation::QueryFriends}).HasValue());
        MockPlatformServicesResponse response;
        response.payload = ApprovedPage(session);
        response.delay = 10ms;
        REQUIRE(backend->SetResponse(MockPlatformServicesOperation::QueryFriends, std::move(response)).HasValue());
        const auto capabilities = backend->InspectCapabilities();
        REQUIRE(capabilities.HasValue());
        auto created = PlatformServicesFrontend::Create(backend, capabilities.Value(), session);
        REQUIRE(created.HasValue());
        auto frontend = std::move(created).Value();
        auto coordinator = FriendsCoordinator(session);
        const auto intent = Query(coordinator, session);
        auto request = frontend.QueryFriends(intent.query);
        REQUIRE(request.HasValue());
        REQUIRE(coordinator.UpdateSession(FriendsSession(PlatformSessionAccessState::Revoked, 1, 2)).HasValue());
        REQUIRE(backend->AdvanceClock(10ms).HasValue());
        CHECK(backend->DispatchDueCompletions() == 1);
        const auto completed = backend->Requests().Query(request.Value());
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value().terminal);
        REQUIRE(completed.Value().terminal->HasValue());
        RequireError(coordinator.Complete(intent, Result<FriendsPage>::Success(*completed.Value().terminal->Value()), {}),
                     PlatformSessionErrors::StaleAccessPolicy);
        REQUIRE(frontend.Close().HasValue());
        RequireError(frontend.QueryFriends(intent.query), FrontendErrors::Unavailable);
        REQUIRE(coordinator.UpdateSession(FriendsSession(PlatformSessionAccessState::Granted, 4, 6)).HasValue());
        RequireError(coordinator.Complete(intent, Result<FriendsPage>::Success(*completed.Value().terminal->Value()), {}),
                     PlatformSessionErrors::StaleSession);
        const auto current = FriendsSession(PlatformSessionAccessState::Granted, 4, 6);
        CHECK_FALSE(coordinator.ReadPage(*current.Subject(), current.AccessRevision(), {}).Value());
        REQUIRE(backend->VerifyExpectations().HasValue());
    }
}  // namespace Horo::PlatformServices
