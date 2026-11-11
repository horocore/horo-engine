#pragma once
/** @file CliHostProgressPump.h @brief Owned native CLI progress presentation during synchronous application dispatch. */

#include "Horo/Cli/CliDispatcher.h"
#include "Horo/Cli/CliOutputPresenter.h"

#include <optional>
#include <thread>

namespace Horo::Application::Internal {
    /** @brief Owns the sole presenter writer until Finish joins it on the application lane.
     * @details The mailbox supports concurrent producers. Presenter access and failure state belong to the worker until join,
     * then transfer to the caller. All borrowed owners outlive this object. Its worker is destroyed first during unwinding.
     * A slow output consumer retains at most one bounded mailbox event while application work continues independently.
     */
    class CliHostProgressPump final {
    public:
        /** @brief Starts bounded progress sampling without moving application ownership.
         * @param mailbox Concurrent bounded event owner.
         * @param presenter Exclusive writer, borrowed until Finish or destruction.
         * @param stop Thread-safe invocation stop authority, requested on presentation failure.
         * @param terminal Whether human presentation uses terminal cadence.
         */
        CliHostProgressPump(Cli::CliProgressMailbox &mailbox, Cli::CliOutputPresenter &presenter, Cli::CliInvocationStopController &stop,
                            bool terminal);
        ~CliHostProgressPump() = default;
        CliHostProgressPump(const CliHostProgressPump &) = delete;
        CliHostProgressPump &operator=(const CliHostProgressPump &) = delete;

        /** @brief Joins the writer and presents any remaining event before final output.
         * @return Success, the original presentation failure, or a host failure after a writer exception.
         */
        [[nodiscard]] Result<void> Finish();

    private:
        /** @brief Presents one coalesced event on the sole writer lane.
         * @param final Bypasses cadence after join to drain the remaining event.
         */
        void Present(bool final);
        /** @brief Samples bounded events until stop or presentation failure.
         * @param token Owned presentation-worker stop token.
         */
        void Run(std::stop_token token) noexcept;

        Cli::CliProgressMailbox &mailbox_;
        Cli::CliOutputPresenter &presenter_;
        Cli::CliInvocationStopController &stop_;
        Cli::CliProgressCadence cadence_;
        std::optional<Error> failure_;
        bool raised_{};
        std::jthread worker_;
    };
}  // namespace Horo::Application::Internal
