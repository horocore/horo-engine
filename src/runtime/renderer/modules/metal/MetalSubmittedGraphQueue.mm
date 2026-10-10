#include "MetalSubmittedGraphQueue.h"

#include "MetalRenderBackendErrors.h"

#include <algorithm>
#include <string>
#include <utility>

namespace Horo::Render::Detail {
    /** @copydoc MetalSubmittedGraphQueue::Initialize */
    void MetalSubmittedGraphQueue::Initialize(const std::uint32_t capacity) {
        commands_ = [[NSMutableArray alloc] initWithCapacity:capacity];
    }

    /** @copydoc MetalSubmittedGraphQueue::Count */
    std::size_t MetalSubmittedGraphQueue::Count() const noexcept {
        return commands_.count;
    }

    /** @copydoc MetalSubmittedGraphQueue::RecordingCount */
    std::size_t MetalSubmittedGraphQueue::RecordingCount() const noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(recordings_, [](const auto &recording) {
            return bool(recording);
        }));
    }

    /** @copydoc MetalSubmittedGraphQueue::Remember */
    void MetalSubmittedGraphQueue::Remember(id<MTLCommandBuffer> commands, IRenderGraphResourceLease *&lease,
                                            std::shared_ptr<MetalParallelRecording> &recording) {
        const std::size_t index = Count();
        leases_[index] = std::exchange(lease, nullptr);
        recordings_[index] = std::move(recording);
        [commands_ addObject:commands];
    }

    /** @brief Observes all worker errors before considering the final owner marker sufficient. */
    Result<bool> MetalSubmittedGraphQueue::OldestComplete() const {
        if (recordings_[0]) {
            const auto complete = recordings_[0]->NativeComplete();
            if (complete.HasError() || !complete.Value())
                return complete;
        }
        id<MTLCommandBuffer> submitted = commands_.firstObject;
        if (submitted.status == MTLCommandBufferStatusError) {
            const char *reason = submitted.error.localizedDescription.UTF8String;
            return Result<bool>::Failure(
                MakeError(MetalBackendErrors::CommandSubmissionFailed,
                          reason == nullptr ? "Metal GPU command submission failed; restart the backend." : std::string{reason}));
        }
        return Result<bool>::Success(submitted.status == MTLCommandBufferStatusCompleted);
    }

    /** @brief Retires one complete graph and preserves canonical retained submission order. */
    void MetalSubmittedGraphQueue::RetireOldest() noexcept {
        if (leases_[0] != nullptr)
            leases_[0]->Release();
        for (std::size_t index = 1; index < leases_.size(); ++index) {
            leases_[index - 1] = leases_[index];
            recordings_[index - 1] = std::move(recordings_[index]);
        }
        leases_.back() = nullptr;
        recordings_.back().reset();
        [commands_ removeObjectAtIndex:0];
    }

    /** @copydoc MetalSubmittedGraphQueue::Poll */
    Result<void> MetalSubmittedGraphQueue::Poll() {
        while (Count() != 0) {
            const auto complete = OldestComplete();
            if (complete.HasError())
                return Result<void>::Failure(complete.ErrorValue());
            if (!complete.Value())
                break;
            RetireOldest();
        }
        return Result<void>::Success();
    }

    /** @copydoc MetalSubmittedGraphQueue::ReleaseLeases */
    void MetalSubmittedGraphQueue::ReleaseLeases() noexcept {
        for (auto *&lease : leases_) {
            if (lease != nullptr) {
                lease->Release();
                lease = nullptr;
            }
        }
    }

    /** @copydoc MetalSubmittedGraphQueue::Clear */
    void MetalSubmittedGraphQueue::Clear() noexcept {
        for (auto &recording : recordings_)
            recording.reset();
        commands_ = nil;
    }
}  // namespace Horo::Render::Detail
