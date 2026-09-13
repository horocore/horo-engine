#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveSlotIndex.h"
#include "SaveTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using namespace Test;

        [[nodiscard]] Sha256Digest Digest(const std::uint8_t suffix) {
            Sha256Digest value{};
            value.bytes.back() = suffix;
            return value;
        }

        [[nodiscard]] SaveSlotCatalogEntry Entry(const std::uint8_t slot, const std::uint8_t generation) {
            return {.publication = {.slot = Id<SaveGameSlotId>(slot),
                                    .generation = Id<SlotGenerationId>(generation),
                                    .kind = SaveSlotKind::Manual,
                                    .savedAtUnixMilliseconds = 1'700'000'000'000ULL,
                                    .playTimeNanoseconds = 42,
                                    .baseScene = Id<SaveBaseSceneId>(3),
                                    .productCompatibility = V<ProductSaveCompatibilityVersion>(1),
                                    .saveSchema = V<SaveSchemaVersion>(1),
                                    .projectBuildId = "test-build",
                                    .canonicalState = {.value = Digest(generation)},
                                    .archiveContent = {.value = Digest(static_cast<std::uint8_t>(generation + 40))}},
                    .display = {.displayName = "Slot"}};
        }

        [[nodiscard]] SaveSlotArtifactObservation Committed(const std::uint8_t slot, const std::uint8_t generation) {
            return {.state = SaveSlotArtifactState::Committed, .entry = Entry(slot, generation)};
        }

        TEST_CASE("Deleting the derived index retains every valid committed slot", "[unit][save][slot-index]") {
            auto rebuild = SaveSlotIndexRebuilder::Create(std::nullopt, 1).Value();
            const std::array observations{Committed(3, 30), Committed(1, 10), Committed(2, 20)};

            REQUIRE(rebuild.Consume(observations, 2).Value() == 2);
            REQUIRE(rebuild.Consume(std::span{observations}.subspan(2), 2).Value() == 1);
            auto result = rebuild.Finalize().Value();

            REQUIRE(ValidateSaveSlotIndex(result.candidate).HasValue());
            REQUIRE(result.candidate.entries.size() == 3);
            REQUIRE(result.candidate.entries[0].publication.slot == Id<SaveGameSlotId>(1));
            REQUIRE(result.candidate.entries[1].publication.slot == Id<SaveGameSlotId>(2));
            REQUIRE(result.candidate.entries[2].publication.slot == Id<SaveGameSlotId>(3));
            REQUIRE(result.diagnostics.size() == 3);
            for (const auto &diagnostic : result.diagnostics)
                REQUIRE(diagnostic.kind == SaveSlotIndexDiagnosticKind::Missing);
        }

        TEST_CASE("Rebuild detects stale orphan duplicate and corrupt storage deterministically", "[unit][save][slot-index]") {
            SaveSlotIndex previous{.revision = 4, .entries = {Entry(1, 9), Entry(4, 40)}};
            const std::array observations{Committed(3, 31), Committed(1, 10), Committed(3, 30),
                                          SaveSlotArtifactObservation{.state = SaveSlotArtifactState::Corrupt,
                                                                      .suspectedSlot = Id<SaveGameSlotId>(5)},
                                          SaveSlotArtifactObservation{.state = SaveSlotArtifactState::Temporary}};
            auto rebuild = SaveSlotIndexRebuilder::Create(previous, 5).Value();
            REQUIRE(rebuild.Consume(observations, observations.size()).Value() == observations.size());
            const auto result = rebuild.Finalize().Value();

            REQUIRE(result.candidate.entries.size() == 2);
            REQUIRE(result.candidate.entries[0].publication.generation == Id<SlotGenerationId>(10));
            REQUIRE(result.candidate.entries[1].publication.generation == Id<SlotGenerationId>(30));
            REQUIRE(result.artifactsExamined == 5);
            REQUIRE(result.diagnostics.size() == 5);
            REQUIRE(result.diagnostics[0].kind == SaveSlotIndexDiagnosticKind::Stale);
            REQUIRE(result.diagnostics[1].kind == SaveSlotIndexDiagnosticKind::Missing);
            REQUIRE(result.diagnostics[2].kind == SaveSlotIndexDiagnosticKind::Duplicate);
            REQUIRE(result.diagnostics[3].kind == SaveSlotIndexDiagnosticKind::Orphaned);
            REQUIRE(result.diagnostics[4].kind == SaveSlotIndexDiagnosticKind::Corrupt);
        }

        TEST_CASE("Rebuild output is independent of scan ordering and page boundaries", "[unit][save][slot-index]") {
            const std::array firstOrder{Committed(3, 31), Committed(1, 10), Committed(3, 30), Committed(2, 20)};
            const std::array secondOrder{Committed(2, 20), Committed(3, 30), Committed(1, 10), Committed(3, 31)};
            auto first = SaveSlotIndexRebuilder::Create(std::nullopt, 7).Value();
            auto second = SaveSlotIndexRebuilder::Create(std::nullopt, 7).Value();
            REQUIRE(first.Consume(firstOrder, firstOrder.size()).HasValue());
            REQUIRE(second.Consume(std::span{secondOrder}.first(1), 1).HasValue());
            REQUIRE(second.Consume(std::span{secondOrder}.subspan(1), 3).HasValue());

            const auto firstResult = first.Finalize().Value();
            const auto secondResult = second.Finalize().Value();
            REQUIRE(firstResult.candidate.entries == secondResult.candidate.entries);
            REQUIRE(firstResult.diagnostics.size() == secondResult.diagnostics.size());
            for (std::size_t index = 0; index < firstResult.diagnostics.size(); ++index) {
                REQUIRE(firstResult.diagnostics[index].kind == secondResult.diagnostics[index].kind);
                REQUIRE(firstResult.diagnostics[index].slot == secondResult.diagnostics[index].slot);
                REQUIRE(firstResult.diagnostics[index].generation == secondResult.diagnostics[index].generation);
            }
        }

        TEST_CASE("Failed rebuild leaves the last published index untouched", "[unit][save][slot-index]") {
            const SaveSlotIndex published{.revision = 8, .entries = {Entry(1, 10)}};
            const SaveSlotIndex preserved = published;
            const SaveSlotIndexLimits limits{.maximumEntries = 1, .maximumArtifacts = 1, .maximumDiagnostics = 1};
            auto rebuild = SaveSlotIndexRebuilder::Create(published, 9, limits).Value();
            const std::array observations{Committed(1, 11), Committed(2, 20)};

            REQUIRE(rebuild.Consume(observations, observations.size()).ErrorValue().code.Value() ==
                    SaveErrors::SlotIndexLimitExceeded.code.Value());
            REQUIRE(published.revision == preserved.revision);
            REQUIRE(published.entries == preserved.entries);
        }

        TEST_CASE("Index validation rejects schema revision duplicates and malformed entries", "[unit][save][slot-index]") {
            SaveSlotIndex index{.revision = 1, .entries = {Entry(1, 10)}};
            REQUIRE(ValidateSaveSlotIndex(index).HasValue());
            index.schemaVersion = 2;
            REQUIRE(ValidateSaveSlotIndex(index).HasError());
            index = {.revision = 0, .entries = {Entry(1, 10)}};
            REQUIRE(ValidateSaveSlotIndex(index).HasError());
            index = {.revision = 1, .entries = {Entry(1, 10), Entry(1, 11)}};
            REQUIRE(ValidateSaveSlotIndex(index).ErrorValue().code.Value() == SaveErrors::SlotIndexCorrupt.code.Value());
            index = {.revision = 1, .entries = {Entry(1, 10)}};
            index.entries.front().publication.slot = {};
            REQUIRE(ValidateSaveSlotIndex(index).ErrorValue().code.Value() == SaveErrors::SlotIndexCorrupt.code.Value());

            const SaveSlotIndex published{.revision = 3, .entries = {Entry(1, 10)}};
            REQUIRE(SaveSlotIndexRebuilder::Create(published, 3).ErrorValue().code.Value() == SaveErrors::SlotIndexInvalid.code.Value());
            REQUIRE(SaveSlotIndexRebuilder::Create(published, 2).HasError());
        }
    }  // namespace
}  // namespace Horo::Runtime
