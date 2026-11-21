#include "Horo/Foundation/Platform.h"
#include "Horo/PlatformServices/PlatformOfflineQueueStorage.h"
#include "PlatformOfflineQueueStorageTestSupport.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <latch>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
    using namespace Horo::PlatformOfflineQueue;
    using namespace Horo::PlatformOfflineQueue::Tests::StorageSupport;

    const Horo::ErrorCodeDescriptor kInjectedFailure{
        .domain = Horo::ErrorDomainId{"test.platform.offline"},
        .code = Horo::ErrorCode{"injected_failure"},
        .defaultSeverity = Horo::ErrorSeverity::Error,
        .summary = "Injected offline storage failure.",
    };

    class FaultingDurableFileSystem final : public Horo::DurableFileSystem {
    public:
        Horo::Result<bool> CreateDirectoryPath(const std::filesystem::path &, Horo::DirectoryCreationMode) override {
            return Horo::Result<bool>::Failure(Horo::MakeError(kInjectedFailure));
        }

        [[nodiscard]] Horo::Result<Horo::ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &,
                                                                                const std::string_view) override {
            if (failAcquire)
                return Horo::Result<Horo::ExclusiveFileLock>::Failure(Horo::MakeError(kInjectedFailure, failureDetail));
            return Horo::Result<Horo::ExclusiveFileLock>::Success(Horo::ExclusiveFileLock{});
        }

        [[nodiscard]] Horo::Result<std::uint64_t> AvailableBytes(const std::filesystem::path &) const override {
            if (failAvailable)
                return Horo::Result<std::uint64_t>::Failure(Horo::MakeError(kInjectedFailure, failureDetail));
            return Horo::Result<std::uint64_t>::Success(1024 * 1024);
        }

        [[nodiscard]] Horo::Result<void> WriteDurable(const std::filesystem::path &, const std::span<const std::byte> bytes) override {
            prepared.assign(bytes.begin(), bytes.end());
            if (failWrite)
                return Horo::Result<void>::Failure(
                    Horo::WithCause(Horo::MakeError(kInjectedFailure, failureDetail), Horo::MakeError(kInjectedFailure, failureDetail)));
            return Horo::Result<void>::Success();
        }

        [[nodiscard]] Horo::Result<void> CopyDurable(const std::filesystem::path &, const std::filesystem::path &) override {
            return Horo::Result<void>::Failure(Horo::MakeError(kInjectedFailure));
        }

        [[nodiscard]] Horo::Result<void> AtomicReplace(const std::filesystem::path &, const std::filesystem::path &) override {
            if (failReplace)
                return Horo::Result<void>::Failure(Horo::MakeError(kInjectedFailure));
            published = prepared;
            prepared.clear();
            return Horo::Result<void>::Success();
        }

        [[nodiscard]] Horo::Result<void> RemoveDurable(const std::filesystem::path &) override {
            prepared.clear();
            return Horo::Result<void>::Success();
        }

        [[nodiscard]] Horo::Result<void> SyncDirectory(const std::filesystem::path &) override {
            return Horo::Result<void>::Success();
        }

        bool failWrite{};
        bool failReplace{};
        bool failAcquire{};
        bool failAvailable{};
        std::string failureDetail;
        std::vector<std::byte> prepared;
        std::vector<std::byte> published;
    };

    class BlockingReplaceDurableFileSystem final : public Horo::DurableFileSystem {
    public:
        Horo::Result<bool> CreateDirectoryPath(const std::filesystem::path &path, Horo::DirectoryCreationMode mode) override {
            return native_.CreateDirectoryPath(path, mode);
        }

        [[nodiscard]] Horo::Result<Horo::ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                                                const std::string_view owner) override {
            return native_.TryAcquireExclusive(path, owner);
        }

        [[nodiscard]] Horo::Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native_.AvailableBytes(path);
        }

        [[nodiscard]] Horo::Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            return native_.WriteDurable(path, bytes);
        }

        [[nodiscard]] Horo::Result<void> CopyDurable(const std::filesystem::path &source,
                                                     const std::filesystem::path &destination) override {
            return native_.CopyDurable(source, destination);
        }

        [[nodiscard]] Horo::Result<void> AtomicReplace(const std::filesystem::path &prepared,
                                                       const std::filesystem::path &destination) override {
            const auto attempt = replacementAttempts_.fetch_add(1);
            if (attempt == 0) {
                {
                    std::lock_guard lock(replacementMutex_);
                    replacementReached_ = true;
                }
                replacing_.notify_all();
                continueReplacement_.wait();
            } else if (!releaseRequested_.load()) {
                return Horo::Result<void>::Failure(Horo::MakeError(kInjectedFailure));
            }
            return native_.AtomicReplace(prepared, destination);
        }

        [[nodiscard]] Horo::Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native_.RemoveDurable(path);
        }

        [[nodiscard]] Horo::Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return native_.SyncDirectory(path);
        }

        [[nodiscard]] bool WaitUntilReplace() {
            std::unique_lock lock(replacementMutex_);
            static_cast<void>(replacing_.wait_for(lock, std::chrono::seconds{5}, [this] {
                return replacementReached_ || publicationFinished_;
            }));
            return replacementReached_;
        }

        void NotifyPublicationFinished() {
            {
                std::lock_guard lock(replacementMutex_);
                publicationFinished_ = true;
            }
            replacing_.notify_all();
        }

        void ContinueReplacement() {
            releaseRequested_.store(true);
            continueReplacement_.count_down();
        }

        [[nodiscard]] std::size_t ReplacementAttempts() const noexcept {
            return replacementAttempts_.load();
        }

    private:
        Horo::NativeDurableFileSystem native_;
        // Publisher writes and the owner observes this predicate only under replacementMutex_.
        std::mutex replacementMutex_;
        std::condition_variable replacing_;
        bool replacementReached_{};
        bool publicationFinished_{};
        std::latch continueReplacement_{1};
        std::atomic_size_t replacementAttempts_{};
        std::atomic_bool releaseRequested_{};
    };

    TEST_CASE("Offline quarantine survives restart without blocking another partition", "[platform-services][offline][storage][recovery]") {
        TemporaryRoot temporary(true);
        Horo::NativeDurableFileSystem files;
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const std::vector corruptRecords{Record(1, 7, 1, "private-presence")};
        REQUIRE(Publish(store, Partition(7), corruptRecords).HasValue());
        const auto document = QueueDocument(temporary.root);
        {
            std::ofstream output(document, std::ios::binary | std::ios::trunc);
            output << "corrupt-evidence";
        }
        REQUIRE(store.Quarantine(Partition(7)).HasValue());
        CHECK(store.Quarantine(Partition(7)).HasValue());
        CHECK(std::filesystem::file_size(document) == 16);
        CHECK(Publish(store, Partition(7), {}).HasError());
        const std::vector valid{Record(2, 8, 1, "valid-independent-lane")};
        REQUIRE(Publish(store, Partition(8), valid).HasValue());
        CHECK(store.Load(Partition(8)).Value() == valid);

        auto reopened = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(reopened.HasValue());
        CHECK(reopened.Value().Load(Partition(7)).HasError());
        CHECK(Publish(reopened.Value(), Partition(7), {}).HasError());
        CHECK(reopened.Value().Load(Partition(8)).Value() == valid);
        // A surviving fence must prevent accidental empty-queue recovery even when the source document disappears.
        REQUIRE(files.RemoveDurable(document).HasValue());
        CHECK(reopened.Value().Load(Partition(7)).HasError());
        CHECK(Publish(reopened.Value(), Partition(7), {}).HasError());
        CHECK(store.Quarantine(Partition(8)).HasError());
        CHECK(store.Quarantine(Partition(9)).HasError());
        CHECK(store.Quarantine({}).HasError());
    }

    TEST_CASE("Quarantine reports checkpoint faults without deleting recovery evidence",
              "[platform-services][offline][storage][recovery]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem native;
        auto initial = PlatformOfflineQueueStorage::Create(native, temporary.root);
        REQUIRE(initial.HasValue());
        const std::vector records{Record(1, 7, 1, "canonical-intent")};
        REQUIRE(Publish(initial.Value(), Partition(7), records).HasValue());
        const auto document = QueueDocument(temporary.root);
        {
            std::ofstream output(document, std::ios::binary | std::ios::trunc);
            output << "recovery-evidence";
        }
        FaultingDurableFileSystem faulting;
        auto created = PlatformOfflineQueueStorage::Create(faulting, temporary.root);
        REQUIRE(created.HasValue());
        faulting.failWrite = true;
        const auto writeFailed = created.Value().Quarantine(Partition(7));
        REQUIRE(writeFailed.HasError());
        CHECK(writeFailed.ErrorValue().code.Value() == "injected_failure");
        CHECK(std::filesystem::file_size(document) == 17);
        faulting.failWrite = false;
        faulting.failReplace = true;
        const auto publicationUnknown = created.Value().Quarantine(Partition(7));
        REQUIRE(publicationUnknown.HasError());
        CHECK(publicationUnknown.ErrorValue().code.Value() == "platform.offline.storage_unknown");
        CHECK(std::filesystem::file_size(document) == 17);
        CHECK(faulting.published.empty());
    }

    TEST_CASE("Offline storage errors exclude protected partition paths and native detail",
              "[platform-services][offline][storage][privacy]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem native;
        auto store = PlatformOfflineQueueStorage::Create(native, temporary.root);
        REQUIRE(store.HasValue());
        const std::vector records{Record(1, 7, 1, "canonical")};
        REQUIRE(Publish(store.Value(), Partition(7), records).HasValue());
        const auto document = QueueDocument(temporary.root);
        {
            std::ofstream output(document, std::ios::binary | std::ios::trunc);
            output << "corrupt";
        }
        const auto corrupt = store.Value().Load(Partition(7));
        REQUIRE(corrupt.HasError());
        CHECK(corrupt.ErrorValue().code.Value() == "platform.offline.corrupt");
        CHECK(corrupt.ErrorValue().message.find(temporary.root.generic_string()) == std::string::npos);
        CHECK(corrupt.ErrorValue().message.find(document.filename().string()) == std::string::npos);
        for (const unsigned failure : {0U, 1U, 2U}) {
            FaultingDurableFileSystem faulting;
            faulting.failureDetail = document.generic_string() + " private-native-cause";
            faulting.failWrite = failure == 0;
            faulting.failAcquire = failure == 1;
            faulting.failAvailable = failure == 2;
            auto failing = PlatformOfflineQueueStorage::Create(faulting, temporary.root);
            REQUIRE(failing.HasValue());
            const auto published = Publish(failing.Value(), Partition(8), std::vector{Record(2, 8, 1, "canonical")});
            REQUIRE(published.HasError());
            CHECK(published.ErrorValue().code.Value() == "injected_failure");
            CHECK(published.ErrorValue().domain.Value() == kInjectedFailure.domain.Value());
            CHECK(published.ErrorValue().severity == kInjectedFailure.defaultSeverity);
            CHECK(published.ErrorValue().message.find(document.generic_string()) == std::string::npos);
            CHECK(published.ErrorValue().message.find("private-native-cause") == std::string::npos);
            CHECK(published.ErrorValue().cause.Get() == nullptr);
            CHECK(published.ErrorValue().diagnostics.empty());
        }
    }

    TEST_CASE("Offline queue storage round-trips bounded versioned Horo intents atomically", "[platform-services][offline][storage]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(7);
        const std::vector records{Record(1, 7, 1, "achievement"), Record(2, 7, 2, "score")};

        REQUIRE(Publish(store, partition, records).HasValue());
        const auto loaded = store.Load(partition);
        REQUIRE(loaded.HasValue());
        CHECK(loaded.Value() == records);

        const auto emptyPartition = store.Load(Partition(8));
        REQUIRE(emptyPartition.HasValue());
        CHECK(emptyPartition.Value().empty());
    }

    TEST_CASE("Offline queue storage rejects unknown operation values", "[platform-services][offline][storage][validation]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        const auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(4);
        const auto unknownOperation = Record(1, 4, 1, "opaque-canonical-payload", static_cast<PlatformOfflineOperationKind>(3));
        const auto result = Publish(store, partition, std::span{&unknownOperation, 1});
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "platform.offline.invalid_record");
    }

    TEST_CASE("Offline queue storage rejects duplicate intent identities", "[platform-services][offline][storage][validation]") {
        FaultingDurableFileSystem files;
        const auto created = PlatformOfflineQueueStorage::Create(files, "/virtual/duplicate-identities");
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(5);
        const std::vector duplicates{Record(1, 5, 1, "first"), Record(1, 5, 2, "second")};

        const auto published = Publish(store, partition, duplicates);
        REQUIRE(published.HasError());
        CHECK(published.ErrorValue().code.Value() == "platform.offline.identity_conflict");
    }

    TEST_CASE("Offline queue storage rejects payloads over the configured bound", "[platform-services][offline][storage][validation]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        const PlatformOfflineQueueLimits limits{.maximumRecords = 2, .maximumPayloadBytes = 4};
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root, limits);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(3);

        const auto oversized = Record(1, 3, 1, "12345");
        const auto oversizedResult = Publish(store, partition, std::span{&oversized, 1});
        REQUIRE(oversizedResult.HasError());
        CHECK(oversizedResult.ErrorValue().code.Value() == "platform.offline.payload_too_large");
    }

    TEST_CASE("Offline queue storage rejects record counts over the configured bound",
              "[platform-services][offline][storage][validation]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        const PlatformOfflineQueueLimits limits{.maximumRecords = 2};
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root, limits);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(3);
        const std::vector tooMany{Record(1, 3, 1, "a"), Record(2, 3, 2, "b"), Record(3, 3, 3, "c")};
        const auto capacityResult = Publish(store, partition, tooMany);
        REQUIRE(capacityResult.HasError());
        CHECK(capacityResult.ErrorValue().code.Value() == "platform.offline.capacity_exceeded");
    }

    TEST_CASE("Offline queue storage rejects malformed documents", "[platform-services][offline][storage][validation]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        const PlatformOfflineQueueLimits limits{.maximumRecords = 2, .maximumPayloadBytes = 4, .maximumDocumentBytes = 128};
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root, limits);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(3);
        const std::vector valid{Record(1, 3, 1, "ok")};
        REQUIRE(Publish(store, partition, valid).HasValue());
        const auto path = QueueDocument(temporary.root);
        REQUIRE_FALSE(path.empty());

        SECTION("unsupported document schema") {
            std::fstream document(path, std::ios::binary | std::ios::in | std::ios::out);
            REQUIRE(document.good());
            document.seekp(8);
            document.put('\0');
            document.put('\0');
            document.put('\0');
            document.put('\2');
            document.close();

            const auto unsupported = store.Load(partition);
            REQUIRE(unsupported.HasError());
            CHECK(unsupported.ErrorValue().code.Value() == "platform.offline.version_unsupported");
        }

        SECTION("digest mismatch") {
            std::fstream document(path, std::ios::binary | std::ios::in | std::ios::out);
            REQUIRE(document.good());
            document.seekp(-1, std::ios::end);
            const char corrupted = 'x';
            document.write(&corrupted, 1);
            document.close();

            const auto corrupt = store.Load(partition);
            REQUIRE(corrupt.HasError());
            CHECK(corrupt.ErrorValue().code.Value() == "platform.offline.corrupt");
        }

        SECTION("oversized document") {
            std::ofstream document(path, std::ios::binary | std::ios::app);
            REQUIRE(document.good());
            document.write("overflow-overflow-overflow-overflow", 35);
            document.close();

            const auto oversizedDocument = store.Load(partition);
            REQUIRE(oversizedDocument.HasError());
            CHECK(oversizedDocument.ErrorValue().code.Value() == "platform.offline.capacity_exceeded");
        }
    }

    TEST_CASE("Offline queue storage keeps the last publication when atomic replacement fails",
              "[platform-services][offline][storage][atomic]") {
        FaultingDurableFileSystem files;
        const auto created = PlatformOfflineQueueStorage::Create(files, "/virtual/state");
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(9);
        const std::vector first{Record(1, 9, 1, "first")};
        const std::vector second{Record(2, 9, 2, "second")};

        REQUIRE(Publish(store, partition, first).HasValue());
        const auto lastPublished = files.published;
        files.failReplace = true;
        const auto failed = Publish(store, partition, second);
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == "platform.offline.storage_unknown");
        CHECK(files.published == lastPublished);
        CHECK_FALSE(files.prepared.empty());
    }

    TEST_CASE("Offline queue storage serializes concurrent publications", "[platform-services][offline][storage][atomic]") {
        TemporaryRoot temporary;
        BlockingReplaceDurableFileSystem files;
        const auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(11);
        const std::vector first{Record(1, 11, 1, "first-publication")};
        const std::vector second{Record(2, 11, 2, "second-publication")};
        std::optional<Horo::Result<void>> firstResult;
        std::optional<Horo::Result<void>> competingResult;
        std::thread firstPublisher([&] {
            firstResult.emplace(Publish(store, partition, first));
            files.NotifyPublicationFinished();
        });

        if (const bool reachedReplacement = files.WaitUntilReplace(); !reachedReplacement) {
            files.ContinueReplacement();
            firstPublisher.join();
            REQUIRE(firstResult.has_value());
            if (firstResult->HasError()) {
                const auto &error = firstResult->ErrorValue();
                INFO(error.domain.Value() << "/" << error.code.Value() << ": " << error.message);
            }
            REQUIRE(reachedReplacement);
        }
        std::thread competingPublisher([&] {
            competingResult.emplace(Publish(store, partition, second));
        });
        competingPublisher.join();
        const bool onlyFirstReachedReplacement = files.ReplacementAttempts() == 1;
        files.ContinueReplacement();
        firstPublisher.join();

        REQUIRE(firstResult.has_value());
        REQUIRE(firstResult->HasValue());
        REQUIRE(competingResult.has_value());
        CHECK(competingResult->HasError());
        CHECK(onlyFirstReachedReplacement);
        const auto firstSnapshot = store.Load(partition);
        REQUIRE(firstSnapshot.HasValue());
        CHECK(firstSnapshot.Value() == first);
        REQUIRE(Publish(store, partition, second).HasValue());
        CHECK(store.Load(partition).Value() == second);
    }

    TEST_CASE("Offline queue storage concurrency fixture wakes after an early publication failure",
              "[platform-services][offline][storage][atomic]") {
        TemporaryRoot temporary;
        BlockingReplaceDurableFileSystem files;
        const auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto invalid = Record(0, 11, 1, "invalid-identity");
        std::optional<Horo::Result<void>> result;
        std::thread publisher([&] {
            result.emplace(Publish(store, Partition(11), std::span{&invalid, 1}));
            files.NotifyPublicationFinished();
        });
        const bool reachedReplacement = files.WaitUntilReplace();
        files.ContinueReplacement();
        publisher.join();

        CHECK_FALSE(reachedReplacement);
        REQUIRE(result.has_value());
        REQUIRE(result->HasError());
        CHECK(result->ErrorValue().code.Value() == "platform.offline.invalid_record");
        CHECK(files.ReplacementAttempts() == 0);
    }

    TEST_CASE("Offline queue storage preserves non-ASCII native root paths", "[platform-services][offline][storage][paths]") {
        TemporaryRoot temporary{true};
        Horo::NativeDurableFileSystem files;
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(12);
        const std::vector records{Record(1, 12, 1, "unicode-root")};

        REQUIRE(Publish(store, partition, records).HasValue());
        CHECK(store.Load(partition).Value() == records);
    }

    TEST_CASE("Offline queue storage rejects invalid partitions records and configuration",
              "[platform-services][offline][storage][validation]") {
        Horo::NativeDurableFileSystem files;
        const auto invalidStore = PlatformOfflineQueueStorage::Create(files, {});
        REQUIRE(invalidStore.HasError());
        CHECK(invalidStore.ErrorValue().code.Value() == "platform.offline.invalid_configuration");

        TemporaryRoot temporary;
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        PlatformOfflineSubjectPartition invalidPartition;
        REQUIRE(store.Load(invalidPartition).HasError());

        PlatformOfflineQueueRecord invalidRecord;
        invalidRecord.partition = Partition(4);
        const auto publish = Publish(store, Partition(4), std::span{&invalidRecord, 1});
        REQUIRE(publish.HasError());
        CHECK(publish.ErrorValue().code.Value() == "platform.offline.invalid_record");
    }
}  // namespace
