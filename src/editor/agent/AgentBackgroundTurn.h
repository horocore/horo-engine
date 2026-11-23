#pragma once

/** @file AgentBackgroundTurn.h @brief Editor-owned lifetime of one background model operation. */

#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <optional>
#include <thread>
#include <utility>

namespace Horo::Agent {
    /** @brief Single editor-lane owner for provider work across transient presentation changes. */
    template <typename ReplyValue> class AgentBackgroundTurn final {
    public:
        using Reply = ReplyValue;
        using Operation = std::packaged_task<Reply(std::stop_token)>;

        enum class CompletionState : std::uint8_t {
            Ready,
            Cancelled,
            Abandoned,
            Failed
        };

        struct Completion final {
            CompletionState state;
            std::optional<Reply> reply;
        };

        AgentBackgroundTurn() = default;
        AgentBackgroundTurn(const AgentBackgroundTurn &) = delete;
        AgentBackgroundTurn &operator=(const AgentBackgroundTurn &) = delete;

        /** @brief Requests cancellation and joins the worker before releasing the runtime. */
        ~AgentBackgroundTurn() {
            Cancel();
        }

        /** @brief Starts one operation with owned inputs and cooperative cancellation.
         * @param operation Worker operation whose captures must not borrow editor services or presentation callbacks.
         * @return True if admitted. A second operation is refused until worker retirement and completion drainage.
         */
        [[nodiscard]] bool Start(Operation operation) {
            if (Busy() || !operation.valid())
                return false;
            try {
                auto result = operation.get_future();
                std::promise<Reply> completion;
                auto pending = completion.get_future();
                worker_ = std::jthread{[operation = std::move(operation), result = std::move(result),
                                        completion = std::move(completion)](const std::stop_token stop) mutable {
                    CompleteOnWorker(operation, result, completion, stop);
                }};
                reply_ = std::move(pending);
                abandoned_ = false;
                return true;
            } catch (const std::exception &) {
                return false;
            }
        }

        /** @brief Requests cancellation for the active operation, suppressing even an already-completed reply. */
        void Cancel() noexcept {
            worker_.request_stop();
        }

        /** @brief Retires the current workspace's operation and prevents its result from reaching the next workspace. */
        void Abandon() noexcept {
            abandoned_ = Busy();
            Cancel();
        }

        /** @brief Drains one ready completion on the editor lane. @return Empty until an operation finishes. */
        [[nodiscard]] std::optional<Completion> Poll() {
            if (!Busy() || reply_.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
                return std::nullopt;
            auto ready = std::move(reply_);
            if (abandoned_)
                return Completion{CompletionState::Abandoned, {}};
            if (worker_.get_stop_token().stop_requested())
                return Completion{CompletionState::Cancelled, {}};
            try {
                return Completion{CompletionState::Ready, ready.get()};
            } catch (...) {
                return Completion{CompletionState::Failed, {}};
            }
        }

        /** @brief Reports whether the one worker slot is still occupied. */
        [[nodiscard]] bool Busy() const noexcept {
            return reply_.valid();
        }

        /** @brief Reports whether the active operation can receive a new cancellation request. */
        [[nodiscard]] bool CanCancel() const noexcept {
            return Busy() && !worker_.get_stop_token().stop_requested();
        }

    private:
        /**
         * @brief Releases the operation and its captured resources on the worker before exposing completion.
         * @param operation The task that owns the provider or checkpoint operation.
         * @param result Its single-owner result future.
         * @param completion The editor's separate completion channel.
         * @param stop The worker's cooperative stop token.
         */
        static void CompleteOnWorker(Operation &operation, std::future<Reply> &result, std::promise<Reply> &completion,
                                     const std::stop_token stop) {
            try {
                operation(stop);
                // The future may retain the packaged callable even after the task handle is released.
                operation = {};
                completion.set_value_at_thread_exit(result.get());
            } catch (...) {
                completion.set_exception_at_thread_exit(std::current_exception());
            }
        }

        std::jthread worker_;
        std::future<Reply> reply_;
        bool abandoned_{};
    };
}  // namespace Horo::Agent
