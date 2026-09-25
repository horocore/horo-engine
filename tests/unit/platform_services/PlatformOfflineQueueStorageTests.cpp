#include "Horo/Foundation/Platform.h"
#include "Horo/PlatformServices/PlatformOfflineQueueStorage.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <latch>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
    using namespace Horo::PlatformOfflineQueue;

    const Horo::ErrorCodeDescriptor kInjectedFailure{
        .domain = Horo::ErrorDomainId{"test.platform.offline"},
        .code = Horo::ErrorCode{"injected_failure"},
        .defaultSeverity = Horo::ErrorSeverity::Error,
        .summary = "Injected offline storage failure.",
    };

    struct TemporaryRoot final {
        explicit TemporaryRoot(const bool nonAscii = false) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            std::filesystem::path name =
                nonAscii ? std::filesystem::path{u8"horo-offline-queue-\u00fc"} : std::filesystem::path{"horo-offline-queue"};
            name += "-" + std::to_string(stamp);
            root = std::filesystem::temp_directory_path() / name;
            std::filesystem::create_directories(root);
        }

        ~TemporaryRoot() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }

        std::filesystem::path root;
    };

    [[nodiscard]] PlatformOfflineIntentId Intent(const std::uint8_t value) {
        PlatformOfflineIntentId identity;
        identity.bytes.back() = static_cast<std::byte>(value);
        return identity;
    }

    [[nodiscard]] PlatformOfflineSubjectPartition Partition(const std::uint8_t value) {
        PlatformOfflineSubjectPartition partition;
        partition.bytes.back() = static_cast<std::byte>(value);
        return partition;
    }

    [[nodiscard]] std::vector<std::byte> Bytes(const std::string_view text) {
        std::vector<std::byte> bytes(text.size());
        if (!text.empty())
            std::memcpy(bytes.data(), text.data(), text.size());
        return bytes;
    }

    [[nodiscard]] PlatformOfflineQueueRecord Record(
        const std::uint8_t identity, const std::uint8_t partition, const std::uint64_t sequence, const std::string_view payload,
        const PlatformOfflineOperationKind operation = PlatformOfflineOperationKind::ProgressionMutation) {
        return PlatformOfflineQueueRecord{.identity = Intent(identity),
                                          .partition = Partition(partition),
                                          .state = PlatformOfflineIntentState::Pending,
                                          .operation = operation,
                                          .sequence = sequence,
                                          .payload = Bytes(payload)};
    }

    [[nodiscard]] std::filesystem::path QueueDocument(const std::filesystem::path &root) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.path().extension() == ".horoqueue")
                return entry.path();
        }
        return {};
    }

    class FaultingDurableFileSystem final : public Horo::DurableFileSystem {
    public:
        [[nodiscard]] Horo::Result<Horo::ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &,
                                                                                const std::string_view) override {
            return Horo::Result<Horo::ExclusiveFileLock>::Success(Horo::ExclusiveFileLock{});
        }

        [[nodiscard]] Horo::Result<std::uint64_t> AvailableBytes(const std::filesystem::path &) const override {
            return Horo::Result<std::uint64_t>::Success(1024 * 1024);
        }

        [[nodiscard]] Horo::Result<void> WriteDurable(const std::filesystem::path &, const std::span<const std::byte> bytes) override {
            prepared.assign(bytes.begin(), bytes.end());
            if (failWrite)
                return Horo::Result<void>::Failure(Horo::MakeError(kInjectedFailure));
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
        std::vector<std::byte> prepared;
        std::vector<std::byte> published;
    };

    class BlockingReplaceDurableFileSystem final : public Horo::DurableFileSystem {
    public:
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
                replacing_.count_down();
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

        void WaitUntilReplace() {
            replacing_.wait();
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
        std::latch replacing_{1};
        std::latch continueReplacement_{1};
        std::atomic_size_t replacementAttempts_{};
        std::atomic_bool releaseRequested_{};
    };

    TEST_CASE("Offline queue storage round-trips bounded versioned Horo intents atomically", "[platform-services][offline][storage]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(7);
        const std::vector records{Record(1, 7, 1, "achievement"), Record(2, 7, 2, "score")};

        REQUIRE(store.Publish(partition, records).HasValue());
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
        const auto result = store.Publish(partition, std::span{&unknownOperation, 1});
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

        const auto published = store.Publish(partition, duplicates);
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
        const auto oversizedResult = store.Publish(partition, std::span{&oversized, 1});
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
        const auto capacityResult = store.Publish(partition, tooMany);
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
        REQUIRE(store.Publish(partition, valid).HasValue());
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

        REQUIRE(store.Publish(partition, first).HasValue());
        const auto lastPublished = files.published;
        files.failReplace = true;
        const auto failed = store.Publish(partition, second);
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
            firstResult.emplace(store.Publish(partition, first));
        });

        files.WaitUntilReplace();
        std::thread competingPublisher([&] {
            competingResult.emplace(store.Publish(partition, second));
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
        REQUIRE(store.Publish(partition, second).HasValue());
        CHECK(store.Load(partition).Value() == second);
    }

    TEST_CASE("Offline queue storage preserves non-ASCII native root paths", "[platform-services][offline][storage][paths]") {
        TemporaryRoot temporary{true};
        Horo::NativeDurableFileSystem files;
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(12);
        const std::vector records{Record(1, 12, 1, "unicode-root")};

        REQUIRE(store.Publish(partition, records).HasValue());
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
        const auto publish = store.Publish(Partition(4), std::span{&invalidRecord, 1});
        REQUIRE(publish.HasError());
        CHECK(publish.ErrorValue().code.Value() == "platform.offline.invalid_record");
    }
}  // namespace
