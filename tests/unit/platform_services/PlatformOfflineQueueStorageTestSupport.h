#pragma once

/** @file PlatformOfflineQueueStorageTestSupport.h
 * @brief Shared bounded fixtures for native offline storage and revision regression cases.
 */

#include "Horo/PlatformServices/PlatformOfflineQueueStorage.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Horo::PlatformOfflineQueue::Tests::StorageSupport {
    struct TemporaryRoot final {
        explicit TemporaryRoot(const bool nonAscii = false) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            std::filesystem::path name =
                nonAscii ? std::filesystem::path{u8"horo-offline-queue-\u00fc"} : std::filesystem::path{"horo-offline-queue"};
            name += "-" + std::to_string(stamp);
            root = std::filesystem::temp_directory_path() / name;
            std::filesystem::create_directories(root);
            // Native publication authority requires canonical spelling, including macOS /var aliases.
            root = std::filesystem::canonical(root);
        }

        ~TemporaryRoot() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }

        std::filesystem::path root;
    };

    [[nodiscard]] inline PlatformOfflineIntentId Intent(const std::uint8_t value) {
        PlatformOfflineIntentId identity;
        identity.bytes.back() = static_cast<std::byte>(value);
        return identity;
    }

    [[nodiscard]] inline PlatformOfflineSubjectPartition Partition(const std::uint8_t value) {
        PlatformOfflineSubjectPartition partition;
        partition.bytes.back() = static_cast<std::byte>(value);
        return partition;
    }

    [[nodiscard]] inline std::vector<std::byte> Bytes(const std::string_view text) {
        std::vector<std::byte> bytes(text.size());
        std::ranges::copy(std::as_bytes(std::span{text.data(), text.size()}), bytes.begin());
        return bytes;
    }

    [[nodiscard]] inline PlatformOfflineQueueRecord Record(
        const std::uint8_t identity, const std::uint8_t partition, const std::uint64_t sequence, const std::string_view payload,
        const PlatformOfflineOperationKind operation = PlatformOfflineOperationKind::ProgressionMutation) {
        return PlatformOfflineQueueRecord{.identity = Intent(identity),
                                          .partition = Partition(partition),
                                          .state = PlatformOfflineIntentState::Pending,
                                          .operation = operation,
                                          .sequence = sequence,
                                          .payload = Bytes(payload)};
    }

    [[nodiscard]] inline std::filesystem::path QueueDocument(const std::filesystem::path &root) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.path().extension() == ".horoqueue")
                return entry.path();
        }
        return {};
    }

    /** @brief Publishes an explicit test replacement based on a freshly validated snapshot; stale-owner cases use tokens directly. */
    [[nodiscard]] inline Horo::Result<void> Publish(const PlatformOfflineQueueStorage &store,
                                                    const PlatformOfflineSubjectPartition &partition,
                                                    const std::span<const PlatformOfflineQueueRecord> records) {
        const auto snapshot = store.LoadSnapshot(partition);
        if (snapshot.HasError())
            return Horo::Result<void>::Failure(snapshot.ErrorValue());
        return store.Publish(partition, records, snapshot.Value().revision);
    }

}  // namespace Horo::PlatformOfflineQueue::Tests::StorageSupport
