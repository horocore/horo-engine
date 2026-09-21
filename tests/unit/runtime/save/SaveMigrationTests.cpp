#include "Horo/Runtime/Save/SaveDiagnostics.h"
#include "Horo/Runtime/Save/SaveMigration.h"
#include "SaveTestUtils.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
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
        return [target, suffix](const SaveMigrationCandidate &source, const SaveMigrationStepContext &) {
            auto candidate = source;
            candidate.archiveFormatVersion = V<ArchiveFormatVersion>(target);
            const auto bytes = Bytes(suffix);
            candidate.archiveBytes.insert(candidate.archiveBytes.end(), bytes.begin(), bytes.end());
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        };
    }

    [[nodiscard]] SaveMigrationFn BumpParticipant(const char *participant, const std::uint32_t target, const std::string_view suffix = {}) {
        return [participant = std::string{participant}, target, suffix](const SaveMigrationCandidate &source,
                                                                        const SaveMigrationStepContext &) {
            auto candidate = source;
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

    TEST_CASE("Executor rejects participant steps that modify unrelated state", "[runtime][save][migration]") {
        auto invalidParticipant = ParticipantStep("scene.1_to_2", "horo.scene.core.v1", 1, 2);
        invalidParticipant.migrate = [](const SaveMigrationCandidate &source, const SaveMigrationStepContext &) {
            auto candidate = source;
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
