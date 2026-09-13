#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveSlotCommitTransaction.h"
#include "SaveTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using namespace Test;

        [[nodiscard]] Error InjectedFailure() {
            return MakeError(SaveErrors::StorageResultInvalid, "Injected durable-store interruption.");
        }

        [[nodiscard]] Sha256Digest Digest(const std::uint8_t suffix) {
            Sha256Digest value{};
            value.bytes.back() = suffix;
            return value;
        }

        [[nodiscard]] SaveStorageAddress Address() {
            return {.namespaceAccess = {.expected = {.product = Id<ProductStorageId>(1),
                                                     .environment = Id<EnvironmentStorageId>(2),
                                                     .owner = ServerWorldOwner{Id<ServerStorageOwnerId>(3)}},
                                        .expectedRevision = 7},
                    .slot = Id<SaveGameSlotId>(4)};
        }

        [[nodiscard]] SaveSlotCatalogEntry Entry(const std::uint8_t generation) {
            return {.publication = {.slot = Address().slot,
                                    .generation = Id<SlotGenerationId>(generation),
                                    .kind = SaveSlotKind::Manual,
                                    .savedAtUnixMilliseconds = 1'700'000'000'000ULL + generation,
                                    .playTimeNanoseconds = 42,
                                    .baseScene = Id<SaveBaseSceneId>(5),
                                    .productCompatibility = V<ProductSaveCompatibilityVersion>(1),
                                    .saveSchema = V<SaveSchemaVersion>(1),
                                    .projectBuildId = "test-build",
                                    .canonicalState = {.value = Digest(generation)},
                                    .archiveContent = {.value = Digest(static_cast<std::uint8_t>(generation + 16))},
                                    .cloudState = SaveSlotCloudState::LocalOnly},
                    .display = {.displayName = "Slot", .summary = "Complete generation"}};
        }

        [[nodiscard]] ImmutableSaveArchive Archive() {
            return {.bytes =
                        std::make_shared<const std::vector<std::byte>>(std::initializer_list<std::byte>{std::byte{0x48}, std::byte{0x53}})};
        }

        class FakeCommitStore final : public ISaveSlotCommitStore {
        public:
            std::optional<SaveSlotCommitJournal> journal;
            std::optional<SaveSlotCatalogEntry> published;
            bool candidatePrepared{};
            bool retainedPrevious{};
            std::size_t failCall{};
            std::size_t calls{};
            std::size_t publishEffects{};
            std::size_t discardEffects{};
            bool publishBeforeFailure{};

            [[nodiscard]] Result<std::optional<SaveSlotCommitJournal>> LoadJournal(const SaveStorageAddress &) override {
                if (ShouldFail())
                    return Result<std::optional<SaveSlotCommitJournal>>::Failure(InjectedFailure());
                return Result<std::optional<SaveSlotCommitJournal>>::Success(journal);
            }

            [[nodiscard]] Result<void> StoreJournal(const SaveSlotCommitJournal &value) override {
                if (ShouldFail())
                    return Result<void>::Failure(InjectedFailure());
                journal = value;
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> PrepareGeneration(const SaveSlotCommitJournal &, const ImmutableSaveArchive &) override {
                if (ShouldFail())
                    return Result<void>::Failure(InjectedFailure());
                candidatePrepared = true;
                return Result<void>::Success();
            }

            [[nodiscard]] Result<SaveSlotCommitObservation> Observe(const SaveSlotCommitJournal &) override {
                if (ShouldFail())
                    return Result<SaveSlotCommitObservation>::Failure(InjectedFailure());
                return Result<SaveSlotCommitObservation>::Success({.published = published, .candidatePrepared = candidatePrepared});
            }

            [[nodiscard]] Result<void> PublishGeneration(const SaveSlotCommitJournal &value) override {
                const bool fails = ShouldFail();
                if (!candidatePrepared)
                    return Result<void>::Failure(InjectedFailure());
                if ((!fails || publishBeforeFailure) && (!published || *published != value.candidate)) {
                    retainedPrevious = !value.previous || published == value.previous;
                    published = value.candidate;
                    ++publishEffects;
                }
                if (fails)
                    return Result<void>::Failure(InjectedFailure());
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> DiscardPrepared(const SaveSlotCommitJournal &value) override {
                if (ShouldFail())
                    return Result<void>::Failure(InjectedFailure());
                if (published && *published == value.candidate)
                    return Result<void>::Failure(InjectedFailure());
                if (candidatePrepared) {
                    candidatePrepared = false;
                    ++discardEffects;
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> RemoveJournal(const SaveSlotCommitJournal &) override {
                if (ShouldFail())
                    return Result<void>::Failure(InjectedFailure());
                journal.reset();
                return Result<void>::Success();
            }

        private:
            [[nodiscard]] bool ShouldFail() {
                ++calls;
                return failCall != 0 && calls == failCall;
            }
        };

        void RequirePublishedRecoveryConverges(SaveSlotCommitTransaction &transaction, FakeCommitStore &store,
                                               const SaveSlotRecoveryAction expected) {
            CHECK(transaction.Recover(Address()).Value() == expected);
            CHECK(transaction.Recover(Address()).Value() == SaveSlotRecoveryAction::None);
            CHECK(store.publishEffects == 1);
            CHECK(store.discardEffects == 0);
        }

        TEST_CASE("Slot commit publishes one complete generation and retains the previous generation", "[unit][save][slot-commit]") {
            FakeCommitStore store;
            store.published = Entry(1);
            SaveSlotCommitTransaction transaction(store);

            const auto result = transaction.Execute(11, Address(), Entry(1), Entry(2), Archive());

            REQUIRE(result.HasValue());
            CHECK_FALSE(result.Value().cleanupDeferred);
            REQUIRE(store.published);
            CHECK(store.published == Entry(2));
            CHECK(store.retainedPrevious);
            CHECK(store.publishEffects == 1);
            CHECK_FALSE(store.journal);
        }

        TEST_CASE("Every pre-publication interruption recovers without changing the visible generation", "[unit][save][slot-commit]") {
            // Calls 1..6 interrupt preflight, journal, prepare, or phase transitions before publication.
            for (std::size_t failCall = 1; failCall <= 6; ++failCall) {
                FakeCommitStore store;
                store.published = Entry(1);
                store.failCall = failCall;
                SaveSlotCommitTransaction transaction(store);

                REQUIRE(transaction.Execute(12, Address(), Entry(1), Entry(2), Archive()).HasError());
                CHECK(store.published == Entry(1));

                store.failCall = 0;
                const auto recovered = transaction.Recover(Address());
                REQUIRE(recovered.HasValue());
                CHECK((recovered.Value() == SaveSlotRecoveryAction::None ||
                       recovered.Value() == SaveSlotRecoveryAction::DiscardedUnpublished));
                CHECK(store.published == Entry(1));
                CHECK_FALSE(store.journal);
                CHECK(transaction.Recover(Address()).Value() == SaveSlotRecoveryAction::None);
                CHECK(store.discardEffects <= 1);
            }
        }

        TEST_CASE("An outcome-unknown publication converges once and replay becomes a no-op", "[unit][save][slot-commit]") {
            FakeCommitStore store;
            store.published = Entry(1);
            store.failCall = 7;  // PublishGeneration fails after the durable Publishing journal.
            SaveSlotCommitTransaction transaction(store);

            const auto attempt = transaction.Execute(13, Address(), Entry(1), Entry(2), Archive());
            REQUIRE(attempt.HasError());
            CHECK(attempt.ErrorValue().code.Value() == SaveErrors::SlotCommitOutcomeUnknown.code.Value());
            CHECK(store.published == Entry(1));

            store.failCall = 0;
            REQUIRE(transaction.Recover(Address()).Value() == SaveSlotRecoveryAction::PublishedCandidate);
            CHECK(store.published == Entry(2));
            CHECK(store.publishEffects == 1);
            CHECK(transaction.Recover(Address()).Value() == SaveSlotRecoveryAction::None);
            CHECK(store.publishEffects == 1);
            CHECK(store.retainedPrevious);
        }

        TEST_CASE("Recovery finalizes a candidate already selected before journal advancement", "[unit][save][slot-commit]") {
            FakeCommitStore store;
            store.published = Entry(1);
            store.failCall = 8;  // Published generation is visible; advancing the journal fails.
            SaveSlotCommitTransaction transaction(store);

            REQUIRE(transaction.Execute(14, Address(), Entry(1), Entry(2), Archive()).HasError());
            REQUIRE(store.journal);
            CHECK(store.journal->phase == SaveSlotCommitPhase::Publishing);
            CHECK(store.published == Entry(2));

            store.failCall = 0;
            RequirePublishedRecoveryConverges(transaction, store, SaveSlotRecoveryAction::PublishedCandidate);
        }

        TEST_CASE("A publish error after replacement is reconciled without repeating the catalog mutation", "[unit][save][slot-commit]") {
            FakeCommitStore store;
            store.published = Entry(1);
            store.failCall = 7;
            store.publishBeforeFailure = true;
            SaveSlotCommitTransaction transaction(store);

            REQUIRE(transaction.Execute(18, Address(), Entry(1), Entry(2), Archive()).HasError());
            CHECK(store.published == Entry(2));
            CHECK(store.publishEffects == 1);

            store.failCall = 0;
            RequirePublishedRecoveryConverges(transaction, store, SaveSlotRecoveryAction::PublishedCandidate);
        }

        TEST_CASE("Cleanup failure cannot turn durable publication into failure or repeat publication", "[unit][save][slot-commit]") {
            FakeCommitStore store;
            store.failCall = 9;  // Journal removal only.
            SaveSlotCommitTransaction transaction(store);

            const auto committed = transaction.Execute(15, Address(), std::nullopt, Entry(2), Archive());
            REQUIRE(committed.HasValue());
            CHECK(committed.Value().cleanupDeferred);
            CHECK(store.published == Entry(2));
            REQUIRE(store.journal);
            CHECK(store.journal->phase == SaveSlotCommitPhase::Published);

            store.failCall = 0;
            RequirePublishedRecoveryConverges(transaction, store, SaveSlotRecoveryAction::FinalizedPublished);
        }

        TEST_CASE("Recovery refuses unrelated or incomplete publication evidence", "[unit][save][slot-commit]") {
            FakeCommitStore unrelated;
            unrelated.journal = SaveSlotCommitJournal{.operation = 16,
                                                      .address = Address(),
                                                      .previous = Entry(1),
                                                      .candidate = Entry(2),
                                                      .phase = SaveSlotCommitPhase::Publishing};
            unrelated.published = Entry(3);
            unrelated.candidatePrepared = true;
            SaveSlotCommitTransaction unrelatedTransaction(unrelated);
            CHECK(unrelatedTransaction.Recover(Address()).HasError());
            CHECK(unrelated.published == Entry(3));
            CHECK(unrelated.publishEffects == 0);

            FakeCommitStore incomplete;
            incomplete.journal = SaveSlotCommitJournal{.operation = 17,
                                                       .address = Address(),
                                                       .previous = Entry(1),
                                                       .candidate = Entry(2),
                                                       .phase = SaveSlotCommitPhase::Publishing};
            incomplete.published = Entry(1);
            SaveSlotCommitTransaction incompleteTransaction(incomplete);
            CHECK(incompleteTransaction.Recover(Address()).HasError());
            CHECK(incomplete.published == Entry(1));
        }
    }  // namespace
}  // namespace Horo::Runtime
