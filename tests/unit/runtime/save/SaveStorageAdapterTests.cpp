#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveStorageAdapter.h"
#include "SaveTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using namespace Test;

        [[nodiscard]] SaveNamespaceId Namespace() {
            return {.product = Id<ProductStorageId>(1),
                    .environment = Id<EnvironmentStorageId>(2),
                    .owner = ServerWorldOwner{Id<ServerStorageOwnerId>(3)}};
        }

        [[nodiscard]] SaveStorageAddress Address(const std::uint8_t slot) {
            return {.namespaceAccess = {.expected = Namespace(), .expectedRevision = 7}, .slot = Id<SaveGameSlotId>(slot)};
        }

        [[nodiscard]] SaveSlotCatalogEntry Entry(const std::uint8_t slot) {
            Sha256Digest digest{};
            digest.bytes.back() = slot;
            return {.publication = {.slot = Id<SaveGameSlotId>(slot),
                                    .generation = Id<SlotGenerationId>(static_cast<std::uint8_t>(slot + 10)),
                                    .savedAtUnixMilliseconds = 1,
                                    .playTimeNanoseconds = 1,
                                    .baseScene = Id<SaveBaseSceneId>(4),
                                    .productCompatibility = V<ProductSaveCompatibilityVersion>(1),
                                    .saveSchema = V<SaveSchemaVersion>(1),
                                    .projectBuildId = "test",
                                    .canonicalState = {.value = digest},
                                    .archiveContent = {.value = digest}},
                    .display = {.displayName = "Slot"}};
        }

        class RecordingProvider final : public ISaveStorageProvider {
        public:
            SaveStorageCapabilities capabilities = SaveStorageCapabilities::All();
            std::optional<Error> failure;
            std::optional<SaveStorageValue> resultOverride;
            bool throwAllocationFailure{};

            [[nodiscard]] SaveStorageCapabilities Capabilities() const noexcept override {
                return capabilities;
            }

            [[nodiscard]] Result<SaveStorageValue> Execute(const SaveStorageRequest &request,
                                                           const CancellationToken &cancellation) override {
                std::lock_guard lock(mutex_);
                ++calls_;
                lastKind_ = request.kind;
                if (throwAllocationFailure)
                    throw std::bad_alloc{};
                if (cancellation.IsCancellationRequested())
                    return Result<SaveStorageValue>::Failure(MakeError(SaveErrors::OperationCancelled));
                if (failure)
                    return Result<SaveStorageValue>::Failure(*failure);
                if (resultOverride)
                    return Result<SaveStorageValue>::Success(*resultOverride);
                return SuccessfulValue(request);
            }

            [[nodiscard]] std::size_t Calls() const {
                std::lock_guard lock(mutex_);
                return calls_;
            }

        private:
            [[nodiscard]] static Result<SaveStorageValue> SuccessfulValue(const SaveStorageRequest &request) {
                switch (request.kind) {
                    case SaveStorageOperationKind::List:
                        return Result<SaveStorageValue>::Success(
                            ImmutableSaveSlotList{std::make_shared<const std::vector<SaveSlotCatalogEntry>>(
                                std::vector<SaveSlotCatalogEntry>{Entry(1), Entry(2)})});
                    case SaveStorageOperationKind::ReadMetadata:
                        return Result<SaveStorageValue>::Success(
                            ImmutableSaveSlotMetadata{std::make_shared<const SaveSlotCatalogEntry>(Entry(1))});
                    case SaveStorageOperationKind::ReadArchive:
                        return Result<SaveStorageValue>::Success(ImmutableSaveArchive{
                            std::make_shared<const std::vector<std::byte>>(std::vector<std::byte>{std::byte{1}, std::byte{2}})});
                    case SaveStorageOperationKind::Exists:
                        return Result<SaveStorageValue>::Success(true);
                    case SaveStorageOperationKind::Write:
                    case SaveStorageOperationKind::Copy:
                    case SaveStorageOperationKind::Rename:
                    case SaveStorageOperationKind::Delete:
                        return Result<SaveStorageValue>::Success(std::monostate{});
                    case SaveStorageOperationKind::Count:
                        break;
                }
                return Result<SaveStorageValue>::Failure(MakeError(SaveErrors::StorageResultInvalid));
            }

            mutable std::mutex mutex_;
            std::size_t calls_{};
            SaveStorageOperationKind lastKind_{SaveStorageOperationKind::List};
        };

        [[nodiscard]] SaveOperationSnapshot Await(const SaveStorageOperation &operation) {
            for (std::size_t attempt = 0; attempt < 2'000; ++attempt) {
                const auto snapshot = operation.Snapshot();
                REQUIRE(snapshot);
                if (snapshot->IsTerminal())
                    return *snapshot;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            FAIL("storage operation did not complete");
        }

        [[nodiscard]] SaveStorageRequest Request(const SaveStorageOperationKind kind) {
            SaveStorageRequest request{.kind = kind, .source = Address(1)};
            if (kind == SaveStorageOperationKind::Copy || kind == SaveStorageOperationKind::Rename)
                request.destination = Address(2);
            if (kind == SaveStorageOperationKind::Write) {
                request.write =
                    SaveStorageWrite{.metadata = Entry(1),
                                     .archive = {std::make_shared<const std::vector<std::byte>>(std::vector<std::byte>{std::byte{1}})}};
            }
            return request;
        }

        void CheckInvalidProviderResult(SaveStorageAdapter &adapter, const OperationId operationId, SaveStorageRequest request) {
            auto operation = adapter.Submit(operationId, std::move(request)).Value();
            const auto terminal = Await(operation);
            REQUIRE(terminal.terminalError);
            CHECK(terminal.terminalError->code.Value() == SaveErrors::StorageResultInvalid.code.Value());
            CHECK(terminal.commit == SaveOperationCommitOutcome::NotCommitted);
            CHECK_FALSE(operation.Value());
        }

        TEST_CASE("Every accepted local slot operation completes exactly once with its typed immutable result",
                  "[unit][runtime][save][storage]") {
            JobSystem jobs({.workerCount = 2});
            auto provider = std::make_shared<RecordingProvider>();
            SaveStorageAdapter adapter(jobs, provider);
            OperationId id = 100;
            for (const SaveStorageOperationKind kind :
                 {SaveStorageOperationKind::List, SaveStorageOperationKind::ReadMetadata, SaveStorageOperationKind::ReadArchive,
                  SaveStorageOperationKind::Write, SaveStorageOperationKind::Exists, SaveStorageOperationKind::Copy,
                  SaveStorageOperationKind::Rename, SaveStorageOperationKind::Delete}) {
                auto admitted = adapter.Submit(id++, Request(kind));
                REQUIRE(admitted.HasValue());
                const SaveStorageOperation operation = std::move(admitted).Value();
                const SaveOperationSnapshot terminal = Await(operation);
                CHECK(terminal.state == SaveOperationState::Completed);
                CHECK(terminal.commit == (kind == SaveStorageOperationKind::List || kind == SaveStorageOperationKind::ReadMetadata ||
                                                  kind == SaveStorageOperationKind::ReadArchive || kind == SaveStorageOperationKind::Exists
                                              ? SaveOperationCommitOutcome::NotCommitted
                                              : SaveOperationCommitOutcome::Committed));
                REQUIRE(operation.Value());
            }
            CHECK(provider->Calls() == 8);
        }

        TEST_CASE("Malformed and unsupported requests are rejected before provider execution", "[unit][runtime][save][storage]") {
            JobSystem jobs;
            auto provider = std::make_shared<RecordingProvider>();
            provider->capabilities = {};
            SaveStorageAdapter adapter(jobs, provider);

            auto unsupported = adapter.Submit(1, Request(SaveStorageOperationKind::ReadArchive));
            REQUIRE(unsupported.HasError());
            CHECK(unsupported.ErrorValue().code.Value() == SaveErrors::StorageCapabilityUnsupported.code.Value());

            auto malformed = Request(SaveStorageOperationKind::Copy);
            malformed.destination = malformed.source;
            REQUIRE(adapter.Submit(2, std::move(malformed)).ErrorValue().code.Value() == SaveErrors::StorageOperationInvalid.code.Value());
            CHECK(provider->Calls() == 0);
        }

        TEST_CASE("Provider failures preserve cause and conservatively qualify mutation publication", "[unit][runtime][save][storage]") {
            JobSystem jobs;
            auto provider = std::make_shared<RecordingProvider>();
            provider->failure = MakeError(SaveErrors::SaveRootUnavailable);
            SaveStorageAdapter adapter(jobs, provider);

            auto read = adapter.Submit(1, Request(SaveStorageOperationKind::ReadArchive)).Value();
            const auto readFailure = Await(read);
            REQUIRE(readFailure.terminalError);
            CHECK(readFailure.terminalError->code.Value() == SaveErrors::SaveRootUnavailable.code.Value());
            CHECK(readFailure.commit == SaveOperationCommitOutcome::NotCommitted);
            CHECK_FALSE(read.Value());

            auto write = adapter.Submit(2, Request(SaveStorageOperationKind::Write)).Value();
            const auto writeFailure = Await(write);
            REQUIRE(writeFailure.terminalError);
            CHECK(writeFailure.terminalError->code.Value() == SaveErrors::SaveRootUnavailable.code.Value());
            CHECK(writeFailure.commit == SaveOperationCommitOutcome::Unknown);
        }

        TEST_CASE("Provider result shape is validated before publication", "[unit][runtime][save][storage]") {
            class ContradictoryProvider final : public ISaveStorageProvider {
            public:
                [[nodiscard]] SaveStorageCapabilities Capabilities() const noexcept override {
                    return SaveStorageCapabilities::All();
                }

                [[nodiscard]] Result<SaveStorageValue> Execute(const SaveStorageRequest &, const CancellationToken &) override {
                    return Result<SaveStorageValue>::Success(std::monostate{});
                }
            };

            JobSystem jobs;
            SaveStorageAdapter adapter(jobs, std::make_shared<ContradictoryProvider>());
            CheckInvalidProviderResult(adapter, 1, Request(SaveStorageOperationKind::ReadArchive));
        }

        TEST_CASE("Provider list results require stable ordering and unique slot identities", "[unit][runtime][save][storage]") {
            JobSystem jobs;
            auto provider = std::make_shared<RecordingProvider>();
            SaveStorageAdapter adapter(jobs, provider);

            for (const auto entries :
                 {std::vector<SaveSlotCatalogEntry>{Entry(2), Entry(1)}, std::vector<SaveSlotCatalogEntry>{Entry(1), Entry(1)}}) {
                provider->resultOverride = ImmutableSaveSlotList{std::make_shared<const std::vector<SaveSlotCatalogEntry>>(entries)};
                CheckInvalidProviderResult(adapter, 1, Request(SaveStorageOperationKind::List));
            }
        }

        TEST_CASE("Provider archive results cannot exceed the configured byte bound", "[unit][runtime][save][storage]") {
            JobSystem jobs;
            auto provider = std::make_shared<RecordingProvider>();
            provider->resultOverride =
                ImmutableSaveArchive{std::make_shared<const std::vector<std::byte>>(std::vector<std::byte>{std::byte{1}, std::byte{2}})};
            SaveStorageAdapter adapter(jobs, provider, {.maximumArchiveBytes = 1, .maximumListedSlots = 1});

            CheckInvalidProviderResult(adapter, 1, Request(SaveStorageOperationKind::ReadArchive));
        }

        TEST_CASE("Provider allocation failures become stable storage allocation errors", "[unit][runtime][save][storage]") {
            JobSystem jobs;
            auto provider = std::make_shared<RecordingProvider>();
            provider->throwAllocationFailure = true;
            SaveStorageAdapter adapter(jobs, provider);
            auto operation = adapter.Submit(1, Request(SaveStorageOperationKind::ReadArchive)).Value();
            const auto terminal = Await(operation);
            REQUIRE(terminal.terminalError);
            CHECK(terminal.terminalError->code.Value() == SaveErrors::StorageAllocationFailed.code.Value());
            CHECK(terminal.commit == SaveOperationCommitOutcome::NotCommitted);
        }

        TEST_CASE("An accepted operation observes its deadline before provider dispatch", "[unit][runtime][save][storage]") {
            JobSystem jobs;
            auto provider = std::make_shared<RecordingProvider>();
            SaveStorageAdapter adapter(jobs, provider);
            auto operation = adapter
                                 .Submit(1, Request(SaveStorageOperationKind::ReadArchive), {},
                                         std::chrono::steady_clock::now() - std::chrono::milliseconds(1))
                                 .Value();

            const auto terminal = Await(operation);
            CHECK(terminal.state == SaveOperationState::Cancelled);
            CHECK(terminal.cancellationReason == SaveCancellationReason::Deadline);
            CHECK(terminal.commit == SaveOperationCommitOutcome::NotCommitted);
            CHECK(provider->Calls() == 0);
            CHECK_FALSE(operation.Value());
        }
    }  // namespace
}  // namespace Horo::Runtime
