#include "Horo/Debug/ConsolePresentation.h"

#include <algorithm>

namespace Horo::Debug {
    namespace {
        /** @brief Clips a borrowed immutable list without allocations or per-row copies. */
        template <typename T> std::span<const T> Window(std::span<const T> rows, std::size_t first, std::size_t count) noexcept {
            if (first >= rows.size())
                return {};
            return rows.subspan(first, std::min({count, std::size_t{128}, rows.size() - first}));
        }
    }  // namespace

    /** @copydoc ConsoleTerminal::ConsoleTerminal */
    ConsoleTerminal::ConsoleTerminal(Input::InputRouter &input, CommandExecutor &executor, ConsoleHistory &history, const bool enabled)
        : input_(input), executor_(executor), history_(history), enabled_(enabled), owner_(std::this_thread::get_id()) {}

    /** @copydoc ConsoleTerminal::Open */
    Result<void> ConsoleTerminal::Open(const bool passThrough) {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeCommandError(CommandError::WrongThread));
        if (!enabled_)
            return Result<void>::Failure(MakeCommandError(CommandError::PermissionDenied));
        Close();
        focus_ = input_.PushContext(Input::InputContextId("runtime.debug.console"),
                                    passThrough ? Input::InputContextKind::FocusedGuiWidget : Input::InputContextKind::ModalRoot);
        if (!input_.FocusText(focus_)) {
            Close();
            return Result<void>::Failure(MakeCommandError(CommandError::MissingCapability));
        }
        return Result<void>::Success();
    }

    /** @copydoc ConsoleTerminal::Close */
    void ConsoleTerminal::Close() noexcept {
        focus_.Reset();
    }

    /** @copydoc ConsoleTerminal::Submit */
    Result<CommandReceipt> ConsoleTerminal::Submit(std::string_view text, const DebugCommandContext &principal) {
        if (std::this_thread::get_id() != owner_)
            return Result<CommandReceipt>::Failure(MakeCommandError(CommandError::WrongThread));
        if (!IsOpen())
            return Result<CommandReceipt>::Failure(MakeCommandError(CommandError::PermissionDenied));
        auto result = executor_.Submit(text, principal);
        if (result.HasValue())
            history_.Append({0, {}, result.Value().redactedText}, result.Value().output.text);
        return result;
    }

    /** @copydoc ConsoleTerminal::IsOpen */
    bool ConsoleTerminal::IsOpen() const noexcept {
        return input_.IsContextActive(focus_);
    }

    /** @copydoc ConsoleTerminal::TakeText */
    Result<std::optional<Input::TextInputDelivery>> ConsoleTerminal::TakeText() {
        if (std::this_thread::get_id() != owner_)
            return Result<std::optional<Input::TextInputDelivery>>::Failure(MakeCommandError(CommandError::WrongThread));
        return Result<std::optional<Input::TextInputDelivery>>::Success(input_.TakeText(focus_));
    }

    /** @copydoc ConsoleTerminal::Rows */
    std::span<const HistoryEntry> ConsoleTerminal::Rows(const std::size_t first, const std::size_t count) const noexcept {
        return Window(history_.Entries(), first, count);
    }

    /** @copydoc ConsoleTerminal::RowCount */
    std::size_t ConsoleTerminal::RowCount() const noexcept {
        return history_.Entries().size();
    }

    /** @copydoc ConsoleStoreOverlays::ConsoleStoreOverlays */
    ConsoleStoreOverlays::ConsoleStoreOverlays(const Log::IStructuredLogQuery &logs, const IOperationQuery &operations)
        : logs_(logs), operations_(operations) {}

    /** @copydoc ConsoleStoreOverlays::SetVisible */
    void ConsoleStoreOverlays::SetVisible(const bool logs, const bool jobs) {
        logsVisible_ = logs;
        jobsVisible_ = jobs;
        if (!logs)
            logSnapshot_.reset();
        if (!jobs)
            operationSnapshot_.reset();
    }

    /** @copydoc ConsoleStoreOverlays::Refresh */
    Result<std::size_t> ConsoleStoreOverlays::Refresh(const std::chrono::steady_clock::time_point now) {
        if (!logsVisible_ && !jobsVisible_)
            return Result<std::size_t>::Success(0);
        std::size_t changed{};
        if (logsVisible_ && now >= nextLogRefresh_) {
            nextLogRefresh_ = now + std::chrono::milliseconds(250);
            auto snapshot = logs_.SnapshotIfChanged(logSnapshot_ ? logSnapshot_->revision : 0);
            if (snapshot && (snapshot->records.size() > 4096 || snapshot->capacity > 4096))
                return Result<std::size_t>::Failure(MakeCommandError(CommandError::LimitExceeded));
            if (snapshot) {
                logSnapshot_ = std::move(snapshot);
                ++changed;
            }
        }
        if (jobsVisible_ && now >= nextJobRefresh_) {
            nextJobRefresh_ = now + std::chrono::milliseconds(250);
            auto snapshot = operations_.SnapshotIfChanged(operationSnapshot_ ? operationSnapshot_->revision : 0);
            if (snapshot && (snapshot->operations.size() > 4096 || snapshot->activeCapacity > 4096 ||
                             snapshot->recentCapacity > 4096 - snapshot->activeCapacity))
                return Result<std::size_t>::Failure(MakeCommandError(CommandError::LimitExceeded));
            if (snapshot) {
                operationSnapshot_ = std::move(snapshot);
                ++changed;
            }
        }
        return Result<std::size_t>::Success(changed);
    }

    /** @copydoc ConsoleStoreOverlays::Logs */
    std::span<const std::shared_ptr<const Log::StructuredLogRecord>> ConsoleStoreOverlays::Logs(std::size_t first,
                                                                                                std::size_t count) const noexcept {
        if (!logSnapshot_)
            return {};
        return Window(std::span<const std::shared_ptr<const Log::StructuredLogRecord>>(logSnapshot_->records), first, count);
    }

    /** @copydoc ConsoleStoreOverlays::Jobs */
    std::span<const OperationRecord> ConsoleStoreOverlays::Jobs(std::size_t first, std::size_t count) const noexcept {
        if (!operationSnapshot_)
            return {};
        return Window(std::span<const OperationRecord>(operationSnapshot_->operations), first, count);
    }

    /** @copydoc ConsoleStoreOverlays::LogRowCount */
    std::size_t ConsoleStoreOverlays::LogRowCount() const noexcept {
        return logSnapshot_ ? logSnapshot_->records.size() : 0;
    }

    /** @copydoc ConsoleStoreOverlays::JobRowCount */
    std::size_t ConsoleStoreOverlays::JobRowCount() const noexcept {
        return operationSnapshot_ ? operationSnapshot_->operations.size() : 0;
    }
}  // namespace Horo::Debug
