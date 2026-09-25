#include "Horo/PlatformServices/PlatformStatCacheCoordinator.h"
#include "PlatformServicesTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    using TestSupport::RequireError;

    namespace {
        [[nodiscard]] PlatformServicesIdSalt TestSalt() {
            PlatformServicesIdSalt salt;
            std::uint8_t nextByte = 133;
            for (auto &byte : salt.bytes)
                byte = static_cast<std::byte>(nextByte++);
            return salt;
        }

        struct StatFixture final {
            PlatformServicesIdSalt salt{TestSalt()};
            PlatformStableIdRegistry stableIds;
            std::shared_ptr<const StatDefinitionRegistry> registry;
            StatId snapshot;
            StatId maximum;
            StatId addOnce;

            StatFixture() {
                const auto entry = [this](const std::string_view key) {
                    const auto derived = DerivePlatformServiceStableId(salt, PlatformServiceIdKind::Stat, key);
                    REQUIRE(derived.HasValue());
                    return PlatformStableIdDeclaration{.kind = PlatformServiceIdKind::Stat,
                                                       .canonicalKey = std::string{key},
                                                       .storedId = derived.Value()};
                };
                const auto snapshotEntry = entry("stats.snapshot");
                const auto maximumEntry = entry("stats.maximum");
                const auto addOnceEntry = entry("stats.add_once");
                auto builtIds = BuildPlatformStableIdRegistry(
                    {.projectId = "platform-stat-coordinator-tests", .salt = salt, .entries = {snapshotEntry, maximumEntry, addOnceEntry}});
                REQUIRE(builtIds.HasValue());
                stableIds = std::move(builtIds).Value();
                snapshot = StatId{snapshotEntry.storedId.value};
                maximum = StatId{maximumEntry.storedId.value};
                addOnce = StatId{addOnceEntry.storedId.value};
                const std::vector<StatDefinition> definitions{{.id = snapshot,
                                                               .authority = ProgressionAuthorityMode::LocalProduct,
                                                               .valueKind = ProgressionValueKind::SignedInteger64,
                                                               .range = {.minimum = -100, .maximum = 100},
                                                               .mutation = StatMutationPolicy::SnapshotAtRevision,
                                                               .localizationKey = "stats.snapshot"},
                                                              {.id = maximum,
                                                               .authority = ProgressionAuthorityMode::LocalProduct,
                                                               .valueKind = ProgressionValueKind::SignedInteger64,
                                                               .range = {.minimum = 0, .maximum = 100},
                                                               .mutation = StatMutationPolicy::SetMaximum,
                                                               .localizationKey = "stats.maximum"},
                                                              {.id = addOnce,
                                                               .authority = ProgressionAuthorityMode::AuthorityServer,
                                                               .valueKind = ProgressionValueKind::UnsignedInteger64,
                                                               .range = {.minimum = 0, .maximum = 100},
                                                               .mutation = StatMutationPolicy::AddOnce,
                                                               .localizationKey = "stats.add_once"}};
                auto built = BuildStatDefinitionRegistry(stableIds, {.stableIdRegistryFingerprint = stableIds.Fingerprint(),
                                                                     .definitions = definitions});
                REQUIRE(built.HasValue());
                registry = std::make_shared<StatDefinitionRegistry>(std::move(built).Value());
            }
        };

        PlatformSessionSnapshot ActiveSession(const std::uint64_t generation = 5, const std::uint64_t accessRevision = 3,
                                              const std::byte nonceByte = std::byte{1}) {
            PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Active,
                                               .generation = {generation},
                                               .providerGeneration = {7},
                                               .accessRevision = {accessRevision}};
            candidate.capabilities.services.fill(PlatformSessionAccessState::Granted);
            PlatformSubjectNonce nonce;
            nonce.bytes.back() = nonceByte;
            candidate.subjectNonce = nonce;
            auto built = BuildPlatformSessionSnapshot(candidate);
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        PlatformStatMutationId Mutation(const std::uint8_t marker) {
            PlatformStatMutationId id;
            id.bytes.back() = static_cast<std::byte>(marker);
            return id;
        }

        PlatformStatStateEvidence Evidence(const PlatformSessionSnapshot &session, const StatId stat, const PlatformStatValue value,
                                           const std::uint64_t providerRevision = 11) {
            return {.stat = stat,
                    .value = value,
                    .providerGeneration = session.ProviderGeneration(),
                    .sessionGeneration = session.Generation(),
                    .accessRevision = session.AccessRevision(),
                    .providerRevision = providerRevision};
        }

        PlatformStatCacheRecord Record(const StatFixture &fixture, const PlatformSessionSnapshot &session,
                                       const PlatformStatStateEvidence state, const std::uint64_t capturedTick = 10,
                                       const std::uint64_t expiresAtTick = 20) {
            return {.subject = *session.Subject(),
                    .state = state,
                    .definitionFingerprint = fixture.registry->Fingerprint(),
                    .capturedTick = capturedTick,
                    .expiresAtTick = expiresAtTick};
        }

        PlatformStatCacheCoordinator Coordinator(const StatFixture &fixture, PlatformSessionSnapshot session = ActiveSession()) {
            auto created = PlatformStatCacheCoordinator::Create(fixture.registry, std::move(session),
                                                                {.maximumCacheEntries = 2,
                                                                 .maximumPendingWrites = 4,
                                                                 .maximumLedgerEntries = 8,
                                                                 .freshnessWindowTicks = 5});
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        PlatformStatWriteRequest SnapshotWrite(const PlatformSessionSnapshot &session, const StatFixture &fixture,
                                               const PlatformStatMutationId mutation, const std::int64_t value = 8,
                                               const std::uint64_t observedTick = 10) {
            return {.subject = *session.Subject(),
                    .accessRevision = session.AccessRevision(),
                    .stat = fixture.snapshot,
                    .authority = ProgressionAuthorityMode::LocalProduct,
                    .value = PlatformStatValue::FromSigned(value),
                    .expectedProviderRevision = 11,
                    .mutation = mutation,
                    .observedTick = observedTick};
        }
    }  // namespace

    TEST_CASE("Stat reads distinguish misses fresh hits and stale cache evidence", "[platform-services][stat][cache]") {
        const StatFixture fixture;
        const auto session = ActiveSession();
        const auto subject = *session.Subject();
        auto coordinator = Coordinator(fixture, session);

        const auto miss = coordinator.ReadStat({subject, session.AccessRevision(), fixture.snapshot, 10});
        REQUIRE(miss.HasValue());
        CHECK(miss.Value().disposition == PlatformStatReadDisposition::ProviderQuery);
        REQUIRE(miss.Value().query.has_value());

        const auto state = Evidence(session, fixture.snapshot, PlatformStatValue::FromSigned(42));
        REQUIRE(coordinator.PublishReadResult(*miss.Value().query, state).HasValue());

        const auto hit = coordinator.ReadStat({subject, session.AccessRevision(), fixture.snapshot, 12});
        REQUIRE(hit.HasValue());
        CHECK(hit.Value().disposition == PlatformStatReadDisposition::FreshCacheHit);
        REQUIRE(hit.Value().cached.has_value());
        CHECK(hit.Value().cached->state.value == PlatformStatValue::FromSigned(42));

        const auto stale = coordinator.ReadStat({subject, session.AccessRevision(), fixture.snapshot, 15});
        REQUIRE(stale.HasValue());
        CHECK(stale.Value().disposition == PlatformStatReadDisposition::StaleCacheQuery);
        CHECK(stale.Value().query.has_value());
    }

    TEST_CASE("Corrupt or cross-generation cache records fail closed without partial restore",
              "[platform-services][stat][cache][validation]") {
        const StatFixture fixture;
        const auto session = ActiveSession();
        auto coordinator = Coordinator(fixture, session);
        auto corrupt = Record(fixture, session, Evidence(session, fixture.snapshot, PlatformStatValue::FromSigned(7)));
        corrupt.state.providerRevision = 0;
        RequireError(coordinator.RestoreCache({corrupt}), StatCoordinatorErrors::CacheCorrupt);
        CHECK(coordinator.CacheEntryCount() == 0);

        auto valid = Record(fixture, session, Evidence(session, fixture.snapshot, PlatformStatValue::FromSigned(7)));
        REQUIRE(coordinator.RestoreCache({valid}).HasValue());
        CHECK(coordinator.CacheEntryCount() == 1);

        auto wrongNamespace = valid;
        wrongNamespace.definitionFingerprint.bytes.front() ^= 0xffU;
        RequireError(coordinator.RestoreCache({wrongNamespace}), StatCoordinatorErrors::CacheCorrupt);
        CHECK(coordinator.CacheEntryCount() == 1);
    }

    TEST_CASE("Cache restore rejects duplicate identities and over-capacity snapshots atomically",
              "[platform-services][stat][cache][validation]") {
        const StatFixture fixture;
        const auto session = ActiveSession();
        auto coordinator = Coordinator(fixture, session);
        const auto valid = Record(fixture, session, Evidence(session, fixture.snapshot, PlatformStatValue::FromSigned(7)));
        REQUIRE(coordinator.RestoreCache({valid}).HasValue());

        RequireError(coordinator.RestoreCache({valid, valid}), StatCoordinatorErrors::CacheCorrupt);
        CHECK(coordinator.CacheEntryCount() == 1);
        RequireError(coordinator.RestoreCache({valid, valid, valid}), StatCoordinatorErrors::CapacityExceeded);
        CHECK(coordinator.CacheEntryCount() == 1);

        const auto cached = coordinator.ReadStat({*session.Subject(), session.AccessRevision(), fixture.snapshot, 12});
        REQUIRE(cached.HasValue());
        CHECK(cached.Value().disposition == PlatformStatReadDisposition::FreshCacheHit);
        REQUIRE(cached.Value().cached.has_value());
        CHECK(cached.Value().cached->state.value == PlatformStatValue::FromSigned(7));
    }

    TEST_CASE("Stat writes enforce authority value schema revision and exact idempotency", "[platform-services][stat][write]") {
        const StatFixture fixture;
        const auto session = ActiveSession();
        auto coordinator = Coordinator(fixture, session);
        const auto valid = SnapshotWrite(session, fixture, Mutation(1));

        auto missingRevision = valid;
        missingRevision.expectedProviderRevision.reset();
        RequireError(coordinator.SubmitWrite(missingRevision), StatCoordinatorErrors::RevisionRequired);

        auto wrongAuthority = valid;
        wrongAuthority.authority = ProgressionAuthorityMode::AuthorityServer;
        RequireError(coordinator.SubmitWrite(wrongAuthority), StatCoordinatorErrors::AuthorityDenied);

        auto wrongValue = valid;
        wrongValue.value = PlatformStatValue::FromUnsigned(8);
        RequireError(coordinator.SubmitWrite(wrongValue), StatCoordinatorErrors::InvalidValue);

        REQUIRE(coordinator.SubmitWrite(valid).Value() == PlatformStatWriteAdmission::Queued);
        auto duplicate = valid;
        duplicate.observedTick = 99;
        CHECK(coordinator.SubmitWrite(duplicate).Value() == PlatformStatWriteAdmission::IgnoredDuplicate);

        auto conflict = valid;
        conflict.value = PlatformStatValue::FromSigned(9);
        RequireError(coordinator.SubmitWrite(conflict), StatCoordinatorErrors::IdempotencyConflict);

        const auto publication = coordinator.TakeNextWrite();
        REQUIRE(publication.HasValue());
        REQUIRE(publication.Value().has_value());
        REQUIRE(coordinator
                    .CompleteWrite(*publication.Value(), PlatformStatWriteOutcome::Succeeded,
                                   Evidence(session, fixture.snapshot, PlatformStatValue::FromSigned(8), 12))
                    .HasValue());
        CHECK_FALSE(coordinator.HasInFlight());
        CHECK(coordinator.CacheEntryCount() == 1);
    }

    TEST_CASE("Successful writes refresh only the typed cache and stale lifecycle evidence cannot publish",
              "[platform-services][stat][lifecycle]") {
        const StatFixture fixture;
        const auto session = ActiveSession();
        const auto subject = *session.Subject();
        auto coordinator = Coordinator(fixture, session);
        const auto query = coordinator.ReadStat({subject, session.AccessRevision(), fixture.snapshot, 10});
        REQUIRE(query.HasValue());
        REQUIRE(query.Value().query.has_value());

        const auto publication = coordinator.SubmitWrite(SnapshotWrite(session, fixture, Mutation(2)));
        REQUIRE(publication.HasValue());
        const auto token = coordinator.TakeNextWrite();
        REQUIRE(token.HasValue());
        REQUIRE(token.Value().has_value());

        REQUIRE(coordinator.UpdateSession(ActiveSession(6, 4, std::byte{2})).HasValue());
        CHECK(coordinator.CacheEntryCount() == 0);
        RequireError(coordinator.PublishReadResult(*query.Value().query,
                                                   Evidence(session, fixture.snapshot, PlatformStatValue::FromSigned(3))),
                     StatCoordinatorErrors::StaleState);
        RequireError(coordinator.CompleteWrite(*token.Value(), PlatformStatWriteOutcome::Cancelled),
                     StatCoordinatorErrors::StalePublication);
        CHECK_FALSE(coordinator.HasInFlight());
    }

    TEST_CASE("Stat coordinator enforces finite capacities and idempotent shutdown", "[platform-services][stat][configuration]") {
        const StatFixture fixture;
        const auto session = ActiveSession();
        RequireError(PlatformStatCacheCoordinator::Create({}, session), StatCoordinatorErrors::InvalidConfiguration);
        RequireError(PlatformStatCacheCoordinator::Create(fixture.registry, session, {.freshnessWindowTicks = 0}),
                     StatCoordinatorErrors::InvalidConfiguration);

        auto coordinator = PlatformStatCacheCoordinator::Create(fixture.registry, session,
                                                                {.maximumCacheEntries = 1,
                                                                 .maximumPendingWrites = 1,
                                                                 .maximumLedgerEntries = 1,
                                                                 .freshnessWindowTicks = 5});
        REQUIRE(coordinator.HasValue());
        auto value = std::move(coordinator).Value();
        auto first = SnapshotWrite(session, fixture, Mutation(3));
        REQUIRE(value.SubmitWrite(first).HasValue());
        auto second = SnapshotWrite(session, fixture, Mutation(4), 9);
        RequireError(value.SubmitWrite(second), StatCoordinatorErrors::CapacityExceeded);
        REQUIRE(value.Close().HasValue());
        REQUIRE(value.Close().HasValue());
        CHECK(value.IsClosed());
        RequireError(value.ReadStat({*session.Subject(), session.AccessRevision(), fixture.snapshot, 1}), StatCoordinatorErrors::Closed);
    }

    TEST_CASE("Failed and cancelled stat writes retain their mutation identity without refreshing cache",
              "[platform-services][stat][write][lifecycle]") {
        const StatFixture fixture;
        const auto session = ActiveSession();
        const auto request = SnapshotWrite(session, fixture, Mutation(11));
        for (const auto outcome : {PlatformStatWriteOutcome::Failed, PlatformStatWriteOutcome::Cancelled}) {
            auto coordinator = Coordinator(fixture, session);
            REQUIRE(coordinator.SubmitWrite(request).Value() == PlatformStatWriteAdmission::Queued);
            const auto publication = coordinator.TakeNextWrite();
            REQUIRE(publication.HasValue());
            REQUIRE(publication.Value().has_value());
            REQUIRE(coordinator.CompleteWrite(*publication.Value(), outcome).HasValue());
            CHECK_FALSE(coordinator.HasInFlight());
            CHECK(coordinator.CacheEntryCount() == 0);
            CHECK(coordinator.SubmitWrite(request).Value() == PlatformStatWriteAdmission::IgnoredDuplicate);
            RequireError(coordinator.CompleteWrite(*publication.Value(), outcome), StatCoordinatorErrors::StalePublication);
            REQUIRE(coordinator.Close().HasValue());
            RequireError(coordinator.SubmitWrite(request), StatCoordinatorErrors::Closed);
        }
    }

    TEST_CASE("Bounded stat cache evicts the older live entry when a new result is accepted",
              "[platform-services][stat][cache][capacity]") {
        const StatFixture fixture;
        const auto session = ActiveSession();
        auto created = PlatformStatCacheCoordinator::Create(fixture.registry, session,
                                                            {.maximumCacheEntries = 1,
                                                             .maximumPendingWrites = 1,
                                                             .maximumLedgerEntries = 1,
                                                             .freshnessWindowTicks = 5});
        REQUIRE(created.HasValue());
        auto coordinator = std::move(created).Value();
        const auto first = coordinator.ReadStat({*session.Subject(), session.AccessRevision(), fixture.snapshot, 10});
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().query.has_value());
        REQUIRE(coordinator.PublishReadResult(*first.Value().query, Evidence(session, fixture.snapshot, PlatformStatValue::FromSigned(3)))
                    .HasValue());
        const auto second = coordinator.ReadStat({*session.Subject(), session.AccessRevision(), fixture.maximum, 10});
        REQUIRE(second.HasValue());
        REQUIRE(second.Value().query.has_value());
        REQUIRE(coordinator.PublishReadResult(*second.Value().query, Evidence(session, fixture.maximum, PlatformStatValue::FromSigned(8)))
                    .HasValue());
        CHECK(coordinator.CacheEntryCount() == 1);
        CHECK(coordinator.ReadStat({*session.Subject(), session.AccessRevision(), fixture.snapshot, 11}).Value().disposition ==
              PlatformStatReadDisposition::ProviderQuery);
        CHECK(coordinator.ReadStat({*session.Subject(), session.AccessRevision(), fixture.maximum, 11}).Value().disposition ==
              PlatformStatReadDisposition::FreshCacheHit);
    }
}  // namespace Horo::PlatformServices
