#include "Horo/Runtime/Render/RenderUpload.h"

#include "BoundedRenderQueueCore.h"
#include "Horo/Runtime/Render/RenderUploadErrors.h"

#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Render {
    namespace {
        [[nodiscard]] Error UploadError(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] bool IsDestinationValid(const RenderUploadDestination &destination) noexcept {
            return std::visit([](const auto handle) {
                return handle.IsValid();
            }, destination);
        }

        [[nodiscard]] RenderResourceOwnerId DestinationOwner(const RenderUploadDestination &destination) noexcept {
            return std::visit([](const auto handle) {
                return handle.owner;
            }, destination);
        }

        [[nodiscard]] bool IsTerminal(const RenderUploadState state) noexcept {
            using enum RenderUploadState;
            return state == Failed || state == Cancelled || state == TimedOut || state == Ready;
        }

        [[nodiscard]] Error UploadAdmissionError(const detail::QueueAdmissionFailure failure) {
            using enum detail::QueueAdmissionFailure;
            switch (failure) {
                case Capacity:
                    return UploadError(RenderUploadErrors::CapacityExceeded, "Upload staging or metadata capacity is exhausted.");
                case Identity:
                    return UploadError(RenderUploadErrors::CapacityExceeded, "Upload request identity space is exhausted.");
                case Allocation:
                    return UploadError(RenderUploadErrors::CapacityExceeded, "Upload staging allocation failed.");
                case Length:
                    return UploadError(RenderUploadErrors::CapacityExceeded, "Upload payload size cannot be represented.");
            }
            return UploadError(RenderUploadErrors::CapacityExceeded);
        }
    }  // namespace

    /** @copydoc RenderUploadDescriptor::IsValid */
    bool RenderUploadDescriptor::IsValid() const noexcept {
        return IsDestinationValid(destination) && destinationByteOffset <= std::numeric_limits<std::size_t>::max() - byteCount &&
               byteCount > 0 && alignment > 0 && (alignment & (alignment - 1U)) == 0 && timeout.count() > 0;
    }

    struct UploadRecord {
        RenderUploadId id;
        RenderUploadDescriptor descriptor;
        std::vector<std::byte> payload;
        RenderUploadState state{RenderUploadState::Pending};
        RenderTimelinePoint completion;
        std::optional<Error> failure;
        bool pendingBytesCharged{true};
    };

    class RenderUploadQueue::Impl final {
        using enum RenderUploadState;
        using Core = detail::BoundedRenderQueueCore<UploadRecord, RenderUploadId, RenderUploadLimits>;
        using OperationAdapter = detail::QueueOperationAdapter<Core>;

    public:
        Impl(const RenderResourceOwnerId renderer, const RenderUploadLimits &limits)
            : core_(renderer, limits), operations_(core_, RenderUploadErrors::WrongThread, RenderUploadErrors::InvalidRequest,
                                                   "Upload identity is malformed, foreign, or no longer tracked.") {}

        [[nodiscard]] Result<RenderUploadId> Request(const RenderUploadDescriptor &descriptor, const std::span<const std::byte> bytes) {
            if (!core_.OnOwnerThread())
                return Result<RenderUploadId>::Failure(UploadError(RenderUploadErrors::WrongThread));
            if (!core_.Accepting())
                return Result<RenderUploadId>::Failure(UploadError(RenderUploadErrors::Stopped, "Upload admission is stopped."));
            const RenderUploadLimits &limits = core_.LimitsValue();
            if (!descriptor.IsValid() || DestinationOwner(descriptor.destination) != core_.Renderer() ||
                descriptor.byteCount > limits.maximumRequestBytes || descriptor.alignment > limits.maximumAlignment) {
                return Result<RenderUploadId>::Failure(
                    UploadError(RenderUploadErrors::InvalidDescriptor, "Upload descriptor exceeds the configured bounds."));
            }
            if (bytes.size() != descriptor.byteCount)
                return Result<RenderUploadId>::Failure(
                    UploadError(RenderUploadErrors::PayloadSizeMismatch, "Upload payload does not match the admitted byte count."));
            auto admitted = core_.Admit(descriptor.byteCount <= limits.maximumPendingBytes - core_.PendingBytes(),
                                        [descriptor, bytes](const RenderUploadId id) {
                return UploadRecord{.id = id, .descriptor = descriptor, .payload = {bytes.begin(), bytes.end()}};
            }, UploadAdmissionError);
            if (admitted.HasValue())
                core_.ChargePending(descriptor.byteCount);
            return admitted;
        }

        [[nodiscard]] Result<std::span<const std::byte>> Payload(const RenderUploadId request) const {
            return operations_.Apply<Result<std::span<const std::byte>>>(request, [](const auto &record) {
                if (record.state != Pending)
                    return Result<std::span<const std::byte>>::Failure(
                        UploadError(RenderUploadErrors::InvalidTransition, "Upload payload is only available before submission."));
                return Result<std::span<const std::byte>>::Success(record.payload);
            });
        }

        [[nodiscard]] Result<RenderUploadState> State(const RenderUploadId request) const {
            return operations_.Apply<Result<RenderUploadState>>(request, [](const auto &record) {
                return Result<RenderUploadState>::Success(record.state);
            });
        }

        [[nodiscard]] Result<bool> IsReady(const RenderUploadId request) const {
            const auto state = State(request);
            return detail::QueueStateIsReady(state, Ready);
        }

        [[nodiscard]] Result<void> MarkSubmitted(const RenderUploadId request, const RenderTimelinePoint completion) {
            return operations_.Apply<Result<void>>(request, [completion, this](auto &record) {
                if (record.state != Pending || !completion.IsValid())
                    return InvalidTransition("Only a pending upload may receive a valid completion point.");
                record.completion = completion;
                record.state = Submitted;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Complete(const RenderUploadId request) {
            return operations_.Apply<Result<void>>(request, [this](auto &record) {
                if (record.state == Cancelled || record.state == TimedOut) {
                    ReleasePayload(record);
                    return Result<void>::Success();
                }
                if (record.state != Submitted || !record.completion.IsValid())
                    return InvalidTransition("Only submitted upload work may complete.");
                ReleasePayload(record);
                record.state = Ready;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Fail(const RenderUploadId request, const Error &error) {
            return operations_.Apply<Result<void>>(request, [this, &error](auto &record) {
                if (record.state == Cancelled || record.state == TimedOut) {
                    ReleasePayload(record);
                    return Result<void>::Success();
                }
                if (record.state != Pending && record.state != Submitted)
                    return InvalidTransition("Only pending or submitted upload work may fail.");
                ReleasePayload(record);
                record.failure = error;
                record.state = Failed;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Cancel(const RenderUploadId request) {
            return operations_.Apply<Result<void>>(request, [this](auto &record) {
                return detail::TransitionQueueRecord(record, Cancelled, Pending, Submitted, [this] {
                    return InvalidTransition("Only pending or submitted upload work may be cancelled.");
                }, [this](auto &pendingRecord) {
                    ReleasePayload(pendingRecord);
                });
            });
        }

        [[nodiscard]] Result<void> Timeout(const RenderUploadId request) {
            return operations_.Apply<Result<void>>(request, [this](auto &record) {
                return detail::TransitionQueueRecord(record, TimedOut, Pending, Submitted, [this] {
                    return InvalidTransition("Only pending or submitted upload work may time out.");
                }, [this](auto &pendingRecord) {
                    ReleasePayload(pendingRecord);
                });
            });
        }

        [[nodiscard]] Result<void> Retire(const RenderUploadId request) {
            return operations_.Apply<Result<void>>(request, [this](auto &record) {
                if (record.state != Cancelled && record.state != TimedOut)
                    return InvalidTransition("Only cancelled or timed-out uploads may retire without publication.");
                ReleasePayload(record);
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Discard(const RenderUploadId request) {
            const auto result = operations_.Apply<Result<void>>(request, [this](const auto &record) {
                if (!IsTerminal(record.state) || record.pendingBytesCharged)
                    return InvalidTransition("Only completed and fully retired uploads may be discarded.");
                return Result<void>::Success();
            });
            if (result.HasValue())
                core_.Erase(request);
            return result;
        }

        [[nodiscard]] RenderUploadSnapshot Snapshot() const noexcept {
            return detail::MakeQueueSnapshot<RenderUploadSnapshot>(core_, Submitted, Ready, IsTerminal, [this](auto &snapshot) {
                snapshot.pendingBytes = core_.PendingBytes();
            });
        }

        void StopAdmission() noexcept {
            operations_.StopAdmission();
        }

        void Shutdown() noexcept {
            operations_.Shutdown();
        }

    private:
        [[nodiscard]] Result<void> InvalidTransition(std::string message) const {
            return Result<void>::Failure(UploadError(RenderUploadErrors::InvalidTransition, std::move(message)));
        }

        void ReleasePending(UploadRecord &record) noexcept {
            core_.ReleasePending(record);
        }

        void ReleasePayload(UploadRecord &record) noexcept {
            ReleasePending(record);
            record.payload.clear();
        }

        Core core_;
        OperationAdapter operations_;
    };

    /** @copydoc RenderUploadQueue::Create */
    Result<std::unique_ptr<RenderUploadQueue>> RenderUploadQueue::Create(const RenderResourceOwnerId renderer,
                                                                         const RenderUploadLimits &limits) {
        if (!renderer.IsValid() || !limits.IsValid())
            return Result<std::unique_ptr<RenderUploadQueue>>::Failure(
                UploadError(RenderUploadErrors::InvalidConfiguration, "Upload owner identity or finite limits are invalid."));
        try {
            return Result<std::unique_ptr<RenderUploadQueue>>::Success(
                std::unique_ptr<RenderUploadQueue>(new RenderUploadQueue(std::make_unique<Impl>(renderer, limits))));  // NOSONAR
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<RenderUploadQueue>>::Failure(
                UploadError(RenderUploadErrors::CapacityExceeded, "Upload queue allocation failed."));
        } catch (const std::length_error &) {
            return Result<std::unique_ptr<RenderUploadQueue>>::Failure(
                UploadError(RenderUploadErrors::CapacityExceeded, "Upload metadata capacity cannot be represented."));
        }
    }

    RenderUploadQueue::RenderUploadQueue(std::unique_ptr<Impl> implementation) noexcept : implementation_(std::move(implementation)) {}

    RenderUploadQueue::~RenderUploadQueue() = default;

    /** @copydoc RenderUploadQueue::Request */
    Result<RenderUploadId> RenderUploadQueue::Request(const RenderUploadDescriptor &descriptor, const std::span<const std::byte> bytes) {
        return implementation_->Request(descriptor, bytes);
    }

    /** @copydoc RenderUploadQueue::Payload */
    Result<std::span<const std::byte>> RenderUploadQueue::Payload(const RenderUploadId request) const {
        return implementation_->Payload(request);
    }

    /** @copydoc RenderUploadQueue::State */
    Result<RenderUploadState> RenderUploadQueue::State(const RenderUploadId request) const {
        return implementation_->State(request);
    }

    /** @copydoc RenderUploadQueue::IsReady */
    Result<bool> RenderUploadQueue::IsReady(const RenderUploadId request) const {
        return implementation_->IsReady(request);
    }

    /** @copydoc RenderUploadQueue::MarkSubmitted */
    Result<void> RenderUploadQueue::MarkSubmitted(const RenderUploadId request, const RenderTimelinePoint completion) {
        return implementation_->MarkSubmitted(request, completion);
    }

    /** @copydoc RenderUploadQueue::Complete */
    Result<void> RenderUploadQueue::Complete(const RenderUploadId request) {
        return implementation_->Complete(request);
    }

    /** @copydoc RenderUploadQueue::Fail */
    Result<void> RenderUploadQueue::Fail(const RenderUploadId request, const Error &error) {
        return implementation_->Fail(request, error);
    }

    /** @copydoc RenderUploadQueue::Cancel */
    Result<void> RenderUploadQueue::Cancel(const RenderUploadId request) {
        return implementation_->Cancel(request);
    }

    /** @copydoc RenderUploadQueue::Timeout */
    Result<void> RenderUploadQueue::Timeout(const RenderUploadId request) {
        return implementation_->Timeout(request);
    }

    /** @copydoc RenderUploadQueue::Retire */
    Result<void> RenderUploadQueue::Retire(const RenderUploadId request) {
        return implementation_->Retire(request);
    }

    /** @copydoc RenderUploadQueue::Discard */
    Result<void> RenderUploadQueue::Discard(const RenderUploadId request) {
        return implementation_->Discard(request);
    }

    /** @copydoc RenderUploadQueue::Snapshot */
    RenderUploadSnapshot RenderUploadQueue::Snapshot() const noexcept {
        return implementation_->Snapshot();
    }

    /** @copydoc RenderUploadQueue::StopAdmission */
    void RenderUploadQueue::StopAdmission() noexcept {
        implementation_->StopAdmission();
    }

    /** @copydoc RenderUploadQueue::Shutdown */
    void RenderUploadQueue::Shutdown() noexcept {
        implementation_->Shutdown();
    }
}  // namespace Horo::Render
