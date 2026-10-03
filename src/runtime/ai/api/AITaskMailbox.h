#pragma once

#include "Horo/AI/AITaskContinuation.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace Horo::AI::Detail {
    /** @brief Preallocated per-instance mailboxes; owner bind/take and concurrent producers use try-lock only.
     * @details The mutex protects terminal/event storage, exact handle and publication latch. The atomic
     * generation separately revokes admission without locking. No lock is held across provider callbacks.
     * Shared continuations retain storage after owner destruction; no storage contains callable code.
     */
    class AiTaskMailbox final : public std::enable_shared_from_this<AiTaskMailbox> {
    public:
        struct Pending final {
            std::optional<AiTaskTerminalResult> terminal;
            bool event{};
        };

        /** @brief Allocates the fixed slot array before instance activation. */
        explicit AiTaskMailbox(std::size_t capacity);
        /** @brief Rebinds an exclusively owner-reserved slot to a fresh task generation. */
        [[nodiscard]] Result<AiTaskContinuation> Bind(std::size_t slot, const AiTaskOperationContext &operation);
        /** @brief Takes one pending candidate and wake without waiting for a producer. */
        [[nodiscard]] Pending Take(std::size_t slot, TaskHandle task);
        /** @brief Revokes a slot immediately without entering its payload lock. */
        void Retire(std::size_t slot) noexcept;
        /** @brief Checks the nonwrapping execution generation through its atomic admission fence. */
        [[nodiscard]] bool IsCurrent(std::size_t slot, TaskHandle task) const noexcept;
        /** @brief Moves one terminal candidate or coalesces an event under a nonblocking payload lock. */
        [[nodiscard]] AiTaskPublicationDisposition Publish(std::size_t slot, TaskHandle task, AiTaskTerminalResult *result) noexcept;

    private:
        struct Slot final {
            std::mutex mutex;
            std::atomic<std::uint32_t> generation{};
            TaskHandle task;
            Pending pending;
            bool published{};
        };

        std::vector<Slot> slots_;
    };
}  // namespace Horo::AI::Detail
