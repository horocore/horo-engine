#include "Horo/PlatformServices/PlatformServicesFrontend.h"
#include "PlatformServicesTestSupport.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>

namespace Horo::PlatformServices {
    using TestSupport::AvailableCapabilities;
    using TestSupport::RequireError;

    namespace {
        constexpr std::size_t ServiceCount = static_cast<std::size_t>(PlatformServiceKind::Count);

        class RoutingBackend final : public IPlatformServicesBackend {
        public:
            PlatformServiceCapabilitySnapshot snapshot;
            PlatformRequestStore requests{{.activeCapacity = 32, .terminalCapacity = 32, .observerCapacity = 32, .generation = {19}}};
            std::array<std::uint32_t, ServiceCount> calls{};
            mutable std::uint32_t inspectCalls{};
            std::uint32_t shutdownCalls{};
            bool shutdownFails{};
            bool shutdownThrows{};
            bool shutdownThrowsNonStandard{};
            bool malformedHandle{};

            Result<PlatformServiceCapabilitySnapshot> InspectCapabilities() const override {
                ++inspectCalls;
                return Result<PlatformServiceCapabilitySnapshot>::Success(snapshot);
            }

            Result<void> Activate(const PlatformServicesBackendConfig &) override {
                return Result<void>::Success();
            }

            Result<void> RequestCancel(PlatformRequestId, PlatformRequestGeneration) override {
                return Result<void>::Success();
            }

            Result<void> Shutdown() override {
                ++shutdownCalls;
                if (shutdownThrows)
                    throw std::runtime_error("test backend shutdown failure");
                if (shutdownThrowsNonStandard)
                    throw 17;
                if (shutdownFails)
                    return Result<void>::Failure(MakeError(BackendErrors::ServiceUnavailable));
                return Result<void>::Success();
            }

            Result<PlatformRequestHandle<void>> UnlockAchievement(AchievementUnlockRequest) override {
                return Admit<void>(PlatformServiceKind::Achievements);
            }

            Result<PlatformRequestHandle<void>> SubmitScore(LeaderboardScoreRequest) override {
                return Admit<void>(PlatformServiceKind::LeaderboardsAndStats);
            }

            Result<PlatformRequestHandle<void>> WriteStat(StatWriteRequest) override {
                return Admit<void>(PlatformServiceKind::LeaderboardsAndStats);
            }

            Result<PlatformRequestHandle<CloudReadResult>> ReadCloudObject(CloudReadRequest) override {
                return Admit<CloudReadResult>(PlatformServiceKind::Cloud);
            }

            Result<PlatformRequestHandle<void>> WriteCloudObject(CloudWriteRequest) override {
                return Admit<void>(PlatformServiceKind::Cloud);
            }

            Result<PlatformRequestHandle<void>> SetPresence(PresenceUpdateRequest) override {
                return Admit<void>(PlatformServiceKind::Presence);
            }

            Result<PlatformRequestHandle<void>> ClearPresence(PlatformSubjectHandle) override {
                return Admit<void>(PlatformServiceKind::Presence);
            }

            Result<PlatformRequestHandle<FriendsPage>> QueryFriends(FriendsQuery) override {
                return Admit<FriendsPage>(PlatformServiceKind::Friends);
            }

            Result<PlatformRequestHandle<PlatformSessionSnapshot>> QueryCurrentSession() override {
                return Admit<PlatformSessionSnapshot>(PlatformServiceKind::Session);
            }

            [[nodiscard]] std::uint32_t TotalCalls() const noexcept {
                std::uint32_t result{};
                for (const auto count : calls)
                    result += count;
                return result;
            }

        private:
            template <typename T> Result<PlatformRequestHandle<T>> Admit(const PlatformServiceKind service) {
                ++calls[static_cast<std::size_t>(service)];
                if (malformedHandle)
                    return Result<PlatformRequestHandle<T>>::Success({});
                return requests.Admit<T>();
            }
        };

        PlatformServiceCapabilitySnapshot Capabilities(const PlatformProviderGeneration generation = {7}) {
            return AvailableCapabilities(generation, {.maxConcurrentRequests = 16, .maxPageEntries = 4, .maxPayloadBytes = 4});
        }

