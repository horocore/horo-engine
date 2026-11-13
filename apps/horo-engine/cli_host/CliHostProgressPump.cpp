#include "CliHostProgressPump.h"

#include "Horo/Cli/CliErrors.h"

#include <condition_variable>
#include <mutex>

namespace Horo::Application::Internal {
    /** @copydoc CliHostProgressPump::CliHostProgressPump */
    CliHostProgressPump::CliHostProgressPump(Cli::CliProgressMailbox &mailbox, Cli::CliOutputPresenter &presenter,
                                             Cli::CliInvocationStopController &stop, const bool terminal)
        : mailbox_(mailbox), presenter_(presenter), stop_(stop), cadence_(terminal), worker_([this](std::stop_token token) {
              Run(token);
          }) {}

    /** @copydoc CliHostProgressPump::Present */
    void CliHostProgressPump::Present(const bool final) {
        const auto event = mailbox_.Take();
        if (!event || (!final && !cadence_.ShouldPresent(*event, std::chrono::steady_clock::now())))
            return;
        if (const auto presented = presenter_.Progress(*event); presented.HasError()) {
            failure_ = presented.ErrorValue();
            stop_.Shutdown();
        }
    }

    /** @copydoc CliHostProgressPump::Run */
    void CliHostProgressPump::Run(const std::stop_token token) noexcept {
        try {
            // This local mutex only supports interruptible sampling waits. Shared events are protected by the mailbox.
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            while (!token.stop_requested() && !failure_ && !raised_) {
                wake.wait_for(lock, token, std::chrono::milliseconds{50}, [] {
                    return false;
                });
                if (!token.stop_requested())
                    Present(false);
            }
        } catch (...) {
            // Report the host failure after joining, including when allocating an error would fail on this worker.
            raised_ = true;
            stop_.Shutdown();
        }
    }

    /** @copydoc CliHostProgressPump::Finish */
    Result<void> CliHostProgressPump::Finish() {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
            if (!failure_ && !raised_)
                Present(true);
        }
        if (failure_)
            return Result<void>::Failure(*failure_);
        return raised_ ? Result<void>::Failure(MakeError(Cli::CliErrors::HostFailure)) : Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
