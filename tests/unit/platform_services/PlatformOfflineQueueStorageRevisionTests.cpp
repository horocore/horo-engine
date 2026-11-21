#include "Horo/Foundation/Platform.h"
#include "PlatformOfflineQueueStorageTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <vector>

namespace {
    using namespace Horo::PlatformOfflineQueue;
    using namespace Horo::PlatformOfflineQueue::Tests::StorageSupport;

    TEST_CASE("Stale offline snapshots cannot overwrite another owner publication",
              "[platform-services][offline][storage][atomic][recovery]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        auto first = PlatformOfflineQueueStorage::Create(files, temporary.root);
        auto second = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        const auto partition = Partition(7);
        const auto firstRead = first.Value().LoadSnapshot(partition);
        const auto secondRead = second.Value().LoadSnapshot(partition);
        REQUIRE(firstRead.HasValue());
        REQUIRE(secondRead.HasValue());
        const std::vector firstRows{Record(1, 7, 1, "first-owner-intent")};
        const std::vector secondRows{Record(2, 7, 2, "second-owner-intent")};
        REQUIRE(first.Value().Publish(partition, firstRows, firstRead.Value().revision).HasValue());
        const auto stale = second.Value().Publish(partition, secondRows, secondRead.Value().revision);
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == "platform.offline.storage_changed");
        CHECK_FALSE(PlatformOfflineQueueErrors::RevisionConflict.retryable);
        CHECK(second.Value().Load(partition).Value() == firstRows);
        auto fresh = second.Value().LoadSnapshot(partition);
        REQUIRE(fresh.HasValue());
        auto merged = fresh.Value().records;
        merged.insert(merged.end(), secondRows.begin(), secondRows.end());
        REQUIRE(second.Value().Publish(partition, merged, fresh.Value().revision).HasValue());
        CHECK(first.Value().Load(partition).Value() == merged);
        CHECK(first.Value().Publish(partition, {}, firstRead.Value().revision).HasError());
        CHECK(first.Value().Load(partition).Value() == merged);
    }

    TEST_CASE("Offline publication tokens cannot cross adapters partitions or lifetimes",
              "[platform-services][offline][storage][validation]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        PlatformOfflineQueueRevision expired;
        {
            auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
            REQUIRE(created.HasValue());
            auto store = std::move(created).Value();
            const auto snapshot = store.LoadSnapshot(Partition(7));
            REQUIRE(snapshot.HasValue());
            expired = snapshot.Value().revision;
            CHECK(store.Publish(Partition(7), {}, {}).HasError());
            CHECK(store.Publish(Partition(8), {}, snapshot.Value().revision).HasError());
            auto foreign = PlatformOfflineQueueStorage::Create(files, temporary.root);
            REQUIRE(foreign.HasValue());
            CHECK(foreign.Value().Publish(Partition(7), {}, snapshot.Value().revision).HasError());
            auto moved = std::move(store);
            CHECK(store.LoadSnapshot(Partition(7)).HasError());
            CHECK(store.Quarantine(Partition(7)).HasError());
            CHECK(store.Publish(Partition(7), {}, snapshot.Value().revision).HasError());
            REQUIRE(moved.Publish(Partition(7), {}, snapshot.Value().revision).HasValue());
        }
        auto reopened = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(reopened.HasValue());
        const auto rejected = reopened.Value().Publish(Partition(7), {}, expired);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "platform.offline.storage_changed");
        CHECK(reopened.Value().Load(Partition(7)).Value().empty());
    }

    TEST_CASE("Copied offline adapters share publication provenance without bypassing conflict checks",
              "[platform-services][offline][storage][atomic]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto original = std::move(created).Value();
        auto copy = original;
        const auto partition = Partition(7);
        const auto absent = original.LoadSnapshot(partition);
        REQUIRE(absent.HasValue());
        const std::vector firstRows{Record(1, 7, 1, "copied-owner-intent")};
        REQUIRE(copy.Publish(partition, firstRows, absent.Value().revision).HasValue());
        const auto stale = original.Publish(partition, {}, absent.Value().revision);
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == "platform.offline.storage_changed");
        CHECK(original.Load(partition).Value() == firstRows);
        const auto fresh = copy.LoadSnapshot(partition);
        REQUIRE(fresh.HasValue());
        REQUIRE(original.Publish(partition, {}, fresh.Value().revision).HasValue());
        CHECK(copy.Load(partition).Value().empty());
    }

    TEST_CASE("Offline publication revisions distinguish absence from an empty corrupt document",
              "[platform-services][offline][storage][recovery]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(7);
        const auto absent = store.LoadSnapshot(partition);
        REQUIRE(absent.HasValue());
        const std::vector rows{Record(1, 7, 1, "original-intent")};
        REQUIRE(store.Publish(partition, rows, absent.Value().revision).HasValue());
        const auto document = QueueDocument(temporary.root);
        {
            std::ofstream output(document, std::ios::binary | std::ios::trunc);
            REQUIRE(output.good());
        }
        const auto rejected = store.Publish(partition, rows, absent.Value().revision);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "platform.offline.storage_changed");
        CHECK(std::filesystem::exists(document));
        CHECK(std::filesystem::file_size(document) == 0);
        CHECK(store.LoadSnapshot(partition).HasError());
    }

    TEST_CASE("A retained offline revision cannot resurrect a removed partition document",
              "[platform-services][offline][storage][recovery]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const auto partition = Partition(7);
        const std::vector rows{Record(1, 7, 1, "original-intent")};
        REQUIRE(Publish(store, partition, rows).HasValue());
        const auto present = store.LoadSnapshot(partition);
        REQUIRE(present.HasValue());
        const auto document = QueueDocument(temporary.root);
        REQUIRE(std::filesystem::remove(document));
        const auto rejected = store.Publish(partition, rows, present.Value().revision);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "platform.offline.storage_changed");
        CHECK_FALSE(std::filesystem::exists(document));
    }

    TEST_CASE("A stale publication token cannot replace newly corrupt recovery evidence",
              "[platform-services][offline][storage][recovery]") {
        TemporaryRoot temporary;
        Horo::NativeDurableFileSystem files;
        auto created = PlatformOfflineQueueStorage::Create(files, temporary.root);
        REQUIRE(created.HasValue());
        auto store = std::move(created).Value();
        const std::vector rows{Record(1, 7, 1, "original-intent")};
        REQUIRE(Publish(store, Partition(7), rows).HasValue());
        const auto snapshot = store.LoadSnapshot(Partition(7));
        REQUIRE(snapshot.HasValue());
        const auto document = QueueDocument(temporary.root);
        {
            std::ofstream output(document, std::ios::binary | std::ios::trunc);
            output << "preserve-corrupt-evidence";
        }
        CHECK(store.LoadSnapshot(Partition(7)).HasError());
        const auto rejected = store.Publish(Partition(7), {}, snapshot.Value().revision);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "platform.offline.storage_changed");
        CHECK(std::filesystem::file_size(document) == 25);
        REQUIRE(store.Quarantine(Partition(7)).HasValue());
        CHECK(store.Publish(Partition(7), {}, snapshot.Value().revision).HasError());
    }

}  // namespace
