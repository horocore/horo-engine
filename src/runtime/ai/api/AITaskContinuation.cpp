#include "Horo/AI/AITaskContinuation.h"

#include "AITaskMailbox.h"

#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Validates the owned failure identity independently of terminal state. */
        bool ValidFailure(const AiTaskFailureDetail &failure) noexcept {
            return failure.kind < AiTaskFailureKind::Count && !failure.cause.domain.Value().empty() && !failure.cause.code.Value().empty();
        }

        /** @brief Admits only the canonical lifecycle's closed terminal shapes. */
        bool ValidTerminal(const AiTaskTerminalResult &result) noexcept {
            using enum AiTaskState;
            if (result.state == Succeeded)
                return !result.failure && !result.cancellationReason;
            if (result.state == Cancelled)
                return !result.failure && result.cancellationReason && *result.cancellationReason < AiTaskCancellationReason::Count;
            if (result.state != Failed || !result.failure || result.cancellationReason)
                return false;
            return ValidFailure(*result.failure);
        }
    }  // namespace

    /** @copydoc AiTaskContinuation::AiTaskContinuation */
    AiTaskContinuation::AiTaskContinuation(std::shared_ptr<Detail::AiTaskMailbox> mailbox, const std::size_t slot,
                                           AiTaskOperationContext operation)
        : mailbox_(std::move(mailbox)), slot_(slot), operation_(std::move(operation)) {}

    /** @copydoc AiTaskContinuation::PublishTerminal */
    Result<AiTaskPublicationDisposition> AiTaskContinuation::PublishTerminal(AiTaskTerminalResult &result) const {
        if (!ValidTerminal(result))
            return Result<AiTaskPublicationDisposition>::Failure(MakeError(AIErrors::TaskFailureInvalid));
        return Result<AiTaskPublicationDisposition>::Success(IsCurrent() ? mailbox_->Publish(slot_, operation_.task, &result)
                                                                         : AiTaskPublicationDisposition::Stale);
    }

    /** @copydoc AiTaskContinuation::NotifyEvent */
    AiTaskPublicationDisposition AiTaskContinuation::NotifyEvent() const noexcept {
        return IsCurrent() ? mailbox_->Publish(slot_, operation_.task, nullptr) : AiTaskPublicationDisposition::Stale;
    }

    /** @copydoc AiTaskContinuation::IsCurrent */
    bool AiTaskContinuation::IsCurrent() const noexcept {
        return mailbox_ && !operation_.IsCancellationRequested() && mailbox_->IsCurrent(slot_, operation_.task);
    }

    namespace Detail {
        /** @copydoc AiTaskMailbox::AiTaskMailbox */
        std::uint64_t AiTaskMailbox::StorageBytes(std::size_t capacity) noexcept {
            return sizeof(AiTaskMailbox) + capacity * sizeof(Slot);
        }

        AiTaskMailbox::AiTaskMailbox(const std::size_t capacity) : slots_(capacity) {}

        /** @copydoc AiTaskMailbox::Bind */
        Result<AiTaskContinuation> AiTaskMailbox::Bind(const std::size_t slot, const AiTaskOperationContext &operation) {
            auto &record = slots_[slot];
            if (const std::unique_lock lock(record.mutex, std::try_to_lock); !lock.owns_lock()) {
                return Result<AiTaskContinuation>::Failure(MakeError(AIErrors::TaskCapacityExceeded));
            } else {
                record.task = operation.task;
                record.pending = {};
                record.pendingGeneration.store(0);
                record.published = false;
                record.generation.store(operation.task.slot.generation);
                return Result<AiTaskContinuation>::Success(AiTaskContinuation(shared_from_this(), slot, operation));
            }
        }

        /** @copydoc AiTaskMailbox::Take */
        AiTaskMailbox::Pending AiTaskMailbox::Take(const std::size_t slot, const TaskHandle task) {
            auto &record = slots_[slot];
            const std::unique_lock lock(record.mutex, std::try_to_lock);
            if (!lock.owns_lock() || record.task != task || !IsCurrent(slot, task))
                return {};
            record.pendingGeneration.store(0);
            return std::exchange(record.pending, {});
        }

        /** @copydoc AiTaskMailbox::HasPending */
        bool AiTaskMailbox::HasPending() noexcept {
            // Clear the hint before scanning so a producer racing the scan can signal another check.
            if (!pendingHint_.load() || !pendingHint_.exchange(false))
                return false;
            for (const auto &slot : slots_) {
                const auto generation = slot.generation.load();
                if (generation != 0 && slot.pendingGeneration.load() == generation) {
                    pendingHint_.store(true);
                    return true;
                }
            }
            return false;
        }

        /** @copydoc AiTaskMailbox::Retire */
        void AiTaskMailbox::Retire(const std::size_t slot) noexcept {
            slots_[slot].generation.store(0);
        }

        /** @copydoc AiTaskMailbox::IsCurrent */
        bool AiTaskMailbox::IsCurrent(const std::size_t slot, const TaskHandle task) const noexcept {
            return slots_[slot].generation.load() == task.slot.generation;
        }

        /** @copydoc AiTaskMailbox::Publish */
        AiTaskPublicationDisposition AiTaskMailbox::Publish(const std::size_t slot, const TaskHandle task,
                                                            AiTaskTerminalResult *result) noexcept {
            auto &record = slots_[slot];
            using enum AiTaskPublicationDisposition;
            {
                const std::unique_lock lock(record.mutex, std::try_to_lock);
                if (!lock.owns_lock())
                    return Contended;
                if (record.task != task || !IsCurrent(slot, task))
                    return Stale;
                if (record.published)
                    return AlreadyPublished;
                if (result) {
                    record.pending.terminal.emplace(std::move(*result));
                    record.published = true;
                } else {
                    record.pending.event = true;
                }
                record.pendingGeneration.store(task.slot.generation);
            }
            pendingHint_.store(true);
            return Accepted;
        }
    }  // namespace Detail
}  // namespace Horo::AI
