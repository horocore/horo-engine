#include "ConsoleText.h"
#include "Horo/Debug/CommandRegistry.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>

namespace Horo::Debug {
    namespace {
        /** @brief Borrowed candidate for one pending row; the caller owns both immutable source and current history storage. */
        struct HistoryValue final {
            OperationState state;
            std::string_view output;
            bool unavailable{};
        };

        /** @brief Selects safe bounded output without allocating or changing the row.
         * @param entry Current pending row. @param record Exact matching source record, or null when retention expired.
         * @param bytes Remaining per-row/aggregate budget at this position in history order.
         * @return Borrowed candidate; UTF-8 validity is checked before publication.
         */
        HistoryValue ProjectValue(const HistoryEntry &entry, const OperationRecord *record, const std::size_t bytes) {
            const auto state = record ? record->state : entry.state;
            std::string_view text = entry.output;
            if (!record)
                text = "Result no longer retained.";
            else if (state == OperationState::Succeeded)
                text = record->message;
            else if (state == OperationState::Failed)
                text = record->error ? std::string_view(record->error->code.Value()) : std::string_view("console.handler_failed");
            else if (state == OperationState::Cancelled)
                text = "job.cancelled";
            return {state, Detail::ConsoleTextPrefix(text, bytes), record == nullptr};
        }
    }  // namespace

    /** @copydoc ConsoleHistory::ProjectOperations */
    Result<std::size_t> ConsoleHistory::ProjectOperations(const OperationStoreSnapshot &snapshot) {
        constexpr std::size_t maximumRecords = 4096;
        if (snapshot.operations.size() > maximumRecords || snapshot.activeCapacity > maximumRecords ||
            snapshot.recentCapacity > maximumRecords - snapshot.activeCapacity)
            return Result<std::size_t>::Failure(MakeCommandError(CommandError::LimitExceeded));
        if (snapshot.operations.size() > snapshot.activeCapacity + snapshot.recentCapacity)
            return Result<std::size_t>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        // A bounded temporary index avoids O(history * store) work and keeps all records in their original authority.
        std::array<const OperationRecord *, maximumRecords> index;
        auto rows = std::span(index).first(snapshot.operations.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto &record = snapshot.operations[i];
            if (record.id == 0 || static_cast<unsigned>(record.state) > static_cast<unsigned>(OperationState::Cancelled))
                return Result<std::size_t>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            rows[i] = &record;
        }
        const auto identity = [](const OperationRecord *record) {
            return record->id;
        };
        std::ranges::sort(rows, {}, identity);
        for (std::size_t i = 1; i < rows.size(); ++i)
            if (rows[i - 1]->id == rows[i]->id)
                return Result<std::size_t>::Failure(MakeCommandError(CommandError::Duplicate));
        const auto source = [&](const HistoryEntry &entry) -> const OperationRecord * {
            const auto found = std::ranges::lower_bound(rows, *entry.operation, {}, identity);
            return found == rows.end() || (*found)->id != *entry.operation ? nullptr : *found;
        };
        auto simulatedBytes = retainedBytes_;
        for (const auto &entry : entries_) {
            if (!entry.HasPendingResult())
                continue;
            const auto available = limits_.totalHistoryBytes - (simulatedBytes - entry.output.size());
            const auto value = ProjectValue(entry, source(entry), std::min(limits_.outputBytes, available));
            if (!IsValidUtf8ScalarSequence(value.output))
                return Result<std::size_t>::Failure(MakeCommandError(CommandError::HandlerFailed));
            simulatedBytes = simulatedBytes - entry.output.size() + value.output.size();
        }
        std::size_t changed{};
        for (auto &entry : entries_) {
            if (!entry.HasPendingResult())
                continue;
            const auto availableBytes = limits_.totalHistoryBytes - (retainedBytes_ - entry.output.size());
            const auto value = ProjectValue(entry, source(entry), std::min(limits_.outputBytes, availableBytes));
            if (entry.state == value.state && !value.unavailable && entry.output == value.output)
                continue;
            std::string output(value.output);
            const auto oldBytes = entry.output.size();
            entry.output = std::move(output);
            retainedBytes_ = retainedBytes_ - oldBytes + entry.output.size();
            entry.state = value.state;
            entry.operationUnavailable = value.unavailable;
            if (!entry.HasPendingResult())
                --pendingResults_;
            ++revision_;
            ++changed;
        }
        return Result<std::size_t>::Success(changed);
    }
}  // namespace Horo::Debug
