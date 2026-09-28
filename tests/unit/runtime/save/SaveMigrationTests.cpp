#include "Horo/Runtime/Save/SaveDiagnostics.h"
#include "Horo/Runtime/Save/SaveMigration.h"
#include "SaveTestUtils.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Test;

    [[nodiscard]] SaveParticipantId Participant(const char *value) {
        return SaveParticipantId::Parse(value).Value();
    }

    [[nodiscard]] std::vector<std::byte> Bytes(const std::string_view value) {
        std::vector<std::byte> result;
        result.reserve(value.size());
        for (const char character : value)
            result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
        return result;
    }

    [[nodiscard]] SaveMigrationParticipantState ParticipantState(const char *id, const std::uint32_t version, const bool required,
                                                                 const std::string_view payload = {}) {
        return {.participant = Participant(id),
                .schemaVersion = V<ParticipantSchemaVersion>(version),
                .required = required,
                .payload = Bytes(payload)};
    }

    [[nodiscard]] SaveMigrationSource Source(const std::uint32_t archiveVersion = 1, const std::uint32_t schemaVersion = 1,
                                             const std::uint32_t productVersion = 1) {
        return {.archiveFormatVersion = V<ArchiveFormatVersion>(archiveVersion),
                .saveSchemaVersion = V<SaveSchemaVersion>(schemaVersion),
                .productCompatibility = V<ProductSaveCompatibilityVersion>(productVersion),
                .participants = {ParticipantState("horo.scene.core.v1", 1, true, "scene"),
                                 ParticipantState("project.gameplay.v1", 1, false, "gameplay")},
                .archiveBytes = Bytes("original")};
    }

    void AddRecord(SaveMigrationSource &source, const std::size_t participantIndex, const std::uint8_t suffix) {
        const auto owner = source.participants[participantIndex].participant;
        source.participants[participantIndex].records.push_back({.record = Id<SaveRecordId>(suffix),
                                                                 .schemaVersion = V<ParticipantSchemaVersion>(1),
                                                                 .sourceParticipant = owner,
                                                                 .sourceRecord = Id<SaveRecordId>(suffix),
                                                                 .sourceSchemaVersion = V<ParticipantSchemaVersion>(1),
                                                                 .payload = Bytes("old")});
    }

    template <typename Tag>
    [[nodiscard]] SaveVersionSupport<Tag> DirectAndMigratable(const std::uint32_t direct, const std::uint32_t migration) {
        SaveVersionSupport<Tag> support{.direct = {.minimum = V<SaveVersion<Tag>>(direct), .maximum = V<SaveVersion<Tag>>(direct)}};
        if (direct != migration)
            support.migrationSource =
                SaveVersionRange<Tag>{.minimum = V<SaveVersion<Tag>>(migration), .maximum = V<SaveVersion<Tag>>(migration)};
        return support;
    }

    [[nodiscard]] SaveMigrationSupportDescriptor Support(const std::uint32_t archiveTarget = 3, const std::uint32_t archiveSource = 1,
                                                         const std::uint32_t participantTarget = 2,
                                                         const std::uint32_t participantSource = 1) {
        return {.compatibility =
                    {.archiveVersions = DirectAndMigratable<ArchiveFormatVersionTag>(archiveTarget, archiveSource),
                     .saveSchemaVersions = {.direct = {.minimum = V<SaveSchemaVersion>(1), .maximum = V<SaveSchemaVersion>(1)}},
                     .productVersions = {.direct = {.minimum = V<ProductSaveCompatibilityVersion>(1),
                                                    .maximum = V<ProductSaveCompatibilityVersion>(1)}},
                     .participants = {{.participant = Participant("horo.scene.core.v1"),
                                       .versions = DirectAndMigratable<ParticipantSchemaVersionTag>(participantTarget, participantSource),
                                       .required = true},
                                      {.participant = Participant("project.gameplay.v1"),
                                       .versions = {.direct = {.minimum = V<ParticipantSchemaVersion>(1),
                                                               .maximum = V<ParticipantSchemaVersion>(1)}},
                                       .required = false}},
                     .supportedFeatureFlagsMask = 0},
                .checkpoints = {}};
    }

    [[nodiscard]] SaveMigrationFn BumpArchive(const std::uint32_t target, const std::string_view suffix = {}) {
        return [target, suffix](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            candidate.archiveFormatVersion = V<ArchiveFormatVersion>(target);
            const auto bytes = Bytes(suffix);
            candidate.archiveBytes.insert(candidate.archiveBytes.end(), bytes.begin(), bytes.end());
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        };
    }

    [[nodiscard]] SaveMigrationFn BumpParticipant(const char *participant, const std::uint32_t target, const std::string_view suffix = {}) {
        return
            [participant = std::string{participant}, target, suffix](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            auto found =
                std::ranges::find(candidate.participants, Participant(participant.c_str()), &SaveMigrationParticipantState::participant);
            if (found == candidate.participants.end())
                return Result<SaveMigrationCandidate>::Failure(MakeError(SaveErrors::MigrationCandidateInvalid));
            found->schemaVersion = V<ParticipantSchemaVersion>(target);
            const auto bytes = Bytes(suffix);
            found->payload.insert(found->payload.end(), bytes.begin(), bytes.end());
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        };
    }

    [[nodiscard]] ArchiveMigrationStep ArchiveStep(const char *id, const std::uint32_t from, const std::uint32_t to,
                                                   const SaveMigrationStepKind kind = SaveMigrationStepKind::Sequential,
                                                   std::vector<SaveMigrationId> equivalence = {}) {
        return {.id = SaveMigrationId{.value = id},
                .kind = kind,
                .from = V<ArchiveFormatVersion>(from),
                .to = V<ArchiveFormatVersion>(to),
                .migrate = BumpArchive(to, id),
                .equivalentSequentialSteps = std::move(equivalence),
                .estimatedWork = 1};
    }

    [[nodiscard]] ParticipantMigrationStep ParticipantStep(const char *id, const char *participant, const std::uint32_t from,
                                                           const std::uint32_t to) {
        return {.id = SaveMigrationId{.value = id},
                .participant = Participant(participant),
                .kind = SaveMigrationStepKind::Sequential,
                .from = V<ParticipantSchemaVersion>(from),
                .to = V<ParticipantSchemaVersion>(to),
                .migrate = BumpParticipant(participant, to, id),
                .equivalentSequentialSteps = {},
                .estimatedWork = 1};
    }

    TEST_CASE("Save migration snapshots are deterministic and freeze later registry changes", "[runtime][save][migration]") {
        const std::vector<SaveMigrationDefinition> definitions{ArchiveStep("archive.1_to_2", 1, 2), ArchiveStep("archive.2_to_3", 2, 3)};
        auto registryResult = SaveMigrationRegistry::Create(definitions);
        REQUIRE(registryResult.HasValue());
        auto registry = std::move(registryResult).Value();
        auto first = registry.Snapshot();
        REQUIRE(first.HasValue());
        const auto firstIdentity = first.Value().CatalogIdentity();
        const auto firstGeneration = first.Value().Generation();

        auto registration = registry.Register(ArchiveStep("archive.3_to_4", 3, 4));
        REQUIRE(registration.HasValue());
        auto second = registry.Snapshot();
        REQUIRE(second.HasValue());
        CHECK(first.Value().Definitions().size() == 2);
        CHECK(second.Value().Definitions().size() == 3);
        CHECK(first.Value().Generation() == firstGeneration);
        CHECK(second.Value().Generation() > firstGeneration);
        CHECK(first.Value().CatalogIdentity() != second.Value().CatalogIdentity());
        CHECK(firstIdentity == first.Value().CatalogIdentity());

        registry.Close();
        auto closedRegistration = registry.Register(ArchiveStep("archive.4_to_5", 4, 5));
        REQUIRE(closedRegistration.HasError());
        CHECK(closedRegistration.ErrorValue().code.Value() == "save.migration.registry_closed");
    }

    TEST_CASE("Planner selects sequential routes and an explicitly equivalent checkpoint", "[runtime][save][migration]") {
        const auto sequentialOne = ArchiveStep("archive.1_to_2", 1, 2);
        const auto sequentialTwo = ArchiveStep("archive.2_to_3", 2, 3);
        const auto checkpoint = ArchiveStep("archive.1_to_3", 1, 3, SaveMigrationStepKind::Checkpoint,
                                            {SaveMigrationId{.value = "archive.1_to_2"}, SaveMigrationId{.value = "archive.2_to_3"}});
        const auto participant = ParticipantStep("scene.1_to_2", "horo.scene.core.v1", 1, 2);
        const std::vector<SaveMigrationDefinition> definitions{sequentialOne, sequentialTwo, checkpoint, participant};
        auto registry = SaveMigrationRegistry::Create(definitions);
        REQUIRE(registry.HasValue());
        auto snapshot = registry.Value().Snapshot();
        REQUIRE(snapshot.HasValue());

        auto support = Support();
        support.checkpoints.push_back(
            {.id = SaveMigrationId{.value = "archive.1_to_3"},
             .axis = SaveMigrationAxis::ArchiveFormat,
             .participant = std::nullopt,
             .equivalentSequentialSteps = {SaveMigrationId{.value = "archive.1_to_2"}, SaveMigrationId{.value = "archive.2_to_3"}}});
        const auto plan = snapshot.Value().Plan(Source(), support);
        REQUIRE(plan.HasValue());
        REQUIRE(plan.Value().definitions.size() == 2);
        CHECK(std::get<ArchiveMigrationStep>(plan.Value().definitions[0]).kind == SaveMigrationStepKind::Checkpoint);
        CHECK(std::get<ParticipantMigrationStep>(plan.Value().definitions[1]).participant == Participant("horo.scene.core.v1"));
        CHECK(plan.Value().targetArchiveFormat == V<ArchiveFormatVersion>(3));
        CHECK(plan.Value().participantTargets.front().schemaVersion == V<ParticipantSchemaVersion>(2));
        CHECK_FALSE(plan.Value().IsNoOp());

        auto sequentialSupport = Support();
        auto sequentialRegistry =
            SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{sequentialOne, sequentialTwo, participant});
        REQUIRE(sequentialRegistry.HasValue());
        auto sequentialSnapshot = sequentialRegistry.Value().Snapshot();
        REQUIRE(sequentialSnapshot.HasValue());
        const auto sequentialPlan = sequentialSnapshot.Value().Plan(Source(), sequentialSupport);
        REQUIRE(sequentialPlan.HasValue());
        REQUIRE(sequentialPlan.Value().definitions.size() == 3);
        CHECK(std::get<ArchiveMigrationStep>(sequentialPlan.Value().definitions[0]).id.value == "archive.1_to_2");
        CHECK(std::get<ArchiveMigrationStep>(sequentialPlan.Value().definitions[1]).id.value == "archive.2_to_3");
    }

    TEST_CASE("Planner rejects gaps, ambiguous routes, cycles, and newer input", "[runtime][save][migration]") {
        const auto gapRegistry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{ArchiveStep("archive.1_to_2", 1, 2)});
        REQUIRE(gapRegistry.HasValue());
        auto gapSnapshot = gapRegistry.Value().Snapshot();
        REQUIRE(gapSnapshot.HasValue());
        auto gap = gapSnapshot.Value().Plan(Source(), Support());
        REQUIRE(gap.HasError());
        CHECK(gap.ErrorValue().code.Value() == "save.migration.path_missing");

        const auto ambiguousRegistry = SaveMigrationRegistry::Create(
            std::vector<SaveMigrationDefinition>{ArchiveStep("archive.1_to_2", 1, 2), ArchiveStep("archive.1_to_3", 1, 3),
                                                 ArchiveStep("archive.2_to_3", 2, 3)});
        REQUIRE(ambiguousRegistry.HasValue());
        auto ambiguousSnapshot = ambiguousRegistry.Value().Snapshot();
        REQUIRE(ambiguousSnapshot.HasValue());
        auto ambiguous = ambiguousSnapshot.Value().Plan(Source(), Support());
        REQUIRE(ambiguous.HasError());
        CHECK(ambiguous.ErrorValue().code.Value() == "save.migration.ambiguous");

        const auto cycle = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{ArchiveStep("archive.2_to_1", 2, 1)});
        REQUIRE(cycle.HasError());
        CHECK(cycle.ErrorValue().code.Value() == "save.migration.backward_edge");

        auto newerSource = Source(4);
        auto validRegistry = SaveMigrationRegistry::Create(
            std::vector<SaveMigrationDefinition>{ArchiveStep("archive.1_to_2", 1, 2), ArchiveStep("archive.2_to_3", 2, 3)});
        REQUIRE(validRegistry.HasValue());
        auto validSnapshot = validRegistry.Value().Snapshot();
        REQUIRE(validSnapshot.HasValue());
        const auto newer = validSnapshot.Value().Plan(newerSource, Support());
        REQUIRE(newer.HasError());
        CHECK(newer.ErrorValue().code.Value() == "save.migration.unsupported_newer");
    }

    TEST_CASE("Registry rejects duplicate typed edges even when identities differ", "[runtime][save][migration]") {
        const auto first = ArchiveStep("archive.first", 1, 2);
        const auto duplicate = ArchiveStep("archive.second", 1, 2);
        const auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{first, duplicate});
        REQUIRE(registry.HasError());
        CHECK(registry.ErrorValue().code.Value() == "save.migration.duplicate_edge");
    }

    TEST_CASE("Planner rejects a checkpoint whose equivalence route is not exact", "[runtime][save][migration]") {
        const auto first = ArchiveStep("archive.1_to_2", 1, 2);
        const auto second = ArchiveStep("archive.2_to_3", 2, 3);
        const auto checkpoint =
            ArchiveStep("archive.1_to_3", 1, 3, SaveMigrationStepKind::Checkpoint, {SaveMigrationId{.value = "archive.1_to_2"}});
        auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{first, second, checkpoint});
        REQUIRE(registry.HasValue());
        auto snapshot = registry.Value().Snapshot();
        REQUIRE(snapshot.HasValue());
        auto support = Support();
        support.checkpoints.push_back({.id = SaveMigrationId{.value = "archive.1_to_3"},
                                       .axis = SaveMigrationAxis::ArchiveFormat,
                                       .participant = std::nullopt,
                                       .equivalentSequentialSteps = {SaveMigrationId{.value = "archive.1_to_2"}}});
        const auto result = snapshot.Value().Plan(Source(), support);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "save.migration.checkpoint_not_equivalent");
    }

    TEST_CASE("Executor creates a detached candidate and preserves the source archive", "[runtime][save][migration]") {
        const auto archive = ArchiveStep("archive.1_to_2", 1, 2);
        const auto participant = ParticipantStep("scene.1_to_2", "horo.scene.core.v1", 1, 2);
        auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{archive, participant});
        REQUIRE(registry.HasValue());
        auto snapshot = registry.Value().Snapshot();
        REQUIRE(snapshot.HasValue());
        const auto source = Source();
        auto plan = snapshot.Value().Plan(source, Support(2, 1));
        REQUIRE(plan.HasValue());

        const auto migrated = SaveMigrationExecutor::Migrate(source, plan.Value());
        REQUIRE(migrated.HasValue());
        CHECK(source.archiveFormatVersion == V<ArchiveFormatVersion>(1));
        CHECK(source.participants.front().schemaVersion == V<ParticipantSchemaVersion>(1));
        CHECK(source.archiveBytes == Bytes("original"));
        CHECK(migrated.Value().archiveFormatVersion == V<ArchiveFormatVersion>(2));
        CHECK(migrated.Value().participants.front().schemaVersion == V<ParticipantSchemaVersion>(2));
        CHECK(migrated.Value().archiveBytes != source.archiveBytes);
        CHECK(migrated.Value().participants.front().payload != source.participants.front().payload);

        auto tamperedPlan = plan.Value();
        tamperedPlan.targetArchiveFormat = V<ArchiveFormatVersion>(3);
        const auto tampered = SaveMigrationExecutor::Migrate(source, tamperedPlan);
        REQUIRE(tampered.HasError());
        CHECK(tampered.ErrorValue().code.Value() == "save.migration.plan_invalid");
    }

    TEST_CASE("Migration steps consume one cumulative work budget", "[runtime][save][migration]") {
        bool sawBudgetContext = false;
        auto first = ArchiveStep("archive.1_to_2", 1, 2);
        first.migrate = [&sawBudgetContext](SaveMigrationCandidate candidate, const SaveMigrationStepContext &context) {
            sawBudgetContext = context.remainingWorkBytes != 0 && context.maximumArchiveBytes != 0 &&
                               context.maximumParticipantPayloadBytes != 0 && context.maximumTotalPayloadBytes != 0;
            return BumpArchive(2, "archive.1_to_2")(std::move(candidate), context);
        };
        auto registry =
            SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{std::move(first), ArchiveStep("archive.2_to_3", 2, 3)});
        REQUIRE(registry.HasValue());
        auto snapshot = registry.Value().Snapshot();
        REQUIRE(snapshot.HasValue());
        auto plan = snapshot.Value().Plan(Source(), Support(3, 1, 1, 1));
        REQUIRE(plan.HasValue());
        SaveMigrationLimits limits;
        limits.maximumCumulativeWorkBytes = 200;
        CHECK(SaveMigrationExecutor::Migrate(Source(), plan.Value(), limits).HasValue());
        CHECK(sawBudgetContext);
        limits.maximumCumulativeWorkBytes = 100;
        const auto exhausted = SaveMigrationExecutor::Migrate(Source(), plan.Value(), limits);
        REQUIRE(exhausted.HasError());
        CHECK(exhausted.ErrorValue().code.Value() == SaveErrors::MigrationLimitExceeded.code.Value());
        REQUIRE(exhausted.ErrorValue().diagnostics.size() == 1);
        CHECK(exhausted.ErrorValue().diagnostics.front().code.Value() == "save.migration.limit.cumulative_work");
        limits.maximumCumulativeWorkBytes = std::numeric_limits<std::uint64_t>::max();
        CHECK(SaveMigrationExecutor::Migrate(Source(), plan.Value(), limits).HasError());
        limits = {};
        limits.maximumPlanSteps = MaximumSaveMigrationPlanSteps + 1;
        CHECK(SaveMigrationExecutor::Migrate(Source(), plan.Value(), limits).HasError());
    }

    TEST_CASE("Executor rejects participant steps that modify unrelated state", "[runtime][save][migration]") {
        auto invalidParticipant = ParticipantStep("scene.1_to_2", "horo.scene.core.v1", 1, 2);
        invalidParticipant.migrate = [](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            candidate.participants.back().payload.push_back(std::byte{0x7f});
            candidate.participants.front().schemaVersion = V<ParticipantSchemaVersion>(2);
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        };
        auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{invalidParticipant});
        REQUIRE(registry.HasValue());
        auto snapshot = registry.Value().Snapshot();
        REQUIRE(snapshot.HasValue());
        auto support = Support(1, 1, 2, 1);
        const auto plan = snapshot.Value().Plan(Source(), support);
        REQUIRE(plan.HasValue());
        const auto result = SaveMigrationExecutor::Migrate(Source(), plan.Value());
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "save.migration.candidate_invalid");
    }

    TEST_CASE("Executor rejects participant steps that modify archive bytes", "[runtime][save][migration]") {
        auto invalidParticipant = ParticipantStep("scene.1_to_2", "horo.scene.core.v1", 1, 2);
        invalidParticipant.migrate = [](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            candidate.archiveBytes.push_back(std::byte{0x7f});
            candidate.participants.front().schemaVersion = V<ParticipantSchemaVersion>(2);
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        };
        auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{invalidParticipant});
        REQUIRE(registry.HasValue());
        auto snapshot = registry.Value().Snapshot();
        REQUIRE(snapshot.HasValue());
        const auto plan = snapshot.Value().Plan(Source(), Support(1, 1, 2, 1));
        REQUIRE(plan.HasValue());
        const auto result = SaveMigrationExecutor::Migrate(Source(), plan.Value());
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "save.migration.candidate_invalid");
    }

    TEST_CASE("Participant record migration advances each owned record and preserves provenance", "[runtime][save][migration]") {
        auto source = Source(1);
        for (std::uint8_t suffix : {std::uint8_t{31}, std::uint8_t{32}})
            AddRecord(source, 0, suffix);
        auto step = ParticipantStep("scene.records.1_to_2", "horo.scene.core.v1", 1, 2);
        step.migrate = {};
        step.migrateRecord = [](const std::span<const std::byte> input, const SaveMigrationRecordContext &context) {
            CHECK(context.from == V<ParticipantSchemaVersion>(1));
            CHECK(context.to == V<ParticipantSchemaVersion>(2));
            CHECK(context.participant == Participant("horo.scene.core.v1"));
            auto output = std::vector<std::byte>{input.begin(), input.end()};
            output.push_back(std::byte{0x42});
            return Result<std::vector<std::byte>>::Success(std::move(output));
        };
        auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{step});
        REQUIRE(registry.HasValue());
        auto snapshot = registry.Value().Snapshot();
        REQUIRE(snapshot.HasValue());
        const auto plan = snapshot.Value().Plan(source, Support(1, 1, 2, 1));
        REQUIRE(plan.HasValue());
        const auto migrated = SaveMigrationExecutor::Migrate(source, plan.Value());
        REQUIRE(migrated.HasValue());
        CHECK(source.participants.front().records.front().payload == Bytes("old"));
        CHECK(migrated.Value().participants.front().schemaVersion == V<ParticipantSchemaVersion>(2));
        for (std::size_t index = 0; index < 2; ++index) {
            const auto &before = source.participants.front().records[index];
            const auto &after = migrated.Value().participants.front().records[index];
            CHECK(after.record == before.record);
            CHECK(after.sourceRecord == before.sourceRecord);
            CHECK(after.sourceParticipant == before.sourceParticipant);
            CHECK(after.sourceSchemaVersion == before.sourceSchemaVersion);
            CHECK(after.schemaVersion == V<ParticipantSchemaVersion>(2));
            CHECK(after.payload.size() == 4);
        }
    }

    TEST_CASE("Participant records require an explicit other-owner transform contract", "[runtime][save][migration]") {
        auto step = ParticipantStep("scene.cross.1_to_2", "horo.scene.core.v1", 1, 2);
        step.migrate = [](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            candidate.participants.front().schemaVersion = V<ParticipantSchemaVersion>(2);
            if (candidate.participants.size() > 1)
                candidate.participants.back().records.front().payload.push_back(std::byte{0x42});
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        };
        auto source = Source(1);
        AddRecord(source, 1, 41);
        auto migrate = [&source](const ParticipantMigrationStep &definition) {
            auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{definition});
            REQUIRE(registry.HasValue());
            auto snapshot = registry.Value().Snapshot();
            REQUIRE(snapshot.HasValue());
            const auto plan = snapshot.Value().Plan(source, Support(1, 1, 2, 1));
            REQUIRE(plan.HasValue());
            return SaveMigrationExecutor::Migrate(source, plan.Value());
        };
        const auto forbidden = migrate(step);
        REQUIRE(forbidden.HasError());
        CHECK(forbidden.ErrorValue().message.find("project.gameplay.v1") != std::string::npos);
        step.crossParticipantTransforms = {
            {.target = Participant("project.gameplay.v1"), .targetSchemaVersion = V<ParticipantSchemaVersion>(1)}};
        const auto permitted = migrate(step);
        REQUIRE(permitted.HasValue());
        CHECK(permitted.Value().participants.back().records.front().payload.size() ==
              source.participants.back().records.front().payload.size() + 1);
        CHECK(source.participants.back().records.front().payload == Bytes("old"));
        step.crossParticipantTransforms.front().targetSchemaVersion = V<ParticipantSchemaVersion>(2);
        CHECK(migrate(step).HasError());
        source.participants.pop_back();
        CHECK(migrate(step).HasValue());
    }

    TEST_CASE("Record failure reports field context and rolls back detached migration", "[runtime][save][migration]") {
        auto source = Source(1);
        AddRecord(source, 0, 31);
        auto step = ParticipantStep("scene.fail.1_to_2", "horo.scene.core.v1", 1, 2);
        step.migrate = {};
        step.migrateRecord = [](std::span<const std::byte>, const SaveMigrationRecordContext &context) {
            return Result<std::vector<std::byte>>::Failure(
                context.Fail(MakeError(SaveErrors::MigrationCandidateInvalid, "invalid value"), "transform.position.x"));
        };
        auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{step});
        REQUIRE(registry.HasValue());
        auto snapshot = registry.Value().Snapshot();
        REQUIRE(snapshot.HasValue());
        const auto plan = snapshot.Value().Plan(source, Support(1, 1, 2, 1));
        REQUIRE(plan.HasValue());
        const auto before = source;
        const auto result = SaveMigrationExecutor::Migrate(source, plan.Value());
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().cause.Get() != nullptr);
        CHECK(result.ErrorValue().message.find("scene.fail.1_to_2") != std::string::npos);
        CHECK(result.ErrorValue().message.find("field=transform.position.x") != std::string::npos);
        CHECK(result.ErrorValue().cause.Get()->cause.Get()->message.find("field=transform.position.x") != std::string::npos);
        CHECK(source == before);
    }

    TEST_CASE("Record identity and provenance cannot be rewritten by an owner callback", "[runtime][save][migration]") {
        auto source = Source(1);
        AddRecord(source, 0, 31);
        auto step = ParticipantStep("scene.provenance.1_to_2", "horo.scene.core.v1", 1, 2);
        step.migrate = [](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            candidate.participants.front().schemaVersion = V<ParticipantSchemaVersion>(2);
            candidate.participants.front().records.front().schemaVersion = V<ParticipantSchemaVersion>(2);
            candidate.participants.front().records.front().sourceSchemaVersion = V<ParticipantSchemaVersion>(2);
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        };
        auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{step});
        REQUIRE(registry.HasValue());
        auto snapshot = registry.Value().Snapshot();
        REQUIRE(snapshot.HasValue());
        const auto plan = snapshot.Value().Plan(source, Support(1, 1, 2, 1));
        REQUIRE(plan.HasValue());
        const auto migrated = SaveMigrationExecutor::Migrate(source, plan.Value());
        REQUIRE(migrated.HasError());
        CHECK(migrated.ErrorValue().code.Value() == SaveErrors::MigrationCandidateInvalid.code.Value());
        CHECK(source.participants.front().records.front().sourceSchemaVersion == V<ParticipantSchemaVersion>(1));
    }

    TEST_CASE("Cross-participant transform contracts reject duplicate or own targets", "[runtime][save][migration]") {
        auto step = ParticipantStep("scene.contract.1_to_2", "horo.scene.core.v1", 1, 2);
        step.crossParticipantTransforms = {{.target = Participant("project.gameplay.v1"),
                                            .targetSchemaVersion = V<ParticipantSchemaVersion>(1)},
                                           {.target = Participant("project.gameplay.v1"),
                                            .targetSchemaVersion = V<ParticipantSchemaVersion>(1)}};
        CHECK(SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{step}).HasError());
        step.crossParticipantTransforms = {
            {.target = Participant("horo.scene.core.v1"), .targetSchemaVersion = V<ParticipantSchemaVersion>(1)}};
        CHECK(SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{step}).HasError());
    }

    TEST_CASE("Save-schema migration retains unknown optional bytes and integrity evidence", "[runtime][save][migration]") {
        auto source = Source();
        auto unknown = ParticipantState("project.future.dlc.v1", 9, false, "opaque");
        const auto stored = Bytes("wire");
        unknown.preservedChunks.push_back({.entry = {.record = Id<SaveRecordId>(31),
                                                     .owner = unknown.participant,
                                                     .storedByteLength = stored.size(),
                                                     .decodedByteLength = stored.size(),
                                                     .decodedHash = ComputeSha256(stored)},
                                           .storedBytes = stored});
        source.participants.push_back(std::move(unknown));
        std::ranges::sort(source.participants, {}, &SaveMigrationParticipantState::participant);

        auto support = Support(1, 1, 1, 1);
        support.compatibility.saveSchemaVersions = DirectAndMigratable<SaveSchemaVersionTag>(2, 1);
        SaveSchemaMigrationStep step{.id = SaveMigrationId{.value = "schema.1_to_2"},
                                     .from = V<SaveSchemaVersion>(1),
                                     .to = V<SaveSchemaVersion>(2),
                                     .migrate = [](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            candidate.saveSchemaVersion = V<SaveSchemaVersion>(2);
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        }};
        auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{step});
        REQUIRE(registry.HasValue());
        auto snapshot = registry.Value().Snapshot();
        REQUIRE(snapshot.HasValue());
        auto plan = snapshot.Value().Plan(source, support);
        REQUIRE(plan.HasValue());
        auto noEvidence = source;
        const auto missingEvidence =
            std::ranges::find(noEvidence.participants, Participant("project.future.dlc.v1"), &SaveMigrationParticipantState::participant);
        missingEvidence->preservedChunks.clear();
        CHECK(snapshot.Value().Plan(noEvidence, support).HasError());
        auto droppable = support;
        droppable.compatibility.droppableUnknownParticipants = {Participant("project.future.dlc.v1")};
        CHECK(snapshot.Value().Plan(noEvidence, droppable).HasValue());
        const auto migrated = SaveMigrationExecutor::Migrate(source, plan.Value());
        REQUIRE(migrated.HasValue());
        const auto unknownId = Participant("project.future.dlc.v1");
        const auto retained = std::ranges::find(migrated.Value().participants, unknownId, &SaveMigrationParticipantState::participant);
        REQUIRE(retained != migrated.Value().participants.end());
        CHECK(retained->preservedChunks ==
              std::ranges::find(source.participants, unknownId, &SaveMigrationParticipantState::participant)->preservedChunks);
    }

    TEST_CASE("Newer optional participant schema stays opaque unless required by another owner", "[runtime][save][migration]") {
        auto source = Source();
        auto unknown = ParticipantState("project.future.dlc.v1", 9, false, "opaque");
        const auto stored = Bytes("wire");
        unknown.preservedChunks.push_back({.entry = {.record = Id<SaveRecordId>(31),
                                                     .owner = unknown.participant,
                                                     .storedByteLength = stored.size(),
                                                     .decodedByteLength = stored.size(),
                                                     .decodedHash = ComputeSha256(stored)},
                                           .storedBytes = stored});
        source.participants.push_back(std::move(unknown));
        std::ranges::sort(source.participants, {}, &SaveMigrationParticipantState::participant);
        auto support = Support(1, 1, 1, 1);
        support.compatibility.saveSchemaVersions = DirectAndMigratable<SaveSchemaVersionTag>(2, 1);
        SaveSchemaMigrationStep step{.id = SaveMigrationId{.value = "schema.1_to_2"},
                                     .from = V<SaveSchemaVersion>(1),
                                     .to = V<SaveSchemaVersion>(2),
                                     .migrate = [](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            candidate.saveSchemaVersion = V<SaveSchemaVersion>(2);
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        }};
        auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{step});
        REQUIRE(registry.HasValue());
        auto snapshot = registry.Value().Snapshot();
        REQUIRE(snapshot.HasValue());
        const auto unknownId = Participant("project.future.dlc.v1");
        auto skewSupport = support;
        const auto supportedV1 = V<ParticipantSchemaVersion>(1);
        skewSupport.compatibility.participants.push_back(
            {.participant = unknownId, .versions = {.direct = {.minimum = supportedV1, .maximum = supportedV1}}, .required = false});
        std::ranges::sort(skewSupport.compatibility.participants, {}, &SaveParticipantCompatibility::participant);
        const auto skewPlan = snapshot.Value().Plan(source, skewSupport);
        REQUIRE(skewPlan.HasValue());
        const auto skewTarget =
            std::ranges::find(skewPlan.Value().participantTargets, unknownId, &SaveMigrationParticipantTarget::participant);
        REQUIRE(skewTarget != skewPlan.Value().participantTargets.end());
        CHECK(skewTarget->preserveUnknown);
        CHECK(SaveMigrationExecutor::Migrate(source, skewPlan.Value()).HasValue());
        skewSupport.compatibility.participants.front().requiredDependencies = {unknownId};
        CHECK(snapshot.Value().Plan(source, skewSupport).HasError());
    }

    TEST_CASE("Save-schema migration refuses to publish changed opaque optional bytes", "[runtime][save][migration]") {
        auto source = Source();
        auto unknown = ParticipantState("project.future.dlc.v1", 9, false, "opaque");
        const auto stored = Bytes("wire");
        unknown.preservedChunks.push_back({.entry = {.record = Id<SaveRecordId>(31),
                                                     .owner = unknown.participant,
                                                     .storedByteLength = stored.size(),
                                                     .decodedByteLength = stored.size(),
                                                     .decodedHash = ComputeSha256(stored)},
                                           .storedBytes = stored});
        source.participants.push_back(std::move(unknown));
        std::ranges::sort(source.participants, {}, &SaveMigrationParticipantState::participant);
        auto support = Support(1, 1, 1, 1);
        support.compatibility.saveSchemaVersions = DirectAndMigratable<SaveSchemaVersionTag>(2, 1);
        const auto unknownId = Participant("project.future.dlc.v1");
        SaveSchemaMigrationStep step{.id = SaveMigrationId{.value = "schema.1_to_2"},
                                     .from = V<SaveSchemaVersion>(1),
                                     .to = V<SaveSchemaVersion>(2),
                                     .migrate = [unknownId](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            candidate.saveSchemaVersion = V<SaveSchemaVersion>(2);
            const auto found = std::ranges::find(candidate.participants, unknownId, &SaveMigrationParticipantState::participant);
            found->preservedChunks[0].storedBytes[0] = std::byte{'X'};
            found->preservedChunks[0].entry.decodedHash = ComputeSha256(found->preservedChunks[0].storedBytes);
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        }};
        auto badRegistry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{step});
        REQUIRE(badRegistry.HasValue());
        auto badSnapshot = badRegistry.Value().Snapshot();
        REQUIRE(badSnapshot.HasValue());
        const auto badPlan = badSnapshot.Value().Plan(source, support);
        REQUIRE(badPlan.HasValue());
        const auto lost = SaveMigrationExecutor::Migrate(source, badPlan.Value());
        REQUIRE(lost.HasError());
        CHECK(lost.ErrorValue().message.find("project.future.dlc.v1") != std::string::npos);
    }

    TEST_CASE("Migration errors are exposed through typed save diagnostics", "[runtime][save][migration]") {
        const auto error = MakeError(SaveErrors::MigrationPathMissing);
        const auto diagnostic = MakeSaveDiagnosticRecord(error);
        REQUIRE(diagnostic.HasValue());
        CHECK(diagnostic.Value().Category() == SaveFailureCategory::Compatibility);
        CHECK(diagnostic.Value().Code().Value() == "save.migration.path_missing");
        const auto descriptors = SaveDiagnosticErrorDescriptors();
        CHECK(std::ranges::any_of(descriptors, [](const auto *descriptor) {
            return descriptor->code.Value() == "save.migration.path_missing";
        }));
    }
}  // namespace
