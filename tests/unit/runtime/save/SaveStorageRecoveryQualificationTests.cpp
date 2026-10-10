#include "Horo/Runtime/Save/SaveSlotIndex.h"
#include "Horo/Runtime/Save/SaveSlotRecovery.h"
#include "Horo/Runtime/Save/SaveTestCompositions.h"
#include "SaveArchiveReaderTestHelpers.h"
#include "SaveFilesystemTestSupport.h"

#include <array>
#include <memory>
#include <optional>
#include <utility>

namespace Horo::Runtime {
    namespace {
        using namespace ArchiveReaderTest;
        using namespace SaveFilesystemTest;

        /** @brief Derives exact catalog evidence from production reader admission, never fabricated digests. */
        SaveSlotCatalogEntry Catalog(const ValidatedSaveArchive &archive) {
            const auto &header = archive.Header();
            return {.publication = {.slot = header.slot,
                                    .generation = header.slotGeneration,
                                    .savedAtUnixMilliseconds = header.capturedAtUnixMilliseconds,
                                    .playTimeNanoseconds = header.playTimeNanoseconds,
                                    .baseScene = header.baseScene,
                                    .productCompatibility = header.productCompatibility,
                                    .saveSchema = archive.Manifest().saveSchemaVersion,
                                    .projectBuildId = header.projectBuildId,
                                    .canonicalState = archive.Manifest().canonicalState,
                                    .archiveContent = archive.Integrity().archiveContent},
                    .display = {.displayName = "Duplicate display label"}};
        }

        /** @brief Reopens physical storage for the exact admitted archive namespace. */
        SaveFilesystemStorage OpenArchiveStorage(const ProductSaveRoot &root, const SaveArchiveHeader &header) {
            const SaveNamespaceId name{header.product, header.environment, UserProfileOwner{header.user, header.profile}};
            auto opened = SaveFilesystemStorage::Open(root, name);
            REQUIRE(opened.HasValue());
            return std::move(opened).Value();
        }

        /** @brief Bounds physical bytes before production validation and keeps the source evidence immutable. */
        ImmutableSaveArchive ReadArchive(const SaveFilesystemStorage &storage, const SaveGameSlotId slot, const std::size_t maximumBytes) {
            auto bytes = storage.Read(slot, maximumBytes);
            REQUIRE(bytes.HasValue());
            return {std::make_shared<const std::vector<std::byte>>(std::move(bytes).Value())};
        }

        /** @brief Admits independent fixture bytes before deriving filesystem ownership. */
        ValidatedSaveArchive AdmitFixture(const std::vector<std::byte> &bytes) {
            auto admitted = SaveArchiveReader{}.Read(bytes);
            REQUIRE(admitted.HasValue());
            return std::move(admitted).Value();
        }

        /** @brief Owns one physically published valid archive and all of its root/storage lifetimes. */
        struct NativeArchiveFixture final {
            TemporaryDirectory temporary;
            FixedEnvironment environment;
            const std::vector<std::byte> bytes = MakeArchive().bytes;
            const ValidatedSaveArchive admitted = AdmitFixture(bytes);
            const ProductSaveRoot root =
                Resolve({.product = admitted.Header().product, .platform = SaveRootPlatform::Test, .testStateRoot = temporary.Path()},
                        environment);
            SaveFilesystemStorage storage = OpenArchiveStorage(root, admitted.Header());

            NativeArchiveFixture() {
                REQUIRE(storage.Replace(admitted.Header().slot, bytes).HasValue());
            }
        };

        TEST_CASE("Corrupt native current archives yield verified LKG plans without overwriting either evidence",
                  "[save][qualification][filesystem][recovery]") {
            NativeArchiveFixture fixture;
            const auto &header = fixture.admitted.Header();
            auto &storage = fixture.storage;
            const SaveSlotRecoveryArtifact backup{Catalog(fixture.admitted), ReadArchive(storage, header.slot, fixture.bytes.size()), 1};
            auto corruptBytes = fixture.bytes;
            corruptBytes[SaveArchivePreambleByteLength] ^= std::byte{1};
            REQUIRE(storage.Replace(header.slot, corruptBytes).HasValue());
            const SaveSlotRecoveryArtifact current{Catalog(fixture.admitted), ReadArchive(storage, header.slot, fixture.bytes.size()), 2};
            SaveArchiveRecoveryValidator validator{SaveArchiveReader{}, UnknownPolicy()};
            const std::array backups{backup};
            const auto planned =
                SaveSlotRecoveryPlanner(validator, {})
                    .Build(
                        {.slot = header.slot, .trigger = SaveSlotRecoveryTrigger::CorruptCurrent, .current = current, .backups = backups});
            REQUIRE(planned.HasValue());
            REQUIRE(planned.Value().promotion);
            CHECK(planned.Value().promotion->artifact.archive.bytes == backup.archive.bytes);
            CHECK(planned.Value().decision == SaveSlotRecoveryDecision::AutomaticPromotion);
            CHECK(planned.Value().currentValidation->state == SaveSlotRecoveryValidationState::Corrupt);
            CHECK(storage.Read(header.slot, fixture.bytes.size()).Value() == corruptBytes);
            CHECK(*backup.archive.bytes == fixture.bytes);
            CHECK(planned.Value().retainedQuarantine.size() == 1);
        }

