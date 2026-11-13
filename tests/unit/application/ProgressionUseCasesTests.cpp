#include "../platform_services/PlatformServicesMockBackend.h"
#include "AllocationProbe.h"
#include "Horo/Application/ProgressionUseCases.h"
#include "Horo/PlatformServices/PlatformServicesBackend.h"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::PlatformServices;

    struct Authority final : IProgressionFactAuthority {
        mutable unsigned calls{};
        bool server{};
        std::optional<Error> error;

        Result<void> Authorize(const PlatformProgressionMutationCandidate &candidate) const override {
            ++calls;
            if (error)
                return Result<void>::Failure(*error);
            if (candidate.authority == ProgressionAuthorityMode::AuthorityServer && !server)
                return Result<void>::Failure(MakeError(PlatformProgressionErrors::InvalidMutation));
            return Result<void>::Success();
        }
    };

    struct Gate final : IProgressionProjectionGate {
        mutable unsigned calls{};
        std::optional<Error> error;

        Result<void> Validate(const PlatformProgressionMutationEnvelope &) const override {
            ++calls;
            if (error)
                return Result<void>::Failure(*error);
            return Result<void>::Success();
        }
    };

    struct ReplayQualifier final : IPlatformProgressionReplayQualifier {
        mutable unsigned calls{};
        std::optional<Error> failure;
        bool corruptEnvelope{};

        Result<PlatformProgressionQualifiedReplayIntent> Qualify(const PlatformProgressionMutationEnvelope &envelope,
                                                                 const PlatformProgressionReplayRequirements &requirements,
                                                                 LeaderboardOrdering ordering) const override {
            ++calls;
            if (failure)
                return Result<PlatformProgressionQualifiedReplayIntent>::Failure(*failure);
            auto original = envelope;
            if (corruptEnvelope)
                original.value = std::int64_t{99};
            return SealQualifiedIntent(original, requirements, PlatformProgressionReplaySafety::AtomicComparison, ordering);
        }

        Result<void> Revalidate(const PlatformProgressionQualifiedReplayIntent &) const override {
            return failure ? Result<void>::Failure(*failure) : Result<void>::Success();
        }
    };

    ProgressionDefinitions Definitions(ProgressionAuthorityMode authority = ProgressionAuthorityMode::LocalProduct) {
        PlatformServicesIdSalt salt;
        salt.bytes.fill(std::byte{13});
        std::vector<PlatformStableIdDeclaration> entries;
        for (const auto kind : {PlatformServiceIdKind::Achievement, PlatformServiceIdKind::Stat, PlatformServiceIdKind::Leaderboard}) {
            const std::string key = kind == PlatformServiceIdKind::Achievement ? "progress.win"
                                    : kind == PlatformServiceIdKind::Stat      ? "progress.stat"
                                                                               : "progress.board";
            const auto id = DerivePlatformServiceStableId(salt, kind, key);
            REQUIRE(id.HasValue());
            entries.push_back({.kind = kind, .canonicalKey = key, .storedId = id.Value()});
        }
        auto ids = BuildPlatformStableIdRegistry({.projectId = "project.progression", .salt = salt, .entries = entries});
        REQUIRE(ids.HasValue());
        auto achievements =
            BuildAchievementDefinitionRegistry(ids.Value(),
                                               {.stableIdRegistryFingerprint = ids.Value().Fingerprint(),
                                                .definitions = {
                                                    {.id = {entries[0].storedId.value},
                                                     .authority = authority,
                                                     .progress = {.kind = AchievementProgressKind::SetProgressMaximum, .total = 100},
                                                     .presentation = {.titleLocalizationKey = "progress.win.title",
                                                                      .descriptionLocalizationKey = "progress.win.description"}}}});
        REQUIRE(achievements.HasValue());
        auto stats = BuildStatDefinitionRegistry(ids.Value(), {.stableIdRegistryFingerprint = ids.Value().Fingerprint(),
                                                               .definitions = {{.id = {entries[1].storedId.value},
                                                                                .authority = authority,
                                                                                .valueKind = ProgressionValueKind::SignedInteger64,
                                                                                .range = {-100, 100},
                                                                                .mutation = StatMutationPolicy::SetMaximum,
                                                                                .localizationKey = "progress.stat"}}});
        REQUIRE(stats.HasValue());
        auto boards = BuildLeaderboardDefinitionRegistry(ids.Value(), stats.Value(),
                                                         {.stableIdRegistryFingerprint = ids.Value().Fingerprint(),
                                                          .definitions = {{.id = {entries[2].storedId.value},
                                                                           .authority = authority,
                                                                           .valueKind = ProgressionValueKind::SignedInteger64,
                                                                           .range = {-100, 100},
                                                                           .ordering = LeaderboardOrdering::LowestFirst,
                                                                           .localizationKey = "progress.board"}}});
        REQUIRE(boards.HasValue());
        return {std::make_shared<PlatformStableIdRegistry>(std::move(ids).Value()),
                std::make_shared<AchievementDefinitionRegistry>(std::move(achievements).Value()),
                std::make_shared<StatDefinitionRegistry>(std::move(stats).Value()),
                std::make_shared<LeaderboardDefinitionRegistry>(std::move(boards).Value())};
    }

    PlatformProgressionSessionScope Scope() {
        PlatformProgressionSessionScope scope{.provider = {1}, .session = {2}, .accessPolicy = {3}};
        scope.subjectPartition.digest.bytes.back() = 4;
        return scope;
    }

    ProgressionFact Fact(StatId id, std::int64_t value = 7) {
        ProgressionFact fact;
        fact.occurrence.bytes.back() = std::byte{1};
        fact.definition = id;
        fact.value = value;
        return fact;
    }

    PlatformProgressionReplayRequirements ReplayRequirementsForStats(const ProgressionDefinitions &definitions) {
        PlatformProgressionReplayRequirements requirements{.provider = {41}, .providerGeneration = Scope().provider, .policy = {1}};
        requirements.providerQualificationFingerprint.bytes.fill(1);
        requirements.effectiveSemanticsFingerprint.bytes.fill(2);
        requirements.projectPolicyFingerprint.bytes.fill(3);
        requirements.mappingGenerationFingerprint.bytes.fill(4);
        requirements.stableIdRegistryFingerprint = definitions.identities->Fingerprint();
        requirements.definitionRegistryFingerprint = definitions.stats->Fingerprint();
        return requirements;
    }
}  // namespace

