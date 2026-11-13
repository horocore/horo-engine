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
    ConsoleTerminal::ConsoleTerminal(Input::InputRouter &input, CommandExecutor &executor, ConsoleHistory &history,
                                     const IOperationQuery &operations, const bool enabled, const ConfigurationService &configuration)
        : input_(input), executor_(executor), history_(history), operations_(operations), configuration_(configuration), enabled_(enabled),
          owner_(std::this_thread::get_id()) {}

    /** @copydoc ConsoleTerminal::Open */
    Result<void> ConsoleTerminal::Open(const bool passThrough) {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeCommandError(CommandError::WrongThread));
        if (!enabled_)
            return Result<void>::Failure(MakeCommandError(CommandError::PermissionDenied));
        Close();
        focus_ = input_.PushContext(Input::InputContextId("runtime.debug.console"),
                                    passThrough ? Input::InputContextKind::FocusedGuiWidget : Input::InputContextKind::ModalRoot,
                                    passThrough ? Input::InputContextPropagation::GameplayPassThrough
                                                : Input::InputContextPropagation::Exclusive);
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
        auto result = executor_.Submit(text, principal, false, configuration_.Snapshot());
        if (result.HasValue())
            history_.Append({0, {}, result.Value().redactedText}, result.Value().output.text, result.Value().operation);
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
        if (IsOpen() && !input_.FocusText(focus_))
            return Result<std::optional<Input::TextInputDelivery>>::Failure(MakeCommandError(CommandError::MissingCapability));
        return Result<std::optional<Input::TextInputDelivery>>::Success(input_.TakeText(focus_));
    }

    /** @copydoc ConsoleTerminal::RefreshResults */
    Result<std::size_t> ConsoleTerminal::RefreshResults(const std::chrono::steady_clock::time_point now) {
        if (std::this_thread::get_id() != owner_)
            return Result<std::size_t>::Failure(MakeCommandError(CommandError::WrongThread));
        if (!enabled_ || history_.PendingResults() == 0 || now < nextResultRefresh_)
            return Result<std::size_t>::Success(0);
        nextResultRefresh_ = now + std::chrono::milliseconds(250);
        const auto snapshot = operations_.SnapshotIfChanged(operationRevision_);
        if (!snapshot)
            return Result<std::size_t>::Success(0);
        auto projected = history_.ProjectOperations(*snapshot);
        if (projected.HasValue())
            operationRevision_ = snapshot->revision;
        return projected;
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
        if (logs != logsVisible_)
            ++logProjectionRevision_;
        if (jobs != jobsVisible_)
            ++jobProjectionRevision_;
        if (logs && !logsVisible_)
            logFilterDirty_ = true;
        logsVisible_ = logs;
        jobsVisible_ = jobs;
        if (!logs) {
            logSnapshot_.reset();
            logRows_.clear();
            logScratch_.clear();
        }
        if (!jobs) {
            operationRows_.clear();
            operationSnapshot_.reset();
        }
    }

    /** @copydoc ConsoleStoreOverlays::Refresh */
    Result<std::size_t> ConsoleStoreOverlays::Refresh(const std::chrono::steady_clock::time_point now) {
        if (std::this_thread::get_id() != owner_)
            return Result<std::size_t>::Failure(MakeCommandError(CommandError::WrongThread));
        if (!logsVisible_ && !jobsVisible_)
            return Result<std::size_t>::Success(0);
        // Independent sources still refresh when their peer fails; return the first failure after both bounded attempts.
        const auto logs = RefreshLogs(now);
        const auto jobs = RefreshJobs(now);
        if (logs.HasError())
            return Result<std::size_t>::Failure(logs.ErrorValue());
        if (jobs.HasError())
            return Result<std::size_t>::Failure(jobs.ErrorValue());
        return Result<std::size_t>::Success(static_cast<std::size_t>(logs.Value()) + static_cast<std::size_t>(jobs.Value()));
    }

    /** @copydoc ConsoleStoreOverlays::RefreshLogs */
    Result<bool> ConsoleStoreOverlays::RefreshLogs(const std::chrono::steady_clock::time_point now) {
        if (!logsVisible_ || now < nextLogRefresh_)
            return Result<bool>::Success(false);
        nextLogRefresh_ = now + std::chrono::milliseconds(250);
        auto snapshot = logs_.SnapshotIfChanged(logSnapshot_ ? logSnapshot_->revision : 0);
        if (snapshot && (snapshot->records.size() > 4096 || snapshot->capacity > 4096))
            return Result<bool>::Failure(MakeCommandError(CommandError::LimitExceeded));
        if (snapshot) {
            auto previous = std::move(logSnapshot_);
            logSnapshot_ = std::move(snapshot);
            logFilterDirty_ = true;
            auto projected = RebuildLogRows();
            if (projected.HasError()) {
                logSnapshot_ = std::move(previous);
                return Result<bool>::Failure(projected.ErrorValue());
            }
            logFilterDirty_ = false;
            return Result<bool>::Success(true);
        }
        if (!logFilterDirty_)
            return Result<bool>::Success(false);
        auto projected = RebuildLogRows();
        if (projected.HasError())
            return Result<bool>::Failure(projected.ErrorValue());
        logFilterDirty_ = false;
        return Result<bool>::Success(true);
    }

    /** @copydoc ConsoleStoreOverlays::RefreshJobs */
    Result<bool> ConsoleStoreOverlays::RefreshJobs(const std::chrono::steady_clock::time_point now) {
        if (!jobsVisible_ || now < nextJobRefresh_)
            return Result<bool>::Success(false);
        nextJobRefresh_ = now + std::chrono::milliseconds(250);
        auto snapshot = operations_.SnapshotIfChanged(operationSnapshot_ ? operationSnapshot_->revision : 0);
        if (snapshot && (snapshot->operations.size() > 4096 || snapshot->activeCapacity > 4096 ||
                         snapshot->recentCapacity > 4096 - snapshot->activeCapacity))
            return Result<bool>::Failure(MakeCommandError(CommandError::LimitExceeded));
        if (!snapshot)
            return Result<bool>::Success(false);
        if (snapshot->operations.size() > snapshot->activeCapacity + snapshot->recentCapacity)
            return Result<bool>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        const auto accepted = AcceptJobs(std::move(*snapshot));
        if (accepted.HasError())
            return Result<bool>::Failure(accepted.ErrorValue());
        return Result<bool>::Success(true);
    }

    /** @copydoc ConsoleStoreOverlays::Logs */
    std::span<const ConsoleLogRow> ConsoleStoreOverlays::Logs(std::size_t first, std::size_t count) const noexcept {
        return Window(std::span<const ConsoleLogRow>(logRows_), first, count);
    }

    /** @copydoc ConsoleStoreOverlays::Jobs */
    std::span<const ConsoleOperationRow> ConsoleStoreOverlays::Jobs(std::size_t first, std::size_t count) const noexcept {
        return Window(std::span<const ConsoleOperationRow>(operationRows_), first, count);
    }

    /** @copydoc ConsoleStoreOverlays::LogRowCount */
    std::size_t ConsoleStoreOverlays::LogRowCount() const noexcept {
        return logRows_.size();
    }

    /** @copydoc ConsoleStoreOverlays::JobRowCount */
    std::size_t ConsoleStoreOverlays::JobRowCount() const noexcept {
        return operationRows_.size();
    }

    /** @copydoc ConsoleStoreOverlays::LogProjectionRevision */
    std::uint64_t ConsoleStoreOverlays::LogProjectionRevision() const noexcept {
        return logProjectionRevision_;
    }

    /** @copydoc ConsoleStoreOverlays::JobProjectionRevision */
    std::uint64_t ConsoleStoreOverlays::JobProjectionRevision() const noexcept {
        return jobProjectionRevision_;
    }
}  // namespace Horo::Debug
