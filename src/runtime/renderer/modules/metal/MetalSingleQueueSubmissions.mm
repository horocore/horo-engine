#include "MetalSingleQueueSubmissions.h"

#include "MetalRenderBackendErrors.h"
#include "MetalVfxGpuGraphInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
    namespace {
        Result<void> Unsupported(const char *message) {
            return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution, message));
        }
    }  // namespace

    Result<void> MetalSingleQueueSubmissions::Initialize(const std::span<const RenderQueueAssignment> assignments,
                                                         const std::uint64_t incarnation) {
        if (assignments.empty() || !incarnation)
            return Unsupported("Metal requires an issued single effective queue and device incarnation.");
        const auto queue = assignments.front().queue;
        for (const auto assignment : assignments)
            if (!assignment.IsValid() || assignment.queue != queue)
                return Unsupported("Metal does not implement distinct native graph queues.");
        entries_.reserve(MaximumEntries);
        queue_ = queue;
        incarnation_ = incarnation;
        return Result<void>::Success();
    }

    bool MetalSingleQueueSubmissions::KnownWait(const RenderTimelinePoint point, id<MTLCommandBuffer> commands,
                                                const std::span<const RenderGraphQueueSubmission> earlier) const noexcept {
        if (point.queue != queue_)
            return false;
        for (const auto &entry : entries_)
            if (entry.point == point && (entry.accepted || entry.commands == commands))
                return true;
        return std::ranges::any_of(earlier, [point](const auto &entry) {
            return entry.submission.signal == point;
        });
    }

    Result<void> MetalSingleQueueSubmissions::ValidatePoint(const RenderQueueSubmission &submission, const std::uint64_t order,
                                                            const std::uint64_t signal, id<MTLCommandBuffer> commands,
                                                            const std::span<const RenderGraphQueueSubmission> earlier) const {
        if (!submission.IsValid() || submission.queue != queue_ || submission.order.value <= order || submission.signal.value <= signal)
            return Unsupported("Metal requires exact increasing frontend-issued single-queue submission points.");
        for (const auto wait : submission.waits)
            if (!KnownWait(wait, commands, earlier))
                return Unsupported("Metal graph wait names an unknown, expired, foreign or future native submission.");
        return Result<void>::Success();
    }

    Result<void> MetalSingleQueueSubmissions::Validate(const RenderGraphExecutionRequest &request, id<MTLCommandBuffer> commands) const {
        if (request.submissions.empty())
            return Result<void>::Success();
        if (!commands || !queue_.IsValid() || request.submissions.size() > RenderGraphSubmissionLimits::MaximumSubmissions)
            return Unsupported("Metal graph submission envelope is unavailable or exceeds its admitted graph bounds.");
        auto order = lastOrder_, signal = lastSignal_;
        for (std::size_t index = 0; index < request.submissions.size(); ++index) {
            const auto &submission = request.submissions[index].submission;
            if (const auto valid = ValidatePoint(submission, order, signal, commands, request.submissions.first(index)); valid.HasError())
                return valid;
            order = submission.order.value;
            signal = submission.signal.value;
        }
        return Result<void>::Success();
    }

    /** @brief Eviction loses observation availability, never invents completion for an unknown point or timeline gap. */
    bool MetalSingleQueueSubmissions::MakeRoom(const std::size_t count) noexcept {
        while (count > entries_.capacity() - entries_.size()) {
            if (entries_.empty() || !entries_.front().accepted || !MetalVfxGpuComplete(entries_.front().commands))
                return false;
            expiredThrough_ = entries_.front().point.value;
            entries_.erase(entries_.begin());
        }
        return true;
    }

    Result<void> MetalSingleQueueSubmissions::Capture(const RenderGraphExecutionRequest &request, id<MTLCommandBuffer> commands) {
        if (const auto valid = Validate(request, commands); valid.HasError())
            return valid;
        if (!MakeRoom(request.submissions.size()))
            return Unsupported("Pending Metal native commands prevent bounded submission receipt admission.");
        for (const auto &entry : request.submissions) {
            entries_.push_back({entry.submission.signal, commands, false});
            lastOrder_ = entry.submission.order.value;
            lastSignal_ = entry.submission.signal.value;
        }
        return Result<void>::Success();
    }

    void MetalSingleQueueSubmissions::Commit(id<MTLCommandBuffer> commands) noexcept {
        for (auto &entry : entries_)
            if (entry.commands == commands)
                entry.accepted = true;
    }

    void MetalSingleQueueSubmissions::Abort(id<MTLCommandBuffer> commands) noexcept {
        std::erase_if(entries_, [commands](const Entry &entry) {
            return entry.commands == commands && !entry.accepted;
        });
    }

    Result<bool> MetalSingleQueueSubmissions::Observe(const RenderCpuWait &wait) const {
        if (!wait.IsValid() || wait.expectedDeviceIncarnation != incarnation_ || wait.completion.queue != queue_)
            return Result<bool>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        if (wait.purpose != RenderCpuWaitPurpose::Poll)
            return Result<bool>::Failure(
                MakeError(MetalBackendErrors::UnsupportedGraphExecution,
                          "Metal single-queue observation supports nonblocking Poll; ordinary rendering never waits on the CPU."));
        const auto found = std::ranges::find(entries_, wait.completion, &Entry::point);
        if (found == entries_.end() || !found->accepted)
            return Result<bool>::Failure(
                MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                          "Metal completion point is unknown, expired, cancelled or not accepted by the native queue."));
        if (found->commands.status == MTLCommandBufferStatusError)
            return Result<bool>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
        return Result<bool>::Success(found->commands.status == MTLCommandBufferStatusCompleted);
    }

    void MetalSingleQueueSubmissions::Clear() noexcept {
        entries_.clear();
        queue_ = {};
        incarnation_ = lastOrder_ = lastSignal_ = expiredThrough_ = 0;
    }
}  // namespace Horo::Render::Detail