static_assert(!std::is_copy_constructible_v<Horo::Application::ProgressionUseCases>);

TEST_CASE("Progression gameplay GUI CLI and server adapters share stable admission",
          "[platform-services][progression][pls004-contract][application]") {
    const auto definitions = Definitions();
    Authority authority;
    Gate gate;
    auto owner = PlatformProgressionCoalescer::Create();
    REQUIRE(owner.HasValue());
    auto created = ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value());
    REQUIRE(created.HasValue());
    auto useCases = std::move(created).Value();
    const auto fact = Fact(definitions.stats->Definitions().front().id);
    const auto gameplay = useCases.Submit(fact, 0);
    const auto gui = useCases.SubmitByKey(PlatformServiceIdKind::Stat, "progress.stat", fact, 0);
    const auto cli = useCases.SubmitByKey(PlatformServiceIdKind::Stat, "progress.stat", fact, 0);
    REQUIRE(gameplay.HasValue());
    REQUIRE(gui.HasValue());
    REQUIRE(cli.HasValue());
    CHECK(gameplay.Value().mutation == gui.Value().mutation);
    CHECK(gameplay.Value().mutation == cli.Value().mutation);
    CHECK(gui.Value().disposition == PlatformProgressionAdmissionDisposition::JoinedExisting);
    REQUIRE(useCases.Cancel(gameplay.Value().mutation).HasValue());
    REQUIRE(useCases.Query(gameplay.Value().mutation).has_value());
    CHECK(useCases.Query(gameplay.Value().mutation)->outcome == PlatformProgressionOutcome::Cancelled);
    CHECK_FALSE(owner.Value()->TakeReady(1).Value().has_value());
}

