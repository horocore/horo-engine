#include "Horo/Runtime/Render/RenderReadback.h"

#include "BoundedRenderQueueCore.h"
#include "Horo/Runtime/Render/RenderReadbackErrors.h"

#include <atomic>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Render {
    namespace {
        [[nodiscard]] Error ReadbackError(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] bool IsSourceValid(const RenderReadbackSource &source) noexcept {
            return std::visit([](const auto handle) {
                return handle.IsValid();
            }, source);
        }

        [[nodiscard]] RenderResourceOwnerId SourceOwner(const RenderReadbackSource &source) noexcept {
            return std::visit([](const auto handle) {
                return handle.owner;
            }, source);
        }

        [[nodiscard]] bool IsTerminal(const RenderReadbackState state) noexcept {
            using enum RenderReadbackState;
            return state == Failed || state == Cancelled || state == TimedOut;
        }

        struct RetainedAccounting {
            std::atomic_size_t bytes{0};
        };

        struct RetainedPayload {
            RetainedPayload(std::shared_ptr<RetainedAccounting> owner, std::vector<std::byte> mapped)
                : accounting(std::move(owner)), bytes(std::move(mapped)) {
                accounting->bytes.fetch_add(bytes.size(), std::memory_order_relaxed);
            }

            ~RetainedPayload() {
                accounting->bytes.fetch_sub(bytes.size(), std::memory_order_relaxed);
            }

            RetainedPayload(const RetainedPayload &) = delete;
            RetainedPayload &operator=(const RetainedPayload &) = delete;
            RetainedPayload(RetainedPayload &&) = delete;
            RetainedPayload &operator=(RetainedPayload &&) = delete;

            std::shared_ptr<RetainedAccounting> accounting;
            std::vector<std::byte> bytes;
        };

        struct ReadbackRecord {
            RenderReadbackId id;
            RenderReadbackDescriptor descriptor;
            RenderReadbackState state{RenderReadbackState::Pending};
            RenderTimelinePoint completion;
            std::shared_ptr<const RetainedPayload> payload;
            std::optional<Error> failure;
            bool pendingBytesCharged{true};
        };

        struct ReadyReadback {
            RenderReadbackId id;
            RenderTimelinePoint completion;
            std::shared_ptr<const RetainedPayload> payload;
        };

        [[nodiscard]] Error ReadbackAdmissionError(const detail::QueueAdmissionFailure failure) {
            using enum detail::QueueAdmissionFailure;
            switch (failure) {
                case Capacity:
                    return ReadbackError(RenderReadbackErrors::CapacityExceeded,
                                         "Readback metadata, staging, or retained-result capacity is exhausted.");
                case Identity:
                    return ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback request identity space is exhausted.");
                case Allocation:
                    return ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback request storage allocation failed.");
                case Length:
                    return ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback metadata capacity cannot be represented.");
            }
            return ReadbackError(RenderReadbackErrors::CapacityExceeded);
        }
    }  // namespace

    /** @copydoc RenderReadbackDescriptor::IsValid */
    bool RenderReadbackDescriptor::IsValid() const noexcept {
        return IsSourceValid(source) && byteCount > 0 && sourceByteOffset <= std::numeric_limits<std::size_t>::max() - byteCount &&
               alignment > 0 && (alignment & (alignment - 1U)) == 0 && timeout.count() > 0;
    }

    class RenderReadbackResult::Impl final {
    public:
        Impl(const RenderReadbackId request, const RenderTimelinePoint completionPoint,
             std::shared_ptr<const RetainedPayload> retainedPayload) noexcept
            : id(request), completion(completionPoint), payload(std::move(retainedPayload)) {}

        RenderReadbackId id;
        RenderTimelinePoint completion;
        std::shared_ptr<const RetainedPayload> payload;
    };

    /** @copydoc RenderReadbackResult::~RenderReadbackResult */
    RenderReadbackResult::~RenderReadbackResult() = default;
    RenderReadbackResult::RenderReadbackResult(RenderReadbackResult &&) noexcept = default;
    RenderReadbackResult &RenderReadbackResult::operator=(RenderReadbackResult &&) noexcept = default;

    RenderReadbackResult::RenderReadbackResult(std::unique_ptr<Impl> implementation) noexcept
        : implementation_(std::move(implementation)) {}

    /** @copydoc RenderReadbackResult::Id */
    RenderReadbackId RenderReadbackResult::Id() const noexcept {
        return implementation_ == nullptr ? RenderReadbackId{} : implementation_->id;
    }

    /** @copydoc RenderReadbackResult::Completion */
    RenderTimelinePoint RenderReadbackResult::Completion() const noexcept {
        return implementation_ == nullptr ? RenderTimelinePoint{} : implementation_->completion;
    }

    /** @copydoc RenderReadbackResult::Bytes */
    std::span<const std::byte> RenderReadbackResult::Bytes() const noexcept {
        if (implementation_ == nullptr || implementation_->payload == nullptr)
            return {};
        return implementation_->payload->bytes;
    }

    class RenderReadbackQueue::Impl final {
        using enum RenderReadbackState;
        using Core = detail::BoundedRenderQueueCore<ReadbackRecord, RenderReadbackId, RenderReadbackLimits>;
        using OperationAdapter = detail::QueueOperationAdapter<Core>;

    public:
        Impl(const RenderResourceOwnerId renderer, const RenderReadbackLimits &limits)
            : retained_(std::make_shared<RetainedAccounting>()), core_(renderer, limits),
              operations_(core_, RenderReadbackErrors::WrongThread, RenderReadbackErrors::InvalidRequest,
                          "Readback identity is malformed, foreign, or no longer tracked.") {}

        [[nodiscard]] Result<RenderReadbackId> Request(const RenderReadbackDescriptor &descriptor) {
            if (!core_.OnOwnerThread())
                return Result<RenderReadbackId>::Failure(ReadbackError(RenderReadbackErrors::WrongThread));
            if (!core_.Accepting())
                return Result<RenderReadbackId>::Failure(ReadbackError(RenderReadbackErrors::Stopped, "Readback admission is stopped."));
            const RenderReadbackLimits &limits = core_.LimitsValue();
            if (!descriptor.IsValid() || SourceOwner(descriptor.source) != core_.Renderer() ||
                descriptor.byteCount > limits.maximumRequestBytes || descriptor.alignment > limits.maximumAlignment) {
                return Result<RenderReadbackId>::Failure(
                    ReadbackError(RenderReadbackErrors::InvalidDescriptor,
                                  "Readback descriptor exceeds the configured byte or alignment bounds."));
            }
            const std::size_t retainedBytes = retained_->bytes.load(std::memory_order_relaxed);
            const bool pendingCapacityAvailable = descriptor.byteCount <= limits.maximumPendingBytes - core_.PendingBytes();
            const bool retainedCapacityAvailable =
                retainedBytes <= limits.maximumRetainedResultBytes &&
                core_.PendingBytes() <= limits.maximumRetainedResultBytes - retainedBytes &&
                descriptor.byteCount <= limits.maximumRetainedResultBytes - retainedBytes - core_.PendingBytes();
            auto admitted = core_.Admit(pendingCapacityAvailable && retainedCapacityAvailable, [descriptor](const RenderReadbackId id) {
                return ReadbackRecord{.id = id, .descriptor = descriptor};
            }, ReadbackAdmissionError);
            if (admitted.HasValue())
                core_.ChargePending(descriptor.byteCount);
            return admitted;
        }

        [[nodiscard]] Result<void> MarkSubmitted(const RenderReadbackId request, const RenderTimelinePoint completion) {
            return operations_.Apply<Result<void>>(request, [completion, this](auto &record) {
                if (record.state != Pending || !completion.IsValid())
                    return InvalidTransition("Only a pending readback may receive a valid completion point.");
                record.state = Submitted;
                record.completion = completion;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Complete(const RenderReadbackId request, const std::span<const std::byte> mappedBytes) {
            return operations_.Apply<Result<void>>(request, [this, mappedBytes](auto &record) {
                return CompleteRecord(record, mappedBytes);
            });
        }

        [[nodiscard]] Result<void> Fail(const RenderReadbackId request, const Error &error) {
            return operations_.Apply<Result<void>>(request, [this, &error](auto &record) {
                if (record.state == Cancelled || record.state == TimedOut) {
                    ReleasePending(record);
                    return Result<void>::Success();
                }
                if (record.state != Pending && record.state != Submitted)
                    return InvalidTransition("Only pending or submitted readback work may fail.");
                ReleasePending(record);
                record.failure = error;
                record.state = Failed;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Cancel(const RenderReadbackId request) {
            return operations_.Apply<Result<void>>(request, [this](auto &record) {
                return detail::TransitionQueueRecord(record, Cancelled, Pending, Submitted, [this] {
                    return InvalidTransition("Only pending or submitted readback work may be cancelled.");
                }, [this](auto &pendingRecord) {
                    ReleasePending(pendingRecord);
                });
            });
        }

        [[nodiscard]] Result<void> Timeout(const RenderReadbackId request) {
            return operations_.Apply<Result<void>>(request, [this](auto &record) {
                return detail::TransitionQueueRecord(record, TimedOut, Pending, Submitted, [this] {
                    return InvalidTransition("Only pending or submitted readback work may time out.");
                }, [this](auto &pendingRecord) {
                    ReleasePending(pendingRecord);
                });
            });
        }

        [[nodiscard]] Result<void> Retire(const RenderReadbackId request) {
            return operations_.Apply<Result<void>>(request, [this](auto &record) {
                if (record.state != Cancelled && record.state != TimedOut)
                    return InvalidTransition("Only cancelled or timed-out submitted work may retire without publication.");
                ReleasePending(record);
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<RenderReadbackState> State(const RenderReadbackId request) const {
            return operations_.Apply<Result<RenderReadbackState>>(request, [this](const auto &record) {
                return Result<RenderReadbackState>::Success(record.state);
            });
        }

        [[nodiscard]] Result<ReadyReadback> AcquireView(const RenderReadbackId request) const {
            return operations_.Apply<Result<ReadyReadback>>(request, [](const auto &record) {
                if (record.state == Pending || record.state == Submitted)
                    return Result<ReadyReadback>::Failure(
                        ReadbackError(RenderReadbackErrors::ResultPending, "Readback result has not completed."));
                if (record.state == Cancelled)
                    return Result<ReadyReadback>::Failure(ReadbackError(RenderReadbackErrors::Cancelled, "Readback was cancelled."));
                if (record.state == TimedOut)
                    return Result<ReadyReadback>::Failure(ReadbackError(RenderReadbackErrors::TimedOut, "Readback timed out."));
                if (record.state == Failed)
                    return Result<ReadyReadback>::Failure(*record.failure);
                return Result<ReadyReadback>::Success({record.id, record.completion, record.payload});
            });
        }

        void ConsumeReady(const RenderReadbackId request) noexcept {
            const ReadbackRecord *record = core_.Find(request);
            if (record != nullptr && record->state == Ready)
                core_.Erase(request);
        }

        [[nodiscard]] Result<void> Discard(const RenderReadbackId request) {
            const auto result = operations_.Apply<Result<void>>(request, [this](const auto &record) {
                if (!IsTerminal(record.state) || record.pendingBytesCharged)
                    return this->InvalidTransition("Only fully retired terminal readbacks may be discarded.");
                return Result<void>::Success();
            });
            if (result.HasValue())
                core_.Erase(request);
            return result;
        }

        [[nodiscard]] RenderReadbackSnapshot Snapshot() const noexcept {
            return detail::MakeQueueSnapshot<RenderReadbackSnapshot>(core_, Submitted, Ready, IsTerminal, [this](auto &snapshot) {
                snapshot.pendingBytes = core_.PendingBytes();
                snapshot.retainedResultBytes = retained_->bytes.load(std::memory_order_relaxed);
            });
        }

        void StopAdmission() noexcept {
            operations_.StopAdmission();
        }

        void Shutdown() noexcept {
            operations_.Shutdown();
        }

    private:
        [[nodiscard]] Result<void> CompleteRecord(ReadbackRecord &record, const std::span<const std::byte> mappedBytes) {
            if (record.state != Submitted && record.state != Cancelled && record.state != TimedOut)
                return InvalidTransition("Only submitted, cancelled, or timed-out backend work may complete.");
            if (record.state == Cancelled || record.state == TimedOut) {
                ReleasePending(record);
                return Result<void>::Success();
            }
            if (!record.completion.IsValid() || mappedBytes.size() != record.descriptor.byteCount)
                return Result<void>::Failure(ReadbackError(RenderReadbackErrors::MappingSizeMismatch,
                                                           "Mapped readback bytes do not match the exact admitted request size."));
            if (const std::size_t retainedBytes = retained_->bytes.load(std::memory_order_relaxed);
                retainedBytes > core_.LimitsValue().maximumRetainedResultBytes ||
                mappedBytes.size() > core_.LimitsValue().maximumRetainedResultBytes - retainedBytes)
                return Result<void>::Failure(
                    ReadbackError(RenderReadbackErrors::CapacityExceeded, "Consumer-retained readback results leave no result capacity."));
            try {
                std::vector<std::byte> owned(mappedBytes.begin(), mappedBytes.end());
                record.payload = std::make_shared<RetainedPayload>(retained_, std::move(owned));
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(
                    ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback result storage allocation failed."));
            } catch (const std::length_error &) {
                return Result<void>::Failure(
                    ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback result size cannot be represented."));
            }
            ReleasePending(record);
            record.state = Ready;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> InvalidTransition(std::string message) const {
            return Result<void>::Failure(ReadbackError(RenderReadbackErrors::InvalidTransition, std::move(message)));
        }

        void ReleasePending(ReadbackRecord &record) noexcept {
            core_.ReleasePending(record);
        }

        std::shared_ptr<RetainedAccounting> retained_{std::make_shared<RetainedAccounting>()};
        Core core_;
        OperationAdapter operations_;
    };

    /** @copydoc RenderReadbackQueue::Create */
    Result<std::unique_ptr<RenderReadbackQueue>> RenderReadbackQueue::Create(const RenderResourceOwnerId renderer,
                                                                             const RenderReadbackLimits &limits) {
        if (!renderer.IsValid() || !limits.IsValid())
            return Result<std::unique_ptr<RenderReadbackQueue>>::Failure(
                ReadbackError(RenderReadbackErrors::InvalidConfiguration, "Readback owner identity or finite limits are invalid."));
        try {
            // std::make_unique cannot access RenderReadbackQueue's private constructor.
            return Result<std::unique_ptr<RenderReadbackQueue>>::Success(
                std::unique_ptr<RenderReadbackQueue>(new RenderReadbackQueue(std::make_unique<Impl>(renderer, limits))));  // NOSONAR
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<RenderReadbackQueue>>::Failure(
                ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback queue allocation failed."));
        } catch (const std::length_error &) {
            return Result<std::unique_ptr<RenderReadbackQueue>>::Failure(
                ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback metadata capacity cannot be represented."));
        }
    }

    RenderReadbackQueue::RenderReadbackQueue(std::unique_ptr<Impl> implementation) noexcept : implementation_(std::move(implementation)) {}

    RenderReadbackQueue::~RenderReadbackQueue() = default;

    /** @copydoc RenderReadbackQueue::Request */
    Result<RenderReadbackId> RenderReadbackQueue::Request(const RenderReadbackDescriptor &descriptor) {
        return implementation_->Request(descriptor);
    }

    /** @copydoc RenderReadbackQueue::MarkSubmitted */
    Result<void> RenderReadbackQueue::MarkSubmitted(const RenderReadbackId request, const RenderTimelinePoint completion) {
        return implementation_->MarkSubmitted(request, completion);
    }

    /** @copydoc RenderReadbackQueue::Complete */
    Result<void> RenderReadbackQueue::Complete(const RenderReadbackId request, const std::span<const std::byte> mappedBytes) {
        return implementation_->Complete(request, mappedBytes);
    }

    /** @copydoc RenderReadbackQueue::Fail */
    Result<void> RenderReadbackQueue::Fail(const RenderReadbackId request, const Error &error) {
        return implementation_->Fail(request, error);
    }

    /** @copydoc RenderReadbackQueue::Cancel */
    Result<void> RenderReadbackQueue::Cancel(const RenderReadbackId request) {
        return implementation_->Cancel(request);
    }

    /** @copydoc RenderReadbackQueue::Timeout */
    Result<void> RenderReadbackQueue::Timeout(const RenderReadbackId request) {
        return implementation_->Timeout(request);
    }

    /** @copydoc RenderReadbackQueue::Retire */
    Result<void> RenderReadbackQueue::Retire(const RenderReadbackId request) {
        return implementation_->Retire(request);
    }

    /** @copydoc RenderReadbackQueue::State */
    Result<RenderReadbackState> RenderReadbackQueue::State(const RenderReadbackId request) const {
        return implementation_->State(request);
    }

    /** @copydoc RenderReadbackQueue::Acquire */
    Result<RenderReadbackResult> RenderReadbackQueue::Acquire(const RenderReadbackId request) {
        auto ready = implementation_->AcquireView(request);
        if (ready.HasError())
            return Result<RenderReadbackResult>::Failure(ready.ErrorValue());
        try {
            auto resultImplementation =
                std::make_unique<RenderReadbackResult::Impl>(ready.Value().id, ready.Value().completion, ready.Value().payload);
            RenderReadbackResult result(std::move(resultImplementation));
            implementation_->ConsumeReady(request);
            return Result<RenderReadbackResult>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<RenderReadbackResult>::Failure(
                ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback lease allocation failed."));
        }
    }

    /** @copydoc RenderReadbackQueue::Discard */
    Result<void> RenderReadbackQueue::Discard(const RenderReadbackId request) {
        return implementation_->Discard(request);
    }

    /** @copydoc RenderReadbackQueue::Snapshot */
    RenderReadbackSnapshot RenderReadbackQueue::Snapshot() const noexcept {
        return implementation_->Snapshot();
    }

    /** @copydoc RenderReadbackQueue::StopAdmission */
    void RenderReadbackQueue::StopAdmission() noexcept {
        implementation_->StopAdmission();
    }

    /** @copydoc RenderReadbackQueue::Shutdown */
    void RenderReadbackQueue::Shutdown() noexcept {
        implementation_->Shutdown();
    }
}  // namespace Horo::Render
