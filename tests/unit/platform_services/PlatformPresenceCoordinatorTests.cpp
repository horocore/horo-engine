#include "Horo/PlatformServices/PlatformPresenceCoordinator.h"
#include "PlatformServicesTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace Horo::PlatformServices {
    using TestSupport::RequireError;

    namespace {
        using namespace std::chrono_literals;

        [[nodiscard]] PlatformServicesIdSalt TestSalt() {
            PlatformServicesIdSalt salt;
            std::uint8_t nextByte = 71;
            for (auto &byte : salt.bytes)
                byte = static_cast<std::byte>(nextByte++);
            return salt;
        }

        struct PresenceFixture final {
            PlatformStableIdDeclaration entry;
            PlatformStableIdRegistry stableIds;
            std::shared_ptr<const PresenceDefinitionRegistry> registry;
            PresenceStatusId status;

            PresenceFixture() {
                const auto derived = DerivePlatformServiceStableId(TestSalt(), PlatformServiceIdKind::PresenceStatus, "presence.playing");
                REQUIRE(derived.HasValue());
                entry = {.kind = PlatformServiceIdKind::PresenceStatus, .canonicalKey = "presence.playing", .storedId = derived.Value()};
                auto stable =
                    BuildPlatformStableIdRegistry({.projectId = "platform-presence-tests", .salt = TestSalt(), .entries = {entry}});
                REQUIRE(stable.HasValue());
                stableIds = std::move(stable).Value();
                entry.storedId = stableIds.Entries().front().storedId;
                status = PresenceStatusId{entry.storedId.value};
                const PresenceDefinition definition{.id = status,
                                                    .detailPolicy = PresenceDetailPolicy::Optional,
                                                    .maximumDetailUtf8Bytes = 8,
                                                    .localizationKey = "presence.playing"};
                auto built = BuildPresenceDefinitionRegistry(stableIds, {.stableIdRegistryFingerprint = stableIds.Fingerprint(),
                                                                         .definitions = {definition}});
                REQUIRE(built.HasValue());
                registry = std::make_shared<PresenceDefinitionRegistry>(std::move(built).Value());
            }
        };

        PlatformSessionSnapshot ActiveSession(const PlatformSessionAccessState presenceAccess = PlatformSessionAccessState::Granted,
                                              const std::uint64_t generation = 5, const std::uint64_t accessRevision = 3) {
            PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Active,
                                               .generation = {generation},
                                               .providerGeneration = {7},
                                               .accessRevision = {accessRevision}};
            candidate.capabilities.services.fill(PlatformSessionAccessState::Granted);
            candidate.capabilities.services[static_cast<std::size_t>(PlatformServiceKind::Presence)] = presenceAccess;
            PlatformSubjectNonce nonce;
            nonce.bytes.back() = std::byte{1};
            candidate.subjectNonce = nonce;
            auto built = BuildPlatformSessionSnapshot(candidate);
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        PlatformSessionSnapshot ClosingSession(const std::uint64_t generation = 6, const std::uint64_t accessRevision = 4) {
            PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Closing,
                                               .generation = {generation},
                                               .providerGeneration = {7},
                                               .accessRevision = {accessRevision},
                                               .reason = PlatformSessionReason::UserSignedOut};
            candidate.capabilities.services.fill(PlatformSessionAccessState::Unavailable);
            auto built = BuildPlatformSessionSnapshot(candidate);
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        PlatformPresenceCoordinator Coordinator(const PresenceFixture &fixture, PlatformSessionSnapshot session = ActiveSession(),
                                                const std::chrono::milliseconds interval = 0ms) {
            auto created = PlatformPresenceCoordinator::Create(fixture.registry, std::move(session),
                                                               {.maximumDetailBytes = 8, .minimumInterval = interval});
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }
    }  // namespace

    TEST_CASE("Presence status mapping rejects unregistered IDs and coalesces latest valid detail",
              "[platform-services][presence][mapping]") {
        const PresenceFixture fixture;
        auto coordinator = Coordinator(fixture);
        const auto session = ActiveSession();
        const auto subject = *session.Subject();

        REQUIRE(coordinator.SubmitSet({subject, session.AccessRevision(), fixture.status, "first"}).Value() ==
                PlatformPresenceAdmission::Queued);
        CHECK(coordinator.SubmitSet({subject, session.AccessRevision(), fixture.status, "first"}).Value() ==
              PlatformPresenceAdmission::IgnoredDuplicate);
        CHECK(coordinator.SubmitSet({subject, session.AccessRevision(), fixture.status, "latest"}).Value() ==
              PlatformPresenceAdmission::Coalesced);

        const auto ready = coordinator.TakeReady(std::chrono::steady_clock::time_point{});
        REQUIRE(ready.HasValue());
        REQUIRE(ready.Value().has_value());
        CHECK_FALSE(ready.Value()->IsClear());
        CHECK(ready.Value()->intent.status == fixture.status);
        CHECK(ready.Value()->intent.detail == "latest");

        RequireError(coordinator.SubmitSet({subject, session.AccessRevision(), PresenceStatusId{99}, "latest"}),
                     PresenceCoordinatorErrors::StatusNotRegistered);
    }

    TEST_CASE("Presence detail policy, UTF-8 and access fences fail closed before publication",
              "[platform-services][presence][validation]") {
        const PresenceFixture fixture;
        const auto active = ActiveSession();
        const auto subject = *active.Subject();
        auto coordinator = Coordinator(fixture, active);

        RequireError(coordinator.SubmitSet({subject, active.AccessRevision(), fixture.status, "123456789"}),
                     PresenceCoordinatorErrors::DetailTooLarge);
        RequireError(coordinator.SubmitSet({subject, active.AccessRevision(), fixture.status, "\xC3"}),
                     PresenceCoordinatorErrors::DetailInvalidUtf8);
        RequireError(coordinator.SubmitSet({subject, {2}, fixture.status, "valid"}), PlatformSessionErrors::StaleAccessPolicy);

        auto denied = Coordinator(fixture, ActiveSession(PlatformSessionAccessState::Denied));
        const auto deniedSession = ActiveSession(PlatformSessionAccessState::Denied);
        RequireError(denied.SubmitClear({*deniedSession.Subject(), deniedSession.AccessRevision()}), PlatformSessionErrors::AccessDenied);

        auto forbiddenDefinition = PresenceDefinition{.id = fixture.status,
                                                      .detailPolicy = PresenceDetailPolicy::Forbidden,
                                                      .maximumDetailUtf8Bytes = 0,
                                                      .localizationKey = "presence.playing"};
        auto forbiddenRegistry =
            BuildPresenceDefinitionRegistry(fixture.stableIds, {.stableIdRegistryFingerprint = fixture.stableIds.Fingerprint(),
                                                                .definitions = {forbiddenDefinition}});
        REQUIRE(forbiddenRegistry.HasValue());
        auto forbidden =
            PlatformPresenceCoordinator::Create(std::make_shared<PresenceDefinitionRegistry>(std::move(forbiddenRegistry).Value()), active,
                                                {.maximumDetailBytes = 8, .minimumInterval = 0ms});
        REQUIRE(forbidden.HasValue());
        auto forbiddenCoordinator = std::move(forbidden).Value();
        RequireError(forbiddenCoordinator.SubmitSet({subject, active.AccessRevision(), fixture.status, "busy"}),
                     PresenceCoordinatorErrors::DetailForbidden);
    }

    TEST_CASE("Presence publication coalesces while in flight and respects the minimum interval",
              "[platform-services][presence][coalescing]") {
        const PresenceFixture fixture;
        auto coordinator = Coordinator(fixture, ActiveSession(), 100ms);
        const auto session = ActiveSession();
        const auto subject = *session.Subject();
        const auto start = std::chrono::steady_clock::time_point{};

        REQUIRE(coordinator.SubmitSet({subject, session.AccessRevision(), fixture.status, "one"}).HasValue());
        const auto first = coordinator.TakeReady(start);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().has_value());
        REQUIRE(coordinator.SubmitSet({subject, session.AccessRevision(), fixture.status, "two"}).Value() ==
                PlatformPresenceAdmission::Queued);
        CHECK_FALSE(coordinator.TakeReady(start).Value().has_value());
        REQUIRE(coordinator.Complete(*first.Value(), PlatformPresencePublicationOutcome::Succeeded, start).HasValue());
        CHECK_FALSE(coordinator.TakeReady(start + 99ms).Value().has_value());

        const auto second = coordinator.TakeReady(start + 100ms);
        REQUIRE(second.HasValue());
        REQUIRE(second.Value().has_value());
        CHECK(second.Value()->intent.detail == "two");
        CHECK(second.Value()->intent.sequence > first.Value()->intent.sequence);
        RequireError(coordinator.Complete(*first.Value(), PlatformPresencePublicationOutcome::Succeeded, start + 100ms),
                     PresenceCoordinatorErrors::StalePublication);
        REQUIRE(coordinator.Complete(*second.Value(), PlatformPresencePublicationOutcome::Failed, start + 100ms).HasValue());
    }

    TEST_CASE("Session replacement and sign-out invalidate pending and in-flight presence safely",
              "[platform-services][presence][lifecycle]") {
        const PresenceFixture fixture;
        const auto active = ActiveSession();
        const auto subject = *active.Subject();
        auto coordinator = Coordinator(fixture, active);
        REQUIRE(coordinator.SubmitSet({subject, active.AccessRevision(), fixture.status, "before"}).HasValue());
        const auto publication = coordinator.TakeReady(std::chrono::steady_clock::time_point{});
        REQUIRE(publication.HasValue());
        REQUIRE(publication.Value().has_value());
        REQUIRE(coordinator.SubmitClear({subject, active.AccessRevision()}).HasValue());
        CHECK(coordinator.HasPending());

        const auto update = coordinator.UpdateSession(ClosingSession());
        REQUIRE(update.HasValue());
        CHECK(update.Value() == PlatformPresenceSessionUpdate::Invalidated);
        CHECK_FALSE(coordinator.HasPending());
        CHECK_FALSE(coordinator.HasInFlight());
        RequireError(coordinator.Complete(*publication.Value(), PlatformPresencePublicationOutcome::Cancelled,
                                          std::chrono::steady_clock::time_point{}),
                     PresenceCoordinatorErrors::StalePublication);
        RequireError(coordinator.SubmitClear({subject, active.AccessRevision()}), PlatformSessionErrors::Closing);

        REQUIRE(coordinator.Close().HasValue());
        CHECK(coordinator.IsClosed());
        RequireError(coordinator.TakeReady(std::chrono::steady_clock::time_point{}), PresenceCoordinatorErrors::Closed);
    }

    TEST_CASE("Presence coordinator rejects unbounded configuration and empty registry ownership",
              "[platform-services][presence][configuration]") {
        const PresenceFixture fixture;
        const auto session = ActiveSession();
        RequireError(PlatformPresenceCoordinator::Create({}, session), PresenceCoordinatorErrors::InvalidConfiguration);
        RequireError(PlatformPresenceCoordinator::Create(fixture.registry, session, {.maximumDetailBytes = 0}),
                     PresenceCoordinatorErrors::InvalidConfiguration);
        RequireError(PlatformPresenceCoordinator::Create(fixture.registry, session, {.maximumDetailBytes = 8, .minimumInterval = -1ms}),
                     PresenceCoordinatorErrors::InvalidConfiguration);
    }
}  // namespace Horo::PlatformServices