TEST_CASE("Progression rejects unknown IDs invalid values and Null before logical admission",
          "[platform-services][progression][pls004-contract][application]") {
    const auto definitions = Definitions();
    Authority authority;
    Gate gate;
    auto owner = PlatformProgressionCoalescer::Create();
    auto created = ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value());
    REQUIRE(created.HasValue());
    auto useCases = std::move(created).Value();
    CHECK(useCases.Submit(Fact({123}), 0).HasError());
    const auto id = definitions.stats->Definitions().front().id;
    CHECK(useCases.Submit(Fact(id, 101), 0).HasError());
    CHECK(useCases.SubmitByKey(PlatformServiceIdKind::Stat, "missing.stat", Fact(id), 0).HasError());
    CHECK(authority.calls == 0);
    CHECK(gate.calls == 0);
    gate.error = MakeError(BackendErrors::NullProvider);
    const auto rejected = useCases.Submit(Fact(id), 0);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code == gate.error->code);
    CHECK_FALSE(owner.Value()->TakeReady(1).Value().has_value());
}

TEST_CASE("Progression authority revocation and session rejection preserve structured failures",
          "[platform-services][progression][pls004-contract][application]") {
    const auto definitions = Definitions(ProgressionAuthorityMode::AuthorityServer);
    Authority authority;
    Gate gate;
    auto owner = PlatformProgressionCoalescer::Create();
    auto created = ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value());
    REQUIRE(created.HasValue());
    auto useCases = std::move(created).Value();
    const auto fact = Fact(definitions.stats->Definitions().front().id);
    CHECK(useCases.Submit(fact, 0).HasError());
    CHECK(gate.calls == 0);
    authority.server = true;
    REQUIRE(useCases.Submit(fact, 0).HasValue());
    authority.error = MakeError(PlatformProgressionErrors::Closed);
    const auto revoked = useCases.Submit(fact, 0);
    REQUIRE(revoked.HasError());
    CHECK(revoked.ErrorValue().code == authority.error->code);
    CHECK(gate.calls == 1);
    owner.Value()->Close();
    authority.error.reset();
    CHECK(useCases.Submit(fact, 0).HasError());
}

TEST_CASE("Progression achievement and leaderboard facts derive registered progress and ordering",
          "[platform-services][progression][pls004-contract][application]") {
    const auto definitions = Definitions();
    Authority authority;
    Gate gate;
    auto owner = PlatformProgressionCoalescer::Create();
    auto created = ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value());
    REQUIRE(created.HasValue());
    auto useCases = std::move(created).Value();
    ProgressionFact progress;
    progress.occurrence.bytes.back() = std::byte{1};
    progress.definition = definitions.achievements->Definitions().front().id;
    progress.value = std::uint64_t{101};
    CHECK(useCases.Submit(progress, 0).HasError());
    progress.value = std::int64_t{3};
    CHECK(useCases.Submit(progress, 0).HasError());
    progress.value = std::uint64_t{25};
    REQUIRE(useCases.Submit(progress, 0).HasValue());
    progress.occurrence.bytes.back() = std::byte{2};
    progress.value = std::uint64_t{10};
    REQUIRE(useCases.Submit(progress, 0).HasValue());
    const auto achievement = owner.Value()->TakeReady(1).Value().value();
    CHECK(achievement.envelope.kind == PlatformProgressionMutationKind::SetProgressMaximum);
    CHECK(std::get<std::uint64_t>(achievement.envelope.value) == 25);

    ProgressionFact score;
    score.definition = definitions.leaderboards->Definitions().front().id;
    score.occurrence.bytes.back() = std::byte{3};
    score.value = std::int64_t{50};
    REQUIRE(useCases.Submit(score, 1).HasValue());
    score.occurrence.bytes.back() = std::byte{4};
    score.value = std::int64_t{-5};
    REQUIRE(useCases.Submit(score, 1).HasValue());
    const auto board = owner.Value()->TakeReady(2).Value().value();
    CHECK(board.envelope.kind == PlatformProgressionMutationKind::SubmitBestScore);
    CHECK(std::get<std::int64_t>(board.envelope.value) == -5);
    REQUIRE(owner.Value()->Complete(board, PlatformProgressionOutcome::Succeeded).HasValue());

    score.expectedRevision = 8;
    score.occurrence.bytes.back() = std::byte{5};
    REQUIRE(useCases.Submit(score, 2).HasValue());
    const auto conditional = owner.Value()->TakeReady(3).Value().value();
    CHECK(conditional.envelope.kind == PlatformProgressionMutationKind::ReplaceScoreAtRevision);
    CHECK(conditional.envelope.expectedRevision == 8);
}

