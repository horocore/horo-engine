#include "Horo/Runtime/Save/SaveDiagnostics.h"
#include "Horo/Runtime/Save/SaveMigration.h"
#include "SaveTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
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

    [[nodiscard]] SaveMigrationSource Source() {
        return {.archiveFormatVersion = V<ArchiveFormatVersion>(1),
                .saveSchemaVersion = V<SaveSchemaVersion>(1),
                .productCompatibility = V<ProductSaveCompatibilityVersion>(1),
                .participants = {{.participant = Participant("horo.scene.core.v1"),
                                  .schemaVersion = V<ParticipantSchemaVersion>(1),
                                  .required = true,
                                  .payload = Bytes("scene")},
                                 {.participant = Participant("project.gameplay.v1"),
                                  .schemaVersion = V<ParticipantSchemaVersion>(1),
                                  .required = false,
                                  .payload = Bytes("gameplay")}},
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

    [[nodiscard]] SaveMigrationSupportDescriptor Support() {
        const auto oneArchive = V<ArchiveFormatVersion>(1);
        const auto oneSchema = V<SaveSchemaVersion>(1);
        const auto oneProduct = V<ProductSaveCompatibilityVersion>(1);
        const auto oneParticipant = V<ParticipantSchemaVersion>(1);
        const auto twoParticipant = V<ParticipantSchemaVersion>(2);
        return {
            .compatibility = {.archiveVersions = {.direct = {.minimum = oneArchive, .maximum = oneArchive}},
                              .saveSchemaVersions = {.direct = {.minimum = oneSchema, .maximum = oneSchema}},
                              .productVersions = {.direct = {.minimum = oneProduct, .maximum = oneProduct}},
                              .participants = {{.participant = Participant("horo.scene.core.v1"),
                                                .versions = {.direct = {.minimum = twoParticipant, .maximum = twoParticipant},
                                                             .migrationSource =
                                                                 SaveVersionRange<ParticipantSchemaVersionTag>{.minimum = oneParticipant,
                                                                                                               .maximum = oneParticipant}},
                                                .required = true},
                                               {.participant = Participant("project.gameplay.v1"),
                                                .versions = {.direct = {.minimum = oneParticipant, .maximum = oneParticipant}},
                                                .required = false}}}};
    }

    [[nodiscard]] ParticipantMigrationStep ParticipantStep(const char *id) {
        return {.id = SaveMigrationId{.value = id},
                .participant = Participant("horo.scene.core.v1"),
                .kind = SaveMigrationStepKind::Sequential,
                .from = V<ParticipantSchemaVersion>(1),
                .to = V<ParticipantSchemaVersion>(2),
                .migrate =
                    [](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            candidate.participants.front().schemaVersion = V<ParticipantSchemaVersion>(2);
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        },
                .estimatedWork = 1};
    }

    [[nodiscard]] Result<SaveMigrationCandidate> Migrate(const SaveMigrationSource &source, const ParticipantMigrationStep &step) {
        auto registry = SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{step});
        if (registry.HasError())
            return Result<SaveMigrationCandidate>::Failure(registry.ErrorValue());
        auto snapshot = registry.Value().Snapshot();
        if (snapshot.HasError())
            return Result<SaveMigrationCandidate>::Failure(snapshot.ErrorValue());
        const auto plan = snapshot.Value().Plan(source, Support());
        if (plan.HasError())
            return Result<SaveMigrationCandidate>::Failure(plan.ErrorValue());
        return SaveMigrationExecutor::Migrate(source, plan.Value());
    }

    TEST_CASE("Participant record migration advances each owned record and preserves provenance", "[runtime][save][migration]") {
        auto source = Source();
        for (std::uint8_t suffix : {std::uint8_t{31}, std::uint8_t{32}})
            AddRecord(source, 0, suffix);
        auto step = ParticipantStep("scene.records.1_to_2");
        step.migrate = {};
        step.migrateRecord = [](const std::span<const std::byte> input, const SaveMigrationRecordContext &context) {
            CHECK(context.from == V<ParticipantSchemaVersion>(1));
            CHECK(context.to == V<ParticipantSchemaVersion>(2));
            CHECK(context.participant == Participant("horo.scene.core.v1"));
            auto output = std::vector<std::byte>{input.begin(), input.end()};
            output.push_back(std::byte{0x42});
            return Result<std::vector<std::byte>>::Success(std::move(output));
        };
        const auto migrated = Migrate(source, step);
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
        auto step = ParticipantStep("scene.cross.1_to_2");
        step.migrate = [](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            candidate.participants.front().schemaVersion = V<ParticipantSchemaVersion>(2);
            if (candidate.participants.size() > 1)
                candidate.participants.back().records.front().payload.push_back(std::byte{0x42});
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        };
        auto source = Source();
        AddRecord(source, 1, 41);
        const auto forbidden = Migrate(source, step);
        REQUIRE(forbidden.HasError());
        CHECK(forbidden.ErrorValue().message.find("project.gameplay.v1") != std::string::npos);
        step.crossParticipantTransforms = {
            {.target = Participant("project.gameplay.v1"), .targetSchemaVersion = V<ParticipantSchemaVersion>(1)}};
        const auto permitted = Migrate(source, step);
        REQUIRE(permitted.HasValue());
        CHECK(permitted.Value().participants.back().records.front().payload.size() ==
              source.participants.back().records.front().payload.size() + 1);
        CHECK(source.participants.back().records.front().payload == Bytes("old"));
        step.crossParticipantTransforms.front().targetSchemaVersion = V<ParticipantSchemaVersion>(2);
        CHECK(Migrate(source, step).HasError());
        source.participants.pop_back();
        CHECK(Migrate(source, step).HasValue());
    }

    TEST_CASE("Record failure reports field context and rolls back detached migration", "[runtime][save][migration]") {
        auto source = Source();
        AddRecord(source, 0, 31);
        auto step = ParticipantStep("scene.fail.1_to_2");
        step.migrate = {};
        step.migrateRecord = [](std::span<const std::byte>, const SaveMigrationRecordContext &context) {
            return Result<std::vector<std::byte>>::Failure(
                context.Fail(MakeError(SaveErrors::MigrationCandidateInvalid, "invalid value"), "transform.position.x"));
        };
        const auto before = source;
        const auto result = Migrate(source, step);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().cause.Get() != nullptr);
        CHECK(result.ErrorValue().message.find("scene.fail.1_to_2") != std::string::npos);
        CHECK(result.ErrorValue().message.find("field=transform.position.x") != std::string::npos);
        CHECK(result.ErrorValue().cause.Get()->cause.Get()->message.find("field=transform.position.x") != std::string::npos);
        CHECK(source == before);
    }

    TEST_CASE("Record identity and provenance cannot be rewritten by an owner callback", "[runtime][save][migration]") {
        auto source = Source();
        AddRecord(source, 0, 31);
        auto step = ParticipantStep("scene.provenance.1_to_2");
        step.migrate = [](SaveMigrationCandidate candidate, const SaveMigrationStepContext &) {
            candidate.participants.front().schemaVersion = V<ParticipantSchemaVersion>(2);
            candidate.participants.front().records.front().schemaVersion = V<ParticipantSchemaVersion>(2);
            candidate.participants.front().records.front().sourceSchemaVersion = V<ParticipantSchemaVersion>(2);
            return Result<SaveMigrationCandidate>::Success(std::move(candidate));
        };
        const auto migrated = Migrate(source, step);
        REQUIRE(migrated.HasError());
        CHECK(migrated.ErrorValue().code.Value() == SaveErrors::MigrationCandidateInvalid.code.Value());
        CHECK(source.participants.front().records.front().sourceSchemaVersion == V<ParticipantSchemaVersion>(1));
    }

    TEST_CASE("Cross-participant transform contracts reject duplicate or own targets", "[runtime][save][migration]") {
        auto step = ParticipantStep("scene.contract.1_to_2");
        step.crossParticipantTransforms = {{.target = Participant("project.gameplay.v1"),
                                            .targetSchemaVersion = V<ParticipantSchemaVersion>(1)},
                                           {.target = Participant("project.gameplay.v1"),
                                            .targetSchemaVersion = V<ParticipantSchemaVersion>(1)}};
        CHECK(SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{step}).HasError());
        step.crossParticipantTransforms = {
            {.target = Participant("horo.scene.core.v1"), .targetSchemaVersion = V<ParticipantSchemaVersion>(1)}};
        CHECK(SaveMigrationRegistry::Create(std::vector<SaveMigrationDefinition>{step}).HasError());
    }
}  // namespace
