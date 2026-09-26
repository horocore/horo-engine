#include "Horo/Cli/CliDispatcher.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/OperationStore.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <ostream>
#include <utility>

namespace Horo::Cli {
    namespace {
        [[nodiscard]] std::string SafeProgressText(std::string value) {
            for (char &character : value) {
                const unsigned char byte = static_cast<unsigned char>(character);
                if (byte < 0x20U || byte == 0x7FU)
                    character = ' ';
            }
            return value;
        }
    }  // namespace

    /** @copydoc CliProgressProjection::CliProgressProjection */
    CliProgressProjection::CliProgressProjection(const IOperationQuery *operations, const JobSystem *jobs,
                                                 CliExecutionCorrelation correlation)
        : operations_(operations), jobs_(jobs), correlation_(std::move(correlation)) {}

    /** @copydoc CliProgressProjection::Poll */
    std::optional<CliProgressEvent> CliProgressProjection::Poll() {
        if (operations_ != nullptr && correlation_.operation.has_value()) {
            if (auto snapshot = operations_->SnapshotIfChanged(operationRevision_); snapshot.has_value()) {
                operationRevision_ = snapshot->revision;
                const auto found =
                    std::find_if(snapshot->operations.begin(), snapshot->operations.end(), [this](const OperationRecord &record) {
                    return record.id == correlation_.operation->value;
                });
                if (found != snapshot->operations.end()) {
                    operationSeen_ = true;
                    return CliProgressEvent{found->phase, found->progress.value_or(0.0F), found->message};
                }
            }
        }
        if (operationSeen_ || jobs_ == nullptr || !correlation_.job.has_value())
            return std::nullopt;
        auto snapshot = jobs_->SnapshotIfChanged(jobRevision_);
        if (!snapshot.has_value())
            return std::nullopt;
        jobRevision_ = snapshot->revision;
        const auto found = std::find_if(snapshot->jobs.begin(), snapshot->jobs.end(), [this](const JobSnapshot &job) {
            return job.id == correlation_.job->value;
        });
        if (found == snapshot->jobs.end())
            return std::nullopt;
        return CliProgressEvent{std::string{found->progress.phase.View()}, found->progress.value.value_or(0.0F), {}};
    }

    /** @copydoc CliInvocationStopController::CliInvocationStopController */
    CliInvocationStopController::CliInvocationStopController(const std::chrono::milliseconds timeout, CancellationToken parent)
        : parent_(std::move(parent)), cancellation_(parent_) {
        if (timeout > std::chrono::milliseconds::zero()) {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            deadlineWorker_ = std::jthread([this, deadline](const std::stop_token stop) {
                std::unique_lock lock(deadlineMutex_);
                deadlineWake_.wait_until(lock, stop, deadline, [] {
                    return false;
                });
                if (!stop.stop_requested())
                    Request(parent_.IsCancellationRequested() ? CliStopReason::ParentCancelled : CliStopReason::TimedOut);
            });
        }
    }

    void CliInvocationStopController::Request(const CliStopReason reason) noexcept {
        CliStopReason expected = CliStopReason::None;
        if (reason_.compare_exchange_strong(expected, reason))
            cancellation_.RequestCancellation();
    }

    /** @copydoc CliInvocationStopController::Interrupt */
    void CliInvocationStopController::Interrupt() noexcept {
        if (interrupts_.fetch_add(1) == 0)
            Request(CliStopReason::Interrupted);
        else
            force_.RequestCancellation();
    }

    /** @copydoc CliInvocationStopController::Shutdown */
    void CliInvocationStopController::Shutdown() noexcept {
        Request(CliStopReason::Shutdown);
    }

    /** @copydoc CliInvocationStopController::Token */
    CancellationToken CliInvocationStopController::Token() const noexcept {
        return cancellation_.Token();
    }

    /** @copydoc CliInvocationStopController::ForceToken */
    CancellationToken CliInvocationStopController::ForceToken() const noexcept {
        return force_.Token();
    }

    /** @copydoc CliInvocationStopController::Reason */
    CliStopReason CliInvocationStopController::Reason() const noexcept {
        const CliStopReason reason = reason_.load();
        return reason == CliStopReason::None && parent_.IsCancellationRequested() ? CliStopReason::ParentCancelled : reason;
    }

    /** @copydoc CliProgressMailbox::Report */
    void CliProgressMailbox::Report(const CliProgressEvent &event) {
        constexpr std::size_t MaximumTextBytes = 4096;
        CliProgressEvent bounded{event.phase.substr(0, MaximumTextBytes),
                                 std::isfinite(event.completion) ? std::clamp(event.completion, 0.0F, 1.0F) : 0.0F,
                                 event.message.substr(0, MaximumTextBytes)};
        std::lock_guard lock(mutex_);
        latest_ = std::move(bounded);
    }

    /** @copydoc CliProgressMailbox::Take */
    std::optional<CliProgressEvent> CliProgressMailbox::Take() {
        std::lock_guard lock(mutex_);
        return std::exchange(latest_, std::nullopt);
    }

    /** @copydoc CliProgressCadence::CliProgressCadence */
    CliProgressCadence::CliProgressCadence(const bool tty, const std::chrono::milliseconds minimumInterval)
        : tty_(tty), minimumInterval_(std::max(minimumInterval, std::chrono::milliseconds::zero())) {}

    /** @copydoc CliProgressCadence::ShouldPresent */
    bool CliProgressCadence::ShouldPresent(const CliProgressEvent &event, const std::chrono::steady_clock::time_point now) {
        if (last_.has_value() && phase_ == event.phase && message_ == event.message && completion_ == event.completion)
            return false;
        if (!tty_ && last_.has_value() && phase_ == event.phase && now - *last_ < minimumInterval_)
            return false;
        last_ = now;
        phase_ = event.phase;
        message_ = event.message;
        completion_ = event.completion;
        return true;
    }

    /** @copydoc CliProgressPresenter::CliProgressPresenter */
    CliProgressPresenter::CliProgressPresenter(CliProgressMailbox &mailbox, std::ostream &output, std::ostream &diagnostics,
                                               const CliProgressOutputMode mode, const bool tty)
        : mailbox_(&mailbox), output_(&output), diagnostics_(&diagnostics), mode_(mode), tty_(tty), cadence_(tty) {}

    /** @copydoc CliProgressPresenter::Pump */
    void CliProgressPresenter::Pump(const std::chrono::steady_clock::time_point now) {
        const auto event = mailbox_->Take();
        if (!event.has_value() || mode_ == CliProgressOutputMode::Json)
            return;
        if (mode_ == CliProgressOutputMode::JsonLines) {
            *output_ << nlohmann::json{{"type", "progress"},
                                       {"phase", event->phase},
                                       {"completion", event->completion},
                                       {"message", event->message}}
                            .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)
                     << '\n';
            return;
        }
        if (!cadence_.ShouldPresent(*event, now))
            return;
        if (tty_)
            *diagnostics_ << '\r';
        *diagnostics_ << SafeProgressText(event->phase) << ' ' << static_cast<int>(event->completion * 100.0F) << '%';
        if (!event->message.empty())
            *diagnostics_ << ' ' << SafeProgressText(event->message);
        if (tty_)
            diagnostics_->flush();
        else
            *diagnostics_ << '\n';
    }
}  // namespace Horo::Cli