TEST_CASE("Progression real Null and scripted frontend profiles preserve availability and shutdown gates",
          "[platform-services][progression][pls004-contract][application][qualification]") {
    for (const bool nullProfile : {false, true}) {
        const auto definitions = Definitions();
        Authority authority;
        Gate semantics;
        auto mock = std::make_shared<TestSupport::MockPlatformServicesBackend>();
        std::shared_ptr<IPlatformServicesBackend> backend = mock;
        if (nullProfile)
            backend = std::make_shared<NullPlatformServicesBackend>();
        auto activated = ActivatePlatformServicesBackend(*backend, {});
        REQUIRE(activated.HasValue());
        auto session = BuildPlatformSessionSnapshot({});
        REQUIRE(session.HasValue());
        auto createdFrontend = PlatformServicesFrontend::Create(backend, activated.Value(), session.Value());
        REQUIRE(createdFrontend.HasValue());
        auto frontend = std::move(createdFrontend).Value();
        FrontendProgressionProjectionGate gate(frontend, semantics);
        auto owner = PlatformProgressionCoalescer::Create();
        REQUIRE(owner.HasValue());
        auto created = ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value());
        REQUIRE(created.HasValue());
        auto useCases = std::move(created).Value();
        auto fact = Fact(definitions.stats->Definitions().front().id);
        const auto admitted = useCases.Submit(fact, 0);
        if (nullProfile) {
            REQUIRE(admitted.HasError());
            CHECK(admitted.ErrorValue().code == BackendErrors::NullProvider.code);
            CHECK(semantics.calls == 0);
            CHECK_FALSE(owner.Value()->TakeReady(1).Value().has_value());
        } else {
            REQUIRE(admitted.HasValue());
            CHECK(semantics.calls == 1);
            // Available service metadata cannot stand in for qualified atomic progression semantics.
            semantics.error = MakeError(BackendErrors::UnsupportedOperation);
            fact.occurrence.bytes.back() = std::byte{2};
            const auto unsupported = useCases.Submit(fact, 0);
            REQUIRE(unsupported.HasError());
            CHECK(unsupported.ErrorValue().code == semantics.error->code);
        }
        CHECK(mock->Calls().empty());
        const auto checksBeforeClose = semantics.calls;
        REQUIRE(frontend.Close().HasValue());
        const auto closed = useCases.Submit(fact, 1);
        REQUIRE(closed.HasError());
        CHECK(closed.ErrorValue().code == FrontendErrors::Unavailable.code);
        CHECK(semantics.calls == checksBeforeClose);
        CHECK(mock->Calls().empty());
    }
}

TEST_CASE("Progression application receipts expose atomic failures and isolate captured subject scopes",
          "[platform-services][progression][pls004-contract][application][qualification]") {
    const auto definitions = Definitions();
    Authority authority;
    Gate gate;
    auto owner = PlatformProgressionCoalescer::Create();
    REQUIRE(owner.HasValue());
    auto created = ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value());
    REQUIRE(created.HasValue());
    auto useCases = std::move(created).Value();
    auto otherScope = Scope();
    ++otherScope.session.value;
    auto other = ProgressionUseCases::Create(definitions, otherScope, {1}, authority, gate, *owner.Value());
    REQUIRE(other.HasValue());
    auto otherUseCases = std::move(other).Value();
    const auto admitted = useCases.Submit(Fact(definitions.stats->Definitions().front().id), 0);
    REQUIRE(admitted.HasValue());
    CHECK_FALSE(otherUseCases.Query(admitted.Value().mutation).has_value());
    CHECK(otherUseCases.Cancel(admitted.Value().mutation).HasError());
    const auto dispatch = owner.Value()->TakeReady(1).Value();
    REQUIRE(dispatch.has_value());
    REQUIRE(owner.Value()
                ->Complete(*dispatch, PlatformProgressionOutcome::Failed, PlatformProviderFailureCategory::PermanentFailure)
                .HasValue());
    const auto observed = useCases.Query(admitted.Value().mutation);
    REQUIRE(observed.has_value());
    CHECK(observed->outcome == PlatformProgressionOutcome::Failed);
    CHECK(observed->failure == PlatformProviderFailureCategory::PermanentFailure);
    CHECK_FALSE(otherUseCases.Query(admitted.Value().mutation).has_value());
}

