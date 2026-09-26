#include "Horo/Runtime/Save/SaveCloudRevisionMetadata.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using namespace Test;

        [[nodiscard]] SaveCloudMetadataScope Scope(const std::uint8_t profile = 4, const std::uint8_t provider = 5,
                                                   const std::uint8_t account = 6) {
            return {.localNamespace = {.product = Id<ProductStorageId>(1),
                                       .environment = Id<EnvironmentStorageId>(2),
                                       .owner = UserProfileOwner{Id<LocalUserStorageId>(3), Id<GameProfileId>(profile)}},
                    .provider = Id<SaveCloudProviderId>(provider),
                    .account = Id<SaveCloudAccountId>(account)};
        }

        [[nodiscard]] SaveSlotIndex Index(const std::uint64_t revision = 1) {
            return {.revision = revision, .entries = {Entry(1, 10), Entry(2, 20)}};
        }

        [[nodiscard]] SaveCloudRevisionMetadata Metadata(const SaveSlotIndex &index, const SaveCloudMetadataScope &scope = Scope(),
                                                         const std::uint64_t revision = 1) {
            return ReconcileSaveCloudRevisionMetadata(index, scope, std::nullopt, revision).Value();
        }

        [[nodiscard]] SaveCloudObjectRef Object() {
            return {.key = {std::byte{0x00}, std::byte{0xff}}, .revision = std::vector<std::byte>{std::byte{0x7f}}};
        }

        TEST_CASE("A cloud sidecar is bound to the exact index revision generation and archive", "[unit][save][cloud-metadata]") {
            auto index = Index();
            auto metadata = Metadata(index);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasValue());

            metadata.indexRevision = 2;
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataStale.code.Value());
            metadata.indexRevision = 1;
            metadata.records[0].generation = Id<SlotGenerationId>(11);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataStale.code.Value());
            metadata.records[0].generation = Id<SlotGenerationId>(10);
            metadata.records[0].archive = Entry(1, 11).publication.archiveContent;
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataStale.code.Value());
        }

        TEST_CASE("Profiles providers and accounts cannot borrow one another's cloud evidence", "[unit][save][cloud-metadata]") {
            const auto index = Index();
            auto prior = Metadata(index);
            prior.records[0].state = SaveCloudGenerationState::Clean;
            prior.records[0].object = Object();
            prior.records[0].lastConfirmed = Id<SaveCloudMutationId>(8);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(7), prior).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataInvalid.code.Value());
            REQUIRE(SaveCloudRevisionSnapshot::Create(index, Scope(7), prior).HasError());

            for (const auto scope : {Scope(7, 5, 6), Scope(4, 7, 6), Scope(4, 5, 7)}) {
                const auto rebuilt = ReconcileSaveCloudRevisionMetadata(index, scope, prior, 2).Value();
                REQUIRE(rebuilt.records[0].state == SaveCloudGenerationState::Unknown);
                REQUIRE_FALSE(rebuilt.records[0].object);
                REQUIRE_FALSE(rebuilt.records[0].lastConfirmed);
            }
            const auto retained = ReconcileSaveCloudRevisionMetadata(index, Scope(), prior, 2).Value();
            REQUIRE(retained.records[0].state == SaveCloudGenerationState::Clean);
            REQUIRE(retained.records[0].object == prior.records[0].object);
            REQUIRE(retained.records[0].lastConfirmed == prior.records[0].lastConfirmed);
        }

        TEST_CASE("Reconciliation resets replaced generations and interrupted transfers", "[unit][save][cloud-metadata]") {
            auto oldIndex = Index();
            auto prior = Metadata(oldIndex);
            prior.records[0].state = SaveCloudGenerationState::Uploading;
            prior.records[0].object = Object();
            prior.records[1].state = SaveCloudGenerationState::Dirty;
            prior.records[1].object = Object();

            const auto restarted = ReconcileSaveCloudRevisionMetadata(oldIndex, Scope(), prior, 2).Value();
            REQUIRE(restarted.records[0].state == SaveCloudGenerationState::Unknown);
            REQUIRE(restarted.records[0].object == prior.records[0].object);
            REQUIRE(restarted.records[1].state == SaveCloudGenerationState::Dirty);

            auto current = Index(2);
            current.entries[1] = Entry(2, 21);
            const auto rebuilt = ReconcileSaveCloudRevisionMetadata(current, Scope(), prior, 2).Value();
            REQUIRE(rebuilt.indexRevision == 2);
            REQUIRE(rebuilt.records[0].state == SaveCloudGenerationState::Unknown);
            REQUIRE(rebuilt.records[0].object == prior.records[0].object);
            REQUIRE(rebuilt.records[1].generation == Id<SlotGenerationId>(21));
            REQUIRE(rebuilt.records[1].state == SaveCloudGenerationState::Unknown);
            REQUIRE_FALSE(rebuilt.records[1].object);
            REQUIRE(ValidateSaveCloudRevisionMetadata(current, Scope(), rebuilt).HasValue());
        }

        TEST_CASE("A missing or invalid sidecar rebuilds without inventing remote authority", "[unit][save][cloud-metadata]") {
            const auto index = Index(3);
            const auto absent = ReconcileSaveCloudRevisionMetadata(index, Scope(), std::nullopt, 1).Value();
            REQUIRE(absent.records.size() == 2);
            REQUIRE_FALSE(absent.records[0].object);
            REQUIRE(absent.records[0].state == SaveCloudGenerationState::Unknown);

            auto corrupt = absent;
            corrupt.schemaVersion = 99;
            corrupt.records[0].state = SaveCloudGenerationState::Clean;
            corrupt.records[0].object = Object();
            const auto rebuilt = ReconcileSaveCloudRevisionMetadata(index, Scope(), corrupt, 2).Value();
            REQUIRE(rebuilt.records[0].state == SaveCloudGenerationState::Unknown);
            REQUIRE_FALSE(rebuilt.records[0].object);
        }

        TEST_CASE("An unchanged generation retains confirmed provider evidence across an index revision", "[unit][save][cloud-metadata]") {
            const auto oldIndex = Index();
            auto prior = Metadata(oldIndex);
            prior.records[0].state = SaveCloudGenerationState::Clean;
            prior.records[0].object = Object();
            const auto current = Index(2);
            const auto rebuilt = ReconcileSaveCloudRevisionMetadata(current, Scope(), prior, 2).Value();
            REQUIRE(rebuilt.records[0].state == SaveCloudGenerationState::Clean);
            REQUIRE(rebuilt.records[0].object == prior.records[0].object);
            REQUIRE(ValidateSaveCloudRevisionMetadata(current, Scope(), rebuilt).HasValue());
        }

        TEST_CASE("Reconciliation never rolls a newer confirmed sidecar back onto an older index", "[unit][save][cloud-metadata]") {
            const auto current = Index(2);
            auto confirmed = Metadata(current);
            confirmed.records[0].state = SaveCloudGenerationState::Clean;
            confirmed.records[0].object = Object();
            const auto older = Index(1);
            REQUIRE(ReconcileSaveCloudRevisionMetadata(older, Scope(), confirmed, 2).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataStale.code.Value());
            REQUIRE(confirmed.records[0].state == SaveCloudGenerationState::Clean);
        }

        TEST_CASE("Validation rejects malformed and unbounded provider evidence", "[unit][save][cloud-metadata]") {
            const auto index = Index();
            auto metadata = Metadata(index);
            metadata.records[0].state = SaveCloudGenerationState::Clean;
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataInvalid.code.Value());
            metadata.records[0].object = Object();
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasValue());
            metadata.records[0].object->key.clear();
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasError());
            metadata.records[0].object->key.resize(513);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataLimitExceeded.code.Value());
            metadata.records[0].object = Object();
            metadata.records[0].object->revision->resize(513);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataLimitExceeded.code.Value());
            metadata.records[0].object = Object();
            metadata.records[0].state = static_cast<SaveCloudGenerationState>(255);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasError());
        }

        TEST_CASE("Every declared sync state remains generation-bound", "[unit][save][cloud-metadata]") {
            const auto index = Index();
            auto metadata = Metadata(index);
            for (const auto state :
                 {SaveCloudGenerationState::Unknown, SaveCloudGenerationState::Clean, SaveCloudGenerationState::Dirty,
                  SaveCloudGenerationState::Uploading, SaveCloudGenerationState::Downloading, SaveCloudGenerationState::Conflicted,
                  SaveCloudGenerationState::Deleted, SaveCloudGenerationState::Failed}) {
                metadata.records[0].state = state;
                metadata.records[0].object = Object();
                REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasValue());
                REQUIRE(metadata.records[0].generation == index.entries[0].publication.generation);
            }
        }

        TEST_CASE("Snapshot owns a deep immutable copy of the validated index and sidecar", "[unit][save][cloud-metadata]") {
            auto index = Index();
            auto metadata = Metadata(index);
            metadata.records[0].object = Object();
            const auto snapshot = SaveCloudRevisionSnapshot::Create(index, Scope(), metadata).Value();
            index.entries[0] = Entry(1, 11);
            metadata.records[0].object->key[0] = std::byte{0x55};
            REQUIRE(snapshot.Index().entries[0].publication.generation == Id<SlotGenerationId>(10));
            REQUIRE(snapshot.Metadata().records[0].object->key[0] == std::byte{0x00});
            REQUIRE(ValidateSaveCloudRevisionMetadata(snapshot.Index(), Scope(), snapshot.Metadata()).HasValue());
        }

        TEST_CASE("Invalid bounds and duplicate records fail closed", "[unit][save][cloud-metadata]") {
            const auto index = Index();
            auto metadata = Metadata(index);
            metadata.records[1] = metadata.records[0];
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasError());
            const SaveCloudRevisionMetadataLimits limits{.maximumRecords = 1};
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), Metadata(index), limits).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataLimitExceeded.code.Value());
            REQUIRE(ReconcileSaveCloudRevisionMetadata(index, Scope(), std::nullopt, 1, limits).HasError());
            REQUIRE(SaveCloudRevisionSnapshot::Create(index, Scope(), metadata).HasError());
        }
    }  // namespace
}  // namespace Horo::Runtime
