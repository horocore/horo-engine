#pragma once

/**
 * @file StructuredLogStore.h
 * @brief Bounded thread-safe in-memory query store for structured log records.
 */

#include "Horo/Foundation/Logging/LogLevel.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Log {
    /** @brief Immutable log data retained for local presentation consumers. */
    struct StructuredLogRecord {
        std::uint64_t sequence{};
        std::chrono::system_clock::time_point timestampUtc;
        Level level{Level::Info};
        std::string category;
        std::string message;
        std::string context;
        std::vector<Telemetry::Field> fields; /**< Record-local metadata redacted by the producer before dispatch. */
    };

    /** @brief Owned bounded snapshot returned to slow presentation consumers. */
    struct StructuredLogSnapshot {
        std::uint64_t revision{};
        std::uint64_t droppedRecordCount{};
        std::size_t capacity{};
        std::vector<std::shared_ptr<const StructuredLogRecord>> records;
    };

    /** @brief Read-only capability exposed to log presentation surfaces. */
    class IStructuredLogQuery {
    public:
        virtual ~IStructuredLogQuery() = default;

        /**
         * @brief Copies the current bounded store only when its revision changed.
         * @param knownRevision Last revision already held by the caller.
         * @return A new owned snapshot, or no value when the caller is current.
         */
        [[nodiscard]] virtual std::optional<StructuredLogSnapshot> SnapshotIfChanged(std::uint64_t knownRevision) const = 0;
    };

    /** @brief Fixed-capacity in-memory sink with overwrite-oldest retention. */
    class StructuredLogStore final : public IStructuredLogQuery {
    public:
        /**
         * @brief Creates an empty store.
         * @param capacity Maximum retained record count; values below one are clamped to one.
         */
        explicit StructuredLogStore(std::size_t capacity);

        /**
         * @brief Appends one immutable record and evicts the oldest record when full.
         * @param record Record owned by the store after the call.
         */
        void Append(StructuredLogRecord record);

        /** @copydoc IStructuredLogQuery::SnapshotIfChanged */
        [[nodiscard]] std::optional<StructuredLogSnapshot> SnapshotIfChanged(std::uint64_t knownRevision) const override;

    private:
        const std::size_t capacity_;
        mutable std::mutex mutex_;
        std::deque<std::shared_ptr<const StructuredLogRecord>> records_;
        std::uint64_t revision_{};
        std::uint64_t droppedRecordCount_{};
    };
}  // namespace Horo::Log