TEST_CASE("Progression durable preparation preserves exact evidence and never falls back to volatile admission",
          "[platform-services][progression][pls004-contract][application][replay]") {
    const auto definitions = Definitions();
    Authority authority;
    Gate gate;
    ReplayQualifier qualifier;
    auto owner = PlatformProgressionCoalescer::Create();
    REQUIRE(owner.HasValue());
    auto created = ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value());
    REQUIRE(created.HasValue());
    auto useCases = std::move(created).Value();
    const auto fact = Fact(definitions.stats->Definitions().front().id);
    auto requirements = ReplayRequirementsForStats(definitions);
    auto qualified = useCases.PrepareDurableIntent(fact, requirements, qualifier);
    REQUIRE(qualified.HasValue());
    CHECK(qualified.Value().Requirements() == requirements);
    CHECK(qualified.Value().Envelope().scope == Scope());
    CHECK(qualified.Value().Envelope().value == fact.value);
    CHECK_FALSE(useCases.Query(qualified.Value().Envelope().mutation).has_value());
    CHECK_FALSE(owner.Value()->TakeReady(1).Value().has_value());
    const auto repeated = useCases.PrepareDurableIntent(fact, requirements, qualifier);
    REQUIRE(repeated.HasValue());
    CHECK(repeated.Value().Envelope() == qualified.Value().Envelope());

    qualifier.failure = MakeError(BackendErrors::UnsupportedOperation);
    const auto unsupported = useCases.PrepareDurableIntent(fact, requirements, qualifier);
    REQUIRE(unsupported.HasError());
    CHECK(unsupported.ErrorValue().code == qualifier.failure->code);
    CHECK_FALSE(owner.Value()->TakeReady(1).Value().has_value());
    qualifier.failure.reset();
    qualifier.corruptEnvelope = true;
    CHECK(useCases.PrepareDurableIntent(fact, requirements, qualifier).HasError());
    CHECK_FALSE(owner.Value()->TakeReady(1).Value().has_value());
    const auto previousCalls = qualifier.calls;
    requirements.stableIdRegistryFingerprint.bytes.fill(9);
    CHECK(useCases.PrepareDurableIntent(fact, requirements, qualifier).HasError());
    CHECK(qualifier.calls == previousCalls);
}

TEST_CASE("Progression committed occurrence replay cannot allocate another intent for a changed payload",
          "[platform-services][progression][pls004-contract][application][idempotency]") {
    const auto definitions = Definitions();
    Authority authority;
    Gate gate;
    auto owner = PlatformProgressionCoalescer::Create();
    REQUIRE(owner.HasValue());
    auto created = ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value());
    REQUIRE(created.HasValue());
    auto useCases = std::move(created).Value();
    const auto original = Fact(definitions.stats->Definitions().front().id);
    const auto admitted = useCases.Submit(original, 0);
    REQUIRE(admitted.HasValue());
    auto changed = original;
    changed.value = std::int64_t{29};
    const auto conflict = useCases.Submit(changed, 0);
    REQUIRE(conflict.HasError());
    CHECK(conflict.ErrorValue().code == PlatformProgressionErrors::IdempotencyConflict.code);
    const auto dispatch = owner.Value()->TakeReady(1).Value();
    REQUIRE(dispatch.has_value());
    CHECK(dispatch->envelope.value == original.value);
    REQUIRE(owner.Value()->Complete(*dispatch, PlatformProgressionOutcome::Succeeded).HasValue());
    const auto repeated = useCases.Submit(original, 1);
    REQUIRE(repeated.HasValue());
    CHECK(repeated.Value().mutation == admitted.Value().mutation);
    CHECK(repeated.Value().disposition == PlatformProgressionAdmissionDisposition::JoinedExisting);
    CHECK(useCases.Submit(changed, 1).HasError());
    CHECK_FALSE(owner.Value()->TakeReady(1).Value().has_value());
    ProgressionFact anotherProjection = original;
    anotherProjection.definition = definitions.achievements->Definitions().front().id;
    anotherProjection.value = std::uint64_t{3};
    REQUIRE(useCases.Submit(anotherProjection, 1).HasValue());
}

