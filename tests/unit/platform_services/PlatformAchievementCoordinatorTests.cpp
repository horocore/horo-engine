#include "Horo/PlatformServices/PlatformAchievementCoordinator.h"
#include "PlatformServicesTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    using TestSupport::RequireError;

    namespace {
        [[nodiscard]] PlatformServicesIdSalt TestSalt() {
            PlatformServicesIdSalt salt;
            std::uint8_t nextByte = 91;
            for (auto &byte : salt.bytes)
                byte = static_cast<std::byte>(nextByte++);
            return salt;
        }

        struct AchievementFixture final {
            PlatformStableIdRegistry stableIds;
            std::shared_ptr<const AchievementDefinitionRegistry> registry;
            AchievementId unlock;
            AchievementId progress;

            AchievementFixture() {
                const auto salt = TestSalt();
                const auto unlockValue = DerivePlatformServiceStableId(salt, PlatformServiceIdKind::Achievement, "achievement.first");
                const auto progressValue = DerivePlatformServiceStableId(salt, PlatformServiceIdKind::Achievement, "achievement.progress");
                REQUIRE(unlockValue.HasValue());
                REQUIRE(progressValue.HasValue());
                auto builtIds = BuildPlatformStableIdRegistry({.projectId = "platform-achievement-tests",
                                                               .salt = salt,
                                                               .entries = {{.kind = PlatformServiceIdKind::Achievement,
                                                                            .canonicalKey = "achievement.first",
                                                                            .storedId = unlockValue.Value()},
                                                                           {.kind = PlatformServiceIdKind::Achievement,
                                                                            .canonicalKey = "achievement.progress",
                                                                            .storedId = progressValue.Value()}}});
                REQUIRE(builtIds.HasValue());
                stableIds = std::move(builtIds).Value();
                unlock = AchievementId{unlockValue.Value().value};
                progress = AchievementId{progressValue.Value().value};
                const std::vector<AchievementDefinition>
                    definitions{{.id = unlock,
                                 .authority = ProgressionAuthorityMode::LocalProduct,
                                 .progress = {.kind = AchievementProgressKind::UnlockOnce, .total = 1},
                                 .presentation = {.titleLocalizationKey = "achievements.first",
                                                  .descriptionLocalizationKey = "achievements.first.description"}},
                                {.id = progress,
                                 .authority = ProgressionAuthorityMode::AuthorityServer,
                                 .progress = {.kind = AchievementProgressKind::SetProgressMaximum, .total = 5},
                                 .presentation = {.titleLocalizationKey = "achievements.progress",
                                                  .descriptionLocalizationKey = "achievements.progress.description"}}};
                auto built = BuildAchievementDefinitionRegistry(stableIds, {.stableIdRegistryFingerprint = stableIds.Fingerprint(),
                                                                            .definitions = definitions});
                REQUIRE(built.HasValue());
                registry = std::make_shared<AchievementDefinitionRegistry>(std::move(built).Value());
            }
        };

        PlatformSessionSnapshot ActiveSession(const std::uint64_t generation = 5, const std::uint64_t accessRevision = 3) {
            PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Active,
                                               .generation = {generation},
                                               .providerGeneration = {7},
                                               .accessRevision = {accessRevision}};
            candidate.capabilities.services.fill(PlatformSessionAccessState::Granted);
            PlatformSubjectNonce nonce;
            nonce.bytes.back() = std::byte{1};
            candidate.subjectNonce = nonce;
            auto built = BuildPlatformSessionSnapshot(candidate);
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        PlatformSessionSnapshot ClosingSession() {
            PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Closing,
                                               .generation = {6},
                                               .providerGeneration = {7},
                                               .accessRevision = {4},
                                               .reason = PlatformSessionReason::UserSignedOut};
            candidate.capabilities.services.fill(PlatformSessionAccessState::Unavailable);
            auto built = BuildPlatformSessionSnapshot(candidate);
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        PlatformAchievementMutationId Mutation(const std::uint8_t marker) {
            PlatformAchievementMutationId id;
            id.bytes.back() = static_cast<std::byte>(marker);
            return id;
        }

        PlatformAchievementCoordinator Coordinator(const AchievementFixture &fixture, PlatformSessionSnapshot session = ActiveSession()) {
            auto created = PlatformAchievementCoordinator::Create(fixture.registry, std::move(session),
                                                                  {.maximumPendingMutations = 4, .maximumLedgerEntries = 8});
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }
    }  // namespace

    TEST_CASE("Achievement mutations use registered authority and exact idempotency identities",
              "[platform-services][achievement][idempotency]") {
        const AchievementFixture fixture;
        const auto session = ActiveSession();
        const auto subject = *session.Subject();
        auto coordinator = Coordinator(fixture, session);
        const PlatformAchievementMutationRequest unlock{subject,
                                                        session.AccessRevision(),
                                                        fixture.unlock,
                                                        ProgressionAuthorityMode::LocalProduct,
                                                        AchievementProgressKind::UnlockOnce,
                                                        1,
                                                        Mutation(1)};

        REQUIRE(coordinator.SubmitMutation(unlock).Value() == PlatformAchievementMutationAdmission::Queued);
        CHECK(coordinator.SubmitMutation(unlock).Value() == PlatformAchievementMutationAdmission::IgnoredDuplicate);

        const PlatformAchievementMutationRequest conflicting{subject,
                                                             session.AccessRevision(),
                                                             fixture.progress,
                                                             ProgressionAuthorityMode::AuthorityServer,
                                                             AchievementProgressKind::SetProgressMaximum,
                                                             2,
                                                             Mutation(1)};
        RequireError(coordinator.SubmitMutation(conflicting), AchievementCoordinatorErrors::IdempotencyConflict);

        const auto publication = coordinator.TakeNextMutation();
        REQUIRE(publication.HasValue());
        REQUIRE(publication.Value().has_value());
        CHECK(publication.Value()->request.achievement == fixture.unlock);
        REQUIRE(coordinator.CompleteMutation(*publication.Value(), PlatformAchievementPublicationOutcome::Succeeded).HasValue());
        CHECK_FALSE(coordinator.HasInFlight());
        CHECK(coordinator.SubmitMutation(unlock).Value() == PlatformAchievementMutationAdmission::IgnoredDuplicate);
    }

    TEST_CASE("Achievement mutation validation rejects unknown, unauthorized and out-of-schema progress",
              "[platform-services][achievement][validation]") {
        const AchievementFixture fixture;
        const auto session = ActiveSession();
        const auto subject = *session.Subject();
        auto coordinator = Coordinator(fixture, session);

        RequireError(coordinator.SubmitMutation({subject, session.AccessRevision(), fixture.unlock,
                                                 ProgressionAuthorityMode::AuthorityServer, AchievementProgressKind::UnlockOnce, 1,
                                                 Mutation(2)}),
                     AchievementCoordinatorErrors::AuthorityDenied);
        RequireError(coordinator.SubmitMutation({subject, session.AccessRevision(), fixture.progress,
                                                 ProgressionAuthorityMode::AuthorityServer, AchievementProgressKind::SetProgressMaximum, 6,
                                                 Mutation(3)}),
                     AchievementCoordinatorErrors::InvalidProgress);
        RequireError(coordinator.SubmitMutation({subject, session.AccessRevision(), fixture.unlock, ProgressionAuthorityMode::LocalProduct,
                                                 AchievementProgressKind::UnlockOnce, 0, Mutation(4)}),
                     AchievementCoordinatorErrors::InvalidProgress);
        RequireError(coordinator.SubmitMutation({subject, session.AccessRevision(), AchievementId{99},
                                                 ProgressionAuthorityMode::LocalProduct, AchievementProgressKind::UnlockOnce, 1,
                                                 Mutation(5)}),
                     AchievementCoordinatorErrors::UnknownAchievement);
    }

    TEST_CASE("Achievement state queries enforce exact generations and bounded remote projection",
              "[platform-services][achievement][query]") {
        const AchievementFixture fixture;
        const auto session = ActiveSession();
        const auto subject = *session.Subject();
        auto coordinator = Coordinator(fixture, session);
        const auto query = coordinator.MakeStateQuery({subject, session.AccessRevision(), fixture.progress});
        REQUIRE(query.HasValue());

        const PlatformAchievementStateSnapshot
            valid{fixture.progress, session.ProviderGeneration(), session.Generation(), session.AccessRevision(), 12, 3, false};
        REQUIRE(coordinator.ValidateStateResult(query.Value(), valid).HasValue());

        auto malformed = valid;
        malformed.progress = 6;
        RequireError(coordinator.ValidateStateResult(query.Value(), malformed), AchievementCoordinatorErrors::InvalidState);
        malformed = valid;
        malformed.sessionGeneration = {4};
        RequireError(coordinator.ValidateStateResult(query.Value(), malformed), AchievementCoordinatorErrors::InvalidState);

        auto currentReplacement = ActiveSession(6, 4);
        REQUIRE(coordinator.UpdateSession(std::move(currentReplacement)).HasValue());
        RequireError(coordinator.ValidateStateResult(query.Value(), valid), AchievementCoordinatorErrors::StaleState);
    }

    TEST_CASE("Achievement session replacement invalidates old publication and old subject admission",
              "[platform-services][achievement][lifecycle]") {
        const AchievementFixture fixture;
        const auto session = ActiveSession();
        const auto subject = *session.Subject();
        auto coordinator = Coordinator(fixture, session);
        const PlatformAchievementMutationRequest request{subject,
                                                         session.AccessRevision(),
                                                         fixture.unlock,
                                                         ProgressionAuthorityMode::LocalProduct,
                                                         AchievementProgressKind::UnlockOnce,
                                                         1,
                                                         Mutation(7)};
        REQUIRE(coordinator.SubmitMutation(request).HasValue());
        const auto publication = coordinator.TakeNextMutation();
        REQUIRE(publication.HasValue());
        REQUIRE(publication.Value().has_value());
        REQUIRE(coordinator.UpdateSession(ClosingSession()).HasValue());
        CHECK_FALSE(coordinator.HasInFlight());
        CHECK(coordinator.PendingCount() == 0);
        RequireError(coordinator.CompleteMutation(*publication.Value(), PlatformAchievementPublicationOutcome::Cancelled),
                     AchievementCoordinatorErrors::StalePublication);
        RequireError(coordinator.SubmitMutation(request), PlatformSessionErrors::Closing);
    }

    TEST_CASE("Achievement coordinator enforces bounded configuration and capacity", "[platform-services][achievement][configuration]") {
        const AchievementFixture fixture;
        const auto session = ActiveSession();
        RequireError(PlatformAchievementCoordinator::Create({}, session), AchievementCoordinatorErrors::InvalidConfiguration);
        RequireError(PlatformAchievementCoordinator::Create(fixture.registry, session, {.maximumPendingMutations = 0}),
                     AchievementCoordinatorErrors::InvalidConfiguration);

        auto coordinator =
            PlatformAchievementCoordinator::Create(fixture.registry, session, {.maximumPendingMutations = 1, .maximumLedgerEntries = 1});
        REQUIRE(coordinator.HasValue());
        auto value = std::move(coordinator).Value();
        const auto subject = *session.Subject();
        REQUIRE(value
                    .SubmitMutation({subject, session.AccessRevision(), fixture.unlock, ProgressionAuthorityMode::LocalProduct,
                                     AchievementProgressKind::UnlockOnce, 1, Mutation(8)})
                    .HasValue());
        RequireError(value.SubmitMutation({subject, session.AccessRevision(), fixture.progress, ProgressionAuthorityMode::AuthorityServer,
                                           AchievementProgressKind::SetProgressMaximum, 2, Mutation(9)}),
                     AchievementCoordinatorErrors::CapacityExceeded);
    }
}  // namespace Horo::PlatformServices