        TEST_CASE("Native archive compatibility failures retain distinct evidence and require recovery confirmation",
                  "[save][qualification][filesystem][recovery]") {
            NativeArchiveFixture fixture;
            const auto &header = fixture.admitted.Header();
            auto &storage = fixture.storage;
            const auto archive = ReadArchive(storage, header.slot, fixture.bytes.size());
            auto wrongScope = Catalog(fixture.admitted);
            wrongScope.publication.slot = Test::Id<SaveGameSlotId>(9);
            const SaveSlotRecoveryArtifact current{wrongScope, archive, 2};
            const std::array backups{SaveSlotRecoveryArtifact{Catalog(fixture.admitted), archive, 1}};
            SaveArchiveRecoveryValidator validator{SaveArchiveReader{}, UnknownPolicy()};
            const auto planned = SaveSlotRecoveryPlanner(validator, {})
                                     .Build({.slot = header.slot,
                                             .trigger = SaveSlotRecoveryTrigger::IncompatibleCurrent,
                                             .current = current,
                                             .backups = backups});
            REQUIRE(planned.HasValue());
            CHECK(planned.Value().RequiresUserConfirmation());
            CHECK(planned.Value().currentValidation->state == SaveSlotRecoveryValidationState::Incompatible);
            CHECK(storage.Read(header.slot, fixture.bytes.size()).Value() == fixture.bytes);
        }

        TEST_CASE("Index reconstruction from native verified archives excludes staging and preserves authoritative bytes",
                  "[save][qualification][filesystem][index]") {
            NativeArchiveFixture fixture;
            const auto &header = fixture.admitted.Header();
            auto &storage = fixture.storage;
            const auto bytes = ReadArchive(storage, header.slot, fixture.bytes.size());
            const auto verified = SaveArchiveReader{}.Read(bytes.bytes);
            REQUIRE(verified.HasValue());
            const auto entry = Catalog(verified.Value());
            const std::array observations{SaveSlotArtifactObservation{SaveSlotArtifactState::Temporary, std::nullopt, header.slot},
                                          SaveSlotArtifactObservation{SaveSlotArtifactState::Committed, entry, std::nullopt}};
            auto created = SaveSlotIndexRebuilder::Create(std::nullopt, 1);
            REQUIRE(created.HasValue());
            auto rebuilder = std::move(created).Value();
            REQUIRE(rebuilder.Consume(observations, 1).Value() == 1);
            REQUIRE(rebuilder.Consume(std::span{observations}.subspan(1), 1).Value() == 1);
            const auto rebuilt = rebuilder.Finalize();
            REQUIRE(rebuilt.HasValue());
            REQUIRE(rebuilt.Value().candidate.entries.size() == 1);
            CHECK(rebuilt.Value().candidate.entries.front() == entry);
            CHECK(ValidateSaveSlotIndex(rebuilt.Value().candidate).HasValue());
            CHECK(storage.Read(header.slot, fixture.bytes.size()).Value() == fixture.bytes);
        }

        TEST_CASE("Deterministic archive publication faults preserve the same admitted source bytes",
                  "[save][qualification][deterministic][headless]") {
            const auto fixture = MakeArchive();
            auto admitted = SaveArchiveReader{}.Read(fixture.bytes);
            REQUIRE(admitted.HasValue());
            const auto &header = admitted.Value().Header();
            const SaveNamespaceId name{header.product, header.environment, UserProfileOwner{header.user, header.profile}};
            const SaveCompositionAddress address{EncodeSaveNamespaceKey(name).Value(), header.slot};
            auto created = CreateDeterministicMockSaveComposition({4, 1, fixture.bytes.size()});
            REQUIRE(created.HasValue());
            auto composition = std::move(created).Value();
            REQUIRE(composition->Submit({.address = address, .bytes = fixture.bytes}).HasValue());
            REQUIRE(composition->AdvanceOne());
            auto damaged = fixture.bytes;
            damaged.back() ^= std::byte{1};
            auto attempt = composition->Submit({.address = address, .bytes = damaged, .fault = SaveCompositionFault::InjectedFailure});
            REQUIRE(attempt.HasValue());
            REQUIRE(composition->AdvanceOne());
            const auto failed = composition->Snapshot(attempt.Value());
            REQUIRE(failed);
            CHECK(failed->state == SaveCompositionOperationState::Failed);
            CHECK(failed->commit == SaveCompositionCommitOutcome::NotCommitted);
            const auto retained = composition->ObjectSnapshot(address);
            REQUIRE(retained);
            CHECK(*retained == fixture.bytes);
            CHECK(SaveArchiveReader{}.Read(*retained).HasValue());
        }
    }  // namespace
}  // namespace Horo::Runtime