TEST_CASE("Progression selected durable owner and evidence survive failed preparation without volatile fallback",
          "[platform-services][progression][pls004-contract][application][replay]") {
    const auto definitions = Definitions();
    Authority authority;
    Gate gate;
    ReplayQualifier qualifier;
    auto owner = PlatformProgressionCoalescer::Create();
    REQUIRE(owner.HasValue());
    auto created = ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value());
    REQUIRE(created.HasValue());
    auto useCases = std::move(created).Value();
    auto requirements = ReplayRequirementsForStats(definitions);
    const auto fact = Fact(definitions.stats->Definitions().front().id);
    qualifier.failure = MakeError(BackendErrors::UnsupportedOperation);
    REQUIRE(useCases.PrepareDurableIntent(fact, requirements, qualifier).HasError());
    const auto fallback = useCases.Submit(fact, 0);
    REQUIRE(fallback.HasError());
    CHECK(fallback.ErrorValue().code == PlatformProgressionErrors::IdempotencyConflict.code);
    qualifier.failure.reset();
    REQUIRE(useCases.PrepareDurableIntent(fact, requirements, qualifier).HasValue());
    auto changed = fact;
    changed.value = std::int64_t{9};
    CHECK(useCases.PrepareDurableIntent(changed, requirements, qualifier).HasError());
    const auto previousCalls = qualifier.calls;
    requirements.projectPolicyFingerprint.bytes.fill(9);
    CHECK(useCases.PrepareDurableIntent(fact, requirements, qualifier).HasError());
    CHECK(qualifier.calls == previousCalls);
    CHECK_FALSE(owner.Value()->TakeReady(1).Value().has_value());
}

TEST_CASE("Progression semantic fact retention remains bounded and move preserves the original authority",
          "[platform-services][progression][pls004-contract][application][lifetime]") {
    const auto definitions = Definitions();
    Authority authority;
    Gate gate;
    auto owner = PlatformProgressionCoalescer::Create();
    REQUIRE(owner.HasValue());
    CHECK(ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value(), {0}).HasError());
    CHECK(ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value(),
                                      {PlatformProgressionMaximumInFlightMutations + 1})
              .HasError());
    auto created = ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value(), {1});
    REQUIRE(created.HasValue());
    auto useCases = std::move(created).Value();
    const auto first = Fact(definitions.stats->Definitions().front().id);
    REQUIRE(useCases.Submit(first, 0).HasValue());
    auto second = first;
    second.occurrence.bytes.back() = std::byte{2};
    CHECK(useCases.Submit(second, 0).HasError());
    auto moved = [&useCases] {
        const Horo::Tests::AllocationProbe::ScopedFailure fail;
        return std::move(useCases);
    }();
    CHECK(useCases.Submit(first, 0).HasError());
    CHECK(useCases.SubmitByKey(PlatformServiceIdKind::Stat, "progress.stat", first, 0).HasError());
    const auto original = moved.Submit(first, 0);
    REQUIRE(original.HasValue());
    CHECK(original.Value().disposition == PlatformProgressionAdmissionDisposition::JoinedExisting);
    auto conflicting = first;
    conflicting.value = std::int64_t{9};
    CHECK(moved.Submit(conflicting, 0).HasError());
    CHECK(moved.Submit(second, 0).HasError());
}

TEST_CASE("Progression fact ledger allocation failure leaves delivery owners untouched",
          "[platform-services][progression][pls004-contract][application][lifetime]") {
    const auto definitions = Definitions();
    Authority authority;
    Gate gate;
    auto owner = PlatformProgressionCoalescer::Create();
    REQUIRE(owner.HasValue());
    const Horo::Tests::AllocationProbe::ScopedFailure fail;
    const auto created = ProgressionUseCases::Create(definitions, Scope(), {1}, authority, gate, *owner.Value());
    REQUIRE(created.HasError());
    CHECK(created.ErrorValue().code == PlatformProgressionErrors::CapacityExceeded.code);
    CHECK(authority.calls == 0);
    CHECK(gate.calls == 0);
    CHECK_FALSE(owner.Value()->TakeReady(1).Value().has_value());
}