        PlatformSessionSnapshot Session(const PlatformProviderGeneration provider = {7}, const PlatformSessionGeneration generation = {5},
                                        const PlatformSessionPhase phase = PlatformSessionPhase::Active,
                                        const PlatformSessionAccessState access = PlatformSessionAccessState::Granted,
                                        const std::byte nonce = std::byte{1}) {
            PlatformSessionCandidate candidate{.phase = phase,
                                               .generation = generation,
                                               .providerGeneration = provider,
                                               .accessRevision = {3},
                                               .reason = phase == PlatformSessionPhase::Active ? PlatformSessionReason::None
                                                                                               : PlatformSessionReason::UserSignedOut};
            candidate.capabilities.services.fill(phase == PlatformSessionPhase::Active ? access : PlatformSessionAccessState::Unavailable);
            if (phase == PlatformSessionPhase::Active) {
                PlatformSubjectNonce subject;
                subject.bytes.back() = nonce;
                candidate.subjectNonce = subject;
            }
            auto built = BuildPlatformSessionSnapshot(candidate);
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        PlatformServicesFrontend Frontend(const std::shared_ptr<RoutingBackend> &backend, PlatformSessionSnapshot session,
                                          PlatformServicesOperationPolicy policy = {}) {
            backend->snapshot = Capabilities(session.ProviderGeneration());
            auto result = PlatformServicesFrontend::Create(backend, backend->snapshot, std::move(session), std::move(policy));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    }  // namespace

    TEST_CASE("Platform Services frontend routes every accepted typed operation exactly once", "[platform-services][frontend]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto session = Session();
        const auto subject = *session.Subject();
        auto frontend = Frontend(backend, session);

        REQUIRE(frontend.UnlockAchievement({subject, {1}}).HasValue());
        REQUIRE(frontend.SubmitScore({subject, {2}, -9}).HasValue());
        REQUIRE(frontend.WriteStat({subject, {3}, 17}).HasValue());
        REQUIRE(frontend.ReadCloudObject({subject, {4}}).HasValue());
        REQUIRE(frontend.WriteCloudObject({subject, {4}, {std::byte{1}, std::byte{2}}}).HasValue());
        REQUIRE(frontend.SetPresence({subject, {5}, "busy"}).HasValue());
        REQUIRE(frontend.ClearPresence(subject).HasValue());
        REQUIRE(frontend.QueryFriends({subject, 4}).HasValue());
        REQUIRE(frontend.QueryCurrentSession().HasValue());

        CHECK(backend->TotalCalls() == 9);
        CHECK(backend->calls[static_cast<std::size_t>(PlatformServiceKind::LeaderboardsAndStats)] == 2);
        CHECK(backend->calls[static_cast<std::size_t>(PlatformServiceKind::Cloud)] == 2);
        CHECK(backend->calls[static_cast<std::size_t>(PlatformServiceKind::Presence)] == 2);
    }

    TEST_CASE("Platform Services frontend rejects malformed identities and bounds before dispatch",
              "[platform-services][frontend][validation]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto session = Session();
        const auto subject = *session.Subject();
        auto frontend = Frontend(backend, session);

        RequireError(frontend.UnlockAchievement({subject, {}}), FrontendErrors::InvalidRequest);
        RequireError(frontend.SubmitScore({subject, {}, 0}), FrontendErrors::InvalidRequest);
        RequireError(frontend.WriteStat({subject, {}, 0}), FrontendErrors::InvalidRequest);
        RequireError(frontend.ReadCloudObject({subject, {}}), FrontendErrors::InvalidRequest);
        RequireError(frontend.WriteCloudObject({subject, {1}, std::vector<std::byte>(5)}), FrontendErrors::InvalidRequest);
        RequireError(frontend.SetPresence({subject, {1}, "12345"}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryFriends({subject, 0}), FrontendErrors::InvalidRequest);
        RequireError(frontend.QueryFriends({subject, 5}), FrontendErrors::InvalidRequest);
        CHECK(backend->TotalCalls() == 0);
        CHECK(backend->requests.RecordCount() == 0);
    }

    TEST_CASE("Platform Services frontend fails closed for session capability and product policy",
              "[platform-services][frontend][admission]") {
        SECTION("stale session") {
            auto backend = std::make_shared<RoutingBackend>();
            const auto oldSession = Session({7}, {4}, PlatformSessionPhase::Active, PlatformSessionAccessState::Granted, std::byte{2});
            const auto currentSession = Session();
            auto frontend = Frontend(backend, currentSession);
            RequireError(frontend.UnlockAchievement({*oldSession.Subject(), {1}}), PlatformSessionErrors::StaleSession);
            CHECK(backend->TotalCalls() == 0);
        }
        SECTION("denied access") {
            auto backend = std::make_shared<RoutingBackend>();
            const auto denied = Session({7}, {5}, PlatformSessionPhase::Active, PlatformSessionAccessState::Denied);
            auto frontend = Frontend(backend, denied);
            RequireError(frontend.QueryFriends({*denied.Subject(), 1}), PlatformSessionErrors::AccessDenied);
            CHECK(backend->TotalCalls() == 0);
        }
        SECTION("inactive session") {
            auto backend = std::make_shared<RoutingBackend>();
            const auto active = Session();
            auto frontend = Frontend(backend, Session({7}, {5}, PlatformSessionPhase::NoSubject));
            RequireError(frontend.ClearPresence(*active.Subject()), PlatformSessionErrors::NoSubject);
            CHECK(backend->TotalCalls() == 0);
        }
        SECTION("product policy") {
            auto backend = std::make_shared<RoutingBackend>();
            const auto session = Session();
            PlatformServicesOperationPolicy policy;
            policy.deniedServices[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = true;
            auto frontend = Frontend(backend, session, policy);
            RequireError(frontend.UnlockAchievement({*session.Subject(), {1}}), FrontendErrors::OperationDenied);
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

    TEST_CASE("A Null frontend rejects every typed read and write before request admission", "[platform-services][frontend][null]") {
        auto backend = std::make_shared<NullPlatformServicesBackend>(PlatformProviderGeneration{7});
        PlatformServicesBackendConfig config;
        auto activated = ActivatePlatformServicesBackend(*backend, config);
        REQUIRE(activated.HasValue());

        const auto session = Session();
        auto created = PlatformServicesFrontend::Create(backend, std::move(activated).Value(), session);
        REQUIRE(created.HasValue());
        auto frontend = std::move(created).Value();
        const auto subject = *session.Subject();

        const auto directNull = backend->ReadCloudObject({subject, {1}});
        const auto routedNull = frontend.ReadCloudObject({subject, {1}});
        REQUIRE(directNull.HasError());
        REQUIRE(routedNull.HasError());
        CHECK(&FrontendErrors::NullProvider == &BackendErrors::NullProvider);
        CHECK(directNull.ErrorValue().domain.Value() == routedNull.ErrorValue().domain.Value());
        CHECK(directNull.ErrorValue().code.Value() == routedNull.ErrorValue().code.Value());
        CHECK(directNull.ErrorValue().severity == routedNull.ErrorValue().severity);
        CHECK(directNull.ErrorValue().message == routedNull.ErrorValue().message);

        RequireError(frontend.UnlockAchievement({subject, {1}}), FrontendErrors::NullProvider);
        RequireError(frontend.SubmitScore({subject, {1}, 5}), FrontendErrors::NullProvider);
        RequireError(frontend.WriteStat({subject, {1}, 5}), FrontendErrors::NullProvider);
        RequireError(frontend.ReadCloudObject({subject, {1}}), FrontendErrors::NullProvider);
        RequireError(frontend.WriteCloudObject({subject, {1}, {}}), FrontendErrors::NullProvider);
        RequireError(frontend.SetPresence({subject, {1}, ""}), FrontendErrors::NullProvider);
        RequireError(frontend.ClearPresence(subject), FrontendErrors::NullProvider);
        RequireError(frontend.QueryFriends({subject, 1}), FrontendErrors::NullProvider);
        RequireError(frontend.QueryCurrentSession(), FrontendErrors::NullProvider);

        CHECK(frontend.Close().HasValue());
        CHECK_FALSE(frontend.IsOpen());
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

    TEST_CASE("Platform Services frontend closes admission before one idempotent backend shutdown",
              "[platform-services][frontend][lifecycle]") {
        auto backend = std::make_shared<RoutingBackend>();
        const auto session = Session();
        auto frontend = Frontend(backend, session);
        CHECK(frontend.IsOpen());
        REQUIRE(frontend.Close().HasValue());
        CHECK_FALSE(frontend.IsOpen());
        REQUIRE(frontend.Close().HasValue());
        CHECK(backend->shutdownCalls == 1);
        RequireError(frontend.QueryCurrentSession(), FrontendErrors::Unavailable);
        CHECK(backend->TotalCalls() == 0);

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
