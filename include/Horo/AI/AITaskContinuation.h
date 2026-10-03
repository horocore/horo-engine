#pragma once

/** @file AITaskContinuation.h
 * @brief Bounded detached task outcomes and event wakes for the decision owner.
 */

#include "Horo/AI/AITaskLifecycle.h"

#include <cstddef>
#include <memory>

namespace Horo::AI {
    namespace Detail {
        class AiTaskMailbox;
    }

    /** @brief Publication is candidate admission, never a lifecycle transition on the producer thread. */
    enum class AiTaskPublicationDisposition : std::uint8_t {
        Accepted,
        AlreadyPublished,
        Stale,
        Contended,
    };

    /**
     * @brief Copyable detached continuation for one exact task execution.
     * @details Producers may retain this value across workers and callbacks. It owns mailbox storage, never
     * the tree, executor, Scene or mutable blackboard. One terminal candidate and one coalesced event fit in
     * its reserved slot. Try-publication never waits; Contended requires retry by the producer. Retirement
     * immediately closes the generation without waiting for producers. Only owner evaluation advances flow.
     */
    class AiTaskContinuation final {
    public:
        AiTaskContinuation() = default;
        /** @brief Offers one canonical terminal candidate, preserving the value on rejection.
         * @param result Owned terminal shape; moved only when accepted.
         * @return Admission disposition, or typed malformed-terminal failure.
         */
        [[nodiscard]] Result<AiTaskPublicationDisposition> PublishTerminal(AiTaskTerminalResult &result) const;
        /** @brief Coalesces an event wake for this generation. @return Nonblocking publication disposition. */
        [[nodiscard]] AiTaskPublicationDisposition NotifyEvent() const noexcept;
        /** @brief Checks logical currentness, independent of retained storage lifetime. @return True while admitted. */
        [[nodiscard]] bool IsCurrent() const noexcept;

        /** @brief Returns detached immutable execution identity. @return Captured operation or an invalid default context. */
        [[nodiscard]] const AiTaskOperationContext &Operation() const noexcept {
            return operation_;
        }

    private:
        friend class Detail::AiTaskMailbox;
        /** @brief Captures one owner-issued mailbox slot and immutable execution fence. */
        AiTaskContinuation(std::shared_ptr<Detail::AiTaskMailbox> mailbox, std::size_t slot, AiTaskOperationContext operation);
        std::shared_ptr<Detail::AiTaskMailbox> mailbox_;
        std::size_t slot_{};
        AiTaskOperationContext operation_;
    };
}  // namespace Horo::AI
