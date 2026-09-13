#include "Horo/Runtime/Render/RenderReadback.h"

#include "Horo/Runtime/Render/RenderReadbackErrors.h"

#include <algorithm>
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
        [[nodiscard]] Error ReadbackError(const ErrorCodeDescriptor &descriptor, std::string message) {
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
            return state == RenderReadbackState::Failed || state == RenderReadbackState::Cancelled ||
                   state == RenderReadbackState::TimedOut;
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
    public:
        Impl(const RenderResourceOwnerId renderer, const RenderReadbackLimits &limits)
            : renderer_(renderer), limits_(limits), retained_(std::make_shared<RetainedAccounting>()) {
            records_.reserve(limits.maximumRequests);
        }

        [[nodiscard]] Result<RenderReadbackId> Request(const RenderReadbackDescriptor &descriptor) {
            if (!accepting_)
                return Result<RenderReadbackId>::Failure(ReadbackError(RenderReadbackErrors::Stopped, "Readback admission is stopped."));
            if (!descriptor.IsValid() || SourceOwner(descriptor.source) != renderer_ ||
                descriptor.byteCount > limits_.maximumRequestBytes || descriptor.alignment > limits_.maximumAlignment) {
                return Result<RenderReadbackId>::Failure(
                    ReadbackError(RenderReadbackErrors::InvalidDescriptor,
                                  "Readback descriptor exceeds the configured byte or alignment bounds."));
            }
            const std::size_t retainedBytes = retained_->bytes.load(std::memory_order_relaxed);
            const std::size_t retainedAndPending = retainedBytes + pendingBytes_;
            if (records_.size() >= limits_.maximumRequests || descriptor.byteCount > limits_.maximumPendingBytes - pendingBytes_ ||
                retainedAndPending > limits_.maximumRetainedResultBytes ||
                descriptor.byteCount > limits_.maximumRetainedResultBytes - retainedAndPending) {
                ++failedAdmissionCount_;
                return Result<RenderReadbackId>::Failure(
                    ReadbackError(RenderReadbackErrors::CapacityExceeded,
                                  "Readback metadata, staging, or retained-result capacity is exhausted."));
            }
            if (nextId_ == std::numeric_limits<std::uint64_t>::max()) {
                ++failedAdmissionCount_;
                return Result<RenderReadbackId>::Failure(
                    ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback request identity space is exhausted."));
            }
            const RenderReadbackId id{renderer_, nextId_++};
            records_.push_back({.id = id, .descriptor = descriptor});
            pendingBytes_ += descriptor.byteCount;
            return Result<RenderReadbackId>::Success(id);
        }

        [[nodiscard]] Result<void> MarkSubmitted(const RenderReadbackId request, const RenderTimelinePoint completion) {
            ReadbackRecord *record = Find(request);
            if (record == nullptr)
                return InvalidRequest();
            if (record->state != RenderReadbackState::Pending || !completion.IsValid())
                return InvalidTransition("Only a pending readback may receive a valid completion point.");
            record->state = RenderReadbackState::Submitted;
            record->completion = completion;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> Complete(const RenderReadbackId request, const std::span<const std::byte> mappedBytes) {
            ReadbackRecord *record = Find(request);
            if (record == nullptr)
                return InvalidRequest();
            if (record->state != RenderReadbackState::Submitted && record->state != RenderReadbackState::Cancelled &&
                record->state != RenderReadbackState::TimedOut) {
                return InvalidTransition("Only submitted, cancelled, or timed-out backend work may complete.");
            }
            if (!record->completion.IsValid() || mappedBytes.size() != record->descriptor.byteCount) {
                return Result<void>::Failure(ReadbackError(RenderReadbackErrors::MappingSizeMismatch,
                                                           "Mapped readback bytes do not match the exact admitted request size."));
            }
            if (record->state == RenderReadbackState::Cancelled || record->state == RenderReadbackState::TimedOut) {
                ReleasePending(*record);
                return Result<void>::Success();
            }
            const std::size_t retainedBytes = retained_->bytes.load(std::memory_order_relaxed);
            if (mappedBytes.size() > limits_.maximumRetainedResultBytes - std::min(retainedBytes, limits_.maximumRetainedResultBytes))
                return Result<void>::Failure(
                    ReadbackError(RenderReadbackErrors::CapacityExceeded, "Consumer-retained readback results leave no result capacity."));
            try {
                std::vector<std::byte> owned(mappedBytes.begin(), mappedBytes.end());
                record->payload = std::make_shared<RetainedPayload>(retained_, std::move(owned));
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(
                    ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback result storage allocation failed."));
            } catch (const std::length_error &) {
                return Result<void>::Failure(
                    ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback result size cannot be represented."));
            }
            ReleasePending(*record);
            record->state = RenderReadbackState::Ready;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> Fail(const RenderReadbackId request, const Error &error) {
            ReadbackRecord *record = Find(request);
            if (record == nullptr)
                return InvalidRequest();
            if (record->state == RenderReadbackState::Cancelled || record->state == RenderReadbackState::TimedOut) {
                ReleasePending(*record);
                return Result<void>::Success();
            }
            if (record->state != RenderReadbackState::Pending && record->state != RenderReadbackState::Submitted)
                return InvalidTransition("Only pending or submitted readback work may fail.");
            ReleasePending(*record);
            record->failure = error;
            record->state = RenderReadbackState::Failed;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> Cancel(const RenderReadbackId request) {
            ReadbackRecord *record = Find(request);
            if (record == nullptr)
                return InvalidRequest();
            if (record->state == RenderReadbackState::Cancelled)
                return Result<void>::Success();
            if (record->state != RenderReadbackState::Pending && record->state != RenderReadbackState::Submitted)
                return InvalidTransition("Only pending or submitted readback work may be cancelled.");
            if (record->state == RenderReadbackState::Pending)
                ReleasePending(*record);
            record->state = RenderReadbackState::Cancelled;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> Timeout(const RenderReadbackId request) {
            ReadbackRecord *record = Find(request);
            if (record == nullptr)
                return InvalidRequest();
            if (record->state == RenderReadbackState::TimedOut)
                return Result<void>::Success();
            if (record->state != RenderReadbackState::Pending && record->state != RenderReadbackState::Submitted)
                return InvalidTransition("Only pending or submitted readback work may time out.");
            if (record->state == RenderReadbackState::Pending)
                ReleasePending(*record);
            record->state = RenderReadbackState::TimedOut;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> Retire(const RenderReadbackId request) {
            ReadbackRecord *record = Find(request);
            if (record == nullptr)
                return InvalidRequest();
            if (record->state != RenderReadbackState::Cancelled && record->state != RenderReadbackState::TimedOut)
                return InvalidTransition("Only cancelled or timed-out submitted work may retire without publication.");
            ReleasePending(*record);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<RenderReadbackState> State(const RenderReadbackId request) const {
            const ReadbackRecord *record = Find(request);
            if (record == nullptr)
                return Result<RenderReadbackState>::Failure(InvalidRequestError());
            return Result<RenderReadbackState>::Success(record->state);
        }

        [[nodiscard]] Result<ReadyReadback> AcquireView(const RenderReadbackId request) const {
            auto record = FindIterator(request);
            if (record == records_.end())
                return Result<ReadyReadback>::Failure(InvalidRequestError());
            if (record->state == RenderReadbackState::Pending || record->state == RenderReadbackState::Submitted)
                return Result<ReadyReadback>::Failure(
                    ReadbackError(RenderReadbackErrors::ResultPending, "Readback result has not completed."));
            if (record->state == RenderReadbackState::Cancelled)
                return Result<ReadyReadback>::Failure(ReadbackError(RenderReadbackErrors::Cancelled, "Readback was cancelled."));
            if (record->state == RenderReadbackState::TimedOut)
                return Result<ReadyReadback>::Failure(ReadbackError(RenderReadbackErrors::TimedOut, "Readback timed out."));
            if (record->state == RenderReadbackState::Failed)
                return Result<ReadyReadback>::Failure(*record->failure);
            return Result<ReadyReadback>::Success({record->id, record->completion, record->payload});
        }

        void ConsumeReady(const RenderReadbackId request) noexcept {
            const auto record = FindIterator(request);
            if (record != records_.end() && record->state == RenderReadbackState::Ready)
                records_.erase(record);
        }

        [[nodiscard]] Result<void> Discard(const RenderReadbackId request) {
            auto record = FindIterator(request);
            if (record == records_.end())
                return InvalidRequest();
            if (!IsTerminal(record->state) || record->pendingBytesCharged)
                return InvalidTransition("Only fully retired terminal readbacks may be discarded.");
            records_.erase(record);
            return Result<void>::Success();
        }

        [[nodiscard]] RenderReadbackSnapshot Snapshot() const noexcept {
            RenderReadbackSnapshot snapshot{.pendingBytes = pendingBytes_,
                                            .retainedResultBytes = retained_->bytes.load(std::memory_order_relaxed),
                                            .requestCount = static_cast<std::uint32_t>(records_.size()),
                                            .failedAdmissionCount = failedAdmissionCount_,
                                            .acceptingRequests = accepting_};
            for (const ReadbackRecord &record : records_) {
                snapshot.submittedCount += record.state == RenderReadbackState::Submitted;
                snapshot.readyCount += record.state == RenderReadbackState::Ready;
                snapshot.terminalCount += IsTerminal(record.state);
            }
            return snapshot;
        }

        void StopAdmission() noexcept {
            accepting_ = false;
        }

        void Shutdown() noexcept {
            accepting_ = false;
            records_.clear();
            pendingBytes_ = 0;
        }

    private:
        [[nodiscard]] Error InvalidRequestError() const {
            return ReadbackError(RenderReadbackErrors::InvalidRequest, "Readback identity is malformed, foreign, or no longer tracked.");
        }

        [[nodiscard]] Result<void> InvalidRequest() const {
            return Result<void>::Failure(InvalidRequestError());
        }

        [[nodiscard]] Result<void> InvalidTransition(std::string message) const {
            return Result<void>::Failure(ReadbackError(RenderReadbackErrors::InvalidTransition, std::move(message)));
        }

        [[nodiscard]] auto FindIterator(const RenderReadbackId request) {
            if (!request.IsValid() || request.renderer != renderer_)
                return records_.end();
            return std::find_if(records_.begin(), records_.end(), [request](const ReadbackRecord &record) {
                return record.id == request;
            });
        }

        [[nodiscard]] auto FindIterator(const RenderReadbackId request) const {
            if (!request.IsValid() || request.renderer != renderer_)
                return records_.end();
            return std::find_if(records_.begin(), records_.end(), [request](const ReadbackRecord &record) {
                return record.id == request;
            });
        }

        [[nodiscard]] ReadbackRecord *Find(const RenderReadbackId request) {
            const auto found = FindIterator(request);
            return found == records_.end() ? nullptr : &*found;
        }

        [[nodiscard]] const ReadbackRecord *Find(const RenderReadbackId request) const {
            if (!request.IsValid() || request.renderer != renderer_)
                return nullptr;
            const auto found = std::find_if(records_.begin(), records_.end(), [request](const ReadbackRecord &record) {
                return record.id == request;
            });
            return found == records_.end() ? nullptr : &*found;
        }

        void ReleasePending(ReadbackRecord &record) noexcept {
            if (!record.pendingBytesCharged)
                return;
            pendingBytes_ -= record.descriptor.byteCount;
            record.pendingBytesCharged = false;
        }

        RenderResourceOwnerId renderer_;
        RenderReadbackLimits limits_;
        std::shared_ptr<RetainedAccounting> retained_;
        std::vector<ReadbackRecord> records_;
        std::size_t pendingBytes_{0};
        std::uint64_t nextId_{1};
        std::uint64_t failedAdmissionCount_{0};
        bool accepting_{true};
    };

    /** @copydoc RenderReadbackQueue::Create */
    Result<std::unique_ptr<RenderReadbackQueue>> RenderReadbackQueue::Create(const RenderResourceOwnerId renderer,
                                                                             const RenderReadbackLimits &limits) {
        if (!renderer.IsValid() || !limits.IsValid())
            return Result<std::unique_ptr<RenderReadbackQueue>>::Failure(
                ReadbackError(RenderReadbackErrors::InvalidConfiguration, "Readback owner identity or finite limits are invalid."));
        try {
            return Result<std::unique_ptr<RenderReadbackQueue>>::Success(
                std::unique_ptr<RenderReadbackQueue>(new RenderReadbackQueue(std::make_unique<Impl>(renderer, limits))));
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
