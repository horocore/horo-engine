#include "Horo/Runtime/Render/FramePacing.h"

#include "Horo/Runtime/Render/FramePacingErrors.h"
#include "Horo/Runtime/Render/RenderBackend.h"

#include <algorithm>
#include <limits>

namespace Horo::Render {
    namespace {
        constexpr std::int64_t Second = 1'000'000'000;
        constexpr std::int64_t MaximumClock = std::numeric_limits<std::int64_t>::max() - Second;

        /** @brief Checks exact and explicitly degraded present-mode facts without allocating. */
        [[nodiscard]] bool ValidMode(const ResolvedPresentMode &mode) noexcept {
            if (mode.requested > PresentMode::Immediate || mode.resolved > PresentMode::Immediate)
                return false;
            if (mode.resolution == PresentModeResolution::Exact)
                return mode.requested == mode.resolved && mode.preferenceIndex == 0;
            return mode.resolution == PresentModeResolution::DegradedFallback && mode.requested != mode.resolved &&
                   mode.preferenceIndex > 0 && mode.preferenceIndex < MaximumPresentModeEntries;
        }

        /** @brief Validates committed lifecycle facts before using them for host admission. */
        [[nodiscard]] bool ValidSurface(const RenderSurfaceSnapshot &surface) noexcept {
            if (!surface.surface.HasOwner() || surface.revision == 0 || surface.state > RenderSurfaceState::Closing)
                return false;
            if (surface.state == RenderSurfaceState::Ready &&
                (!surface.surface.IsAttachedGeneration() || !surface.active || surface.inFlightSequence.has_value()))
                return false;
            if (!surface.active)
                return true;
            return surface.active->extent.IsValid() && surface.active->displayRevision != 0 && ValidMode(surface.active->presentMode);
        }

        /** @brief Rejects older or contradictory publications before changing pacing baselines. */
        [[nodiscard]] bool StaleSurface(const RenderSurfaceSnapshot &candidate, const RenderSurfaceSnapshot &active) noexcept {
            return candidate.surface.owner != active.surface.owner || candidate.revision < active.revision ||
                   candidate.surface.generation < active.surface.generation ||
                   (candidate.revision == active.revision && candidate != active);
        }

        /** @brief Validates qualified display identity independently of timestamp ordering. */
        [[nodiscard]] bool ValidNativeIdentity(const NativePresentTiming &native, const RenderSurfaceId surface,
                                               const std::uint64_t frameNumber) noexcept {
            return native.surface == surface && native.frameNumber != 0 && native.frameNumber <= frameNumber;
        }

        /** @brief Validates native timestamps and explicit calibration span against observation time. */
        [[nodiscard]] bool ValidNativeClock(const NativePresentTiming &native, const Duration observed) noexcept {
            return native.displayTime.ToNanoseconds() > 0 && native.clockUncertainty.ToNanoseconds() >= 0 &&
                   native.clockUncertainty <= Duration::FromMilliseconds(1) && native.displayTime <= observed + native.clockUncertainty;
        }

        /** @brief Computes one refresh interval only when both native samples expose real refresh ordinals. */
        [[nodiscard]] Result<std::optional<std::int64_t>> NativeInterval(const NativePresentTiming &native,
                                                                         const std::optional<NativePresentTiming> &previous) {
            if (!previous)
                return Result<std::optional<std::int64_t>>::Success(std::nullopt);
            if (native.frameNumber <= previous->frameNumber || native.displayTime <= previous->displayTime)
                return Result<std::optional<std::int64_t>>::Failure(MakeError(FramePacingErrors::StaleEvidence));
            if (native.displayOrdinal == 0 || previous->displayOrdinal == 0)
                return Result<std::optional<std::int64_t>>::Success(std::nullopt);
            if (native.displayOrdinal <= previous->displayOrdinal)
                return Result<std::optional<std::int64_t>>::Failure(MakeError(FramePacingErrors::StaleEvidence));
            const auto elapsed = (native.displayTime - previous->displayTime).ToNanoseconds();
            const auto delta = native.displayOrdinal - previous->displayOrdinal;
            if (delta > static_cast<std::uint64_t>(elapsed))
                return Result<std::optional<std::int64_t>>::Failure(MakeError(FramePacingErrors::InvalidNativeTiming));
            const auto interval = elapsed / static_cast<std::int64_t>(delta);
            if (interval < 1'000'000 || interval > Second)
                return Result<std::optional<std::int64_t>>::Failure(MakeError(FramePacingErrors::InvalidNativeTiming));
            return Result<std::optional<std::int64_t>>::Success(interval);
        }
    }  // namespace

    /** @copydoc IRenderBackend::PresentWithTiming */
    Result<void> IRenderBackend::PresentWithTiming(const FrameToken frame, const PresentationTimingRequest &) {
        return Present(frame);
    }

    /** @copydoc IRenderBackend::PollNativePresentTiming */
    Result<std::optional<NativePresentTiming>> IRenderBackend::PollNativePresentTiming() {
        return Result<std::optional<NativePresentTiming>>::Failure(MakeError(FramePacingErrors::NativeTimingUnsupported));
    }

    /** @copydoc FramePacer::FramePacer */
    FramePacer::FramePacer() noexcept : ownerThread_(std::this_thread::get_id()) {}

    /** @copydoc FramePacer::CheckOwner */
    Result<void> FramePacer::CheckOwner() const {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(FramePacingErrors::WrongThread));
        return Result<void>::Success();
    }

    /** @copydoc FramePacer::ClearBaselines */
    void FramePacer::ClearBaselines() noexcept {
        deadline_.reset();
        lastPoll_.reset();
        lastPresentEnd_.reset();
        lastNative_.reset();
        intervals_.fill(0);
        intervalCursor_ = 0;
        statistics_.lastNativeDisplayTime.reset();
        statistics_.estimatedRefreshHertz.reset();
        statistics_.nativeIntervalSamples = 0;
    }

    /** @copydoc FramePacer::Configure */
    Result<void> FramePacer::Configure(const FramePacingPolicy policy, const RenderSurfaceSnapshot &surface) {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (stopped_)
            return Result<void>::Failure(MakeError(FramePacingErrors::Stopped));
        if (policy.maximumFramesPerSecond > 1000)
            return Result<void>::Failure(MakeError(FramePacingErrors::InvalidPolicy));
        if (!ValidSurface(surface))
            return Result<void>::Failure(MakeError(FramePacingErrors::InvalidSurface));
        if (surface_ && StaleSurface(surface, *surface_))
            return Result<void>::Failure(MakeError(FramePacingErrors::StaleEvidence));
        if (!surface_ || surface.revision != surface_->revision || policy.maximumFramesPerSecond != policy_.maximumFramesPerSecond)
            ClearBaselines();
        surface_ = surface;
        policy_ = policy;
        return Result<void>::Success();
    }

    /** @copydoc FramePacer::ValidatePoll */
    Result<void> FramePacer::ValidatePoll(const std::int64_t stamp, const bool cancelled) const {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (stopped_)
            return Result<void>::Failure(MakeError(FramePacingErrors::Stopped));
        if (cancelled)
            return Result<void>::Failure(MakeError(FramePacingErrors::Cancelled));
        if (!surface_)
            return Result<void>::Failure(MakeError(FramePacingErrors::InvalidSurface));
        if (stamp < 0 || stamp > MaximumClock || (lastPoll_.has_value() && stamp < *lastPoll_))
            return Result<void>::Failure(MakeError(FramePacingErrors::InvalidClock));
        return Result<void>::Success();
    }

    /** @copydoc FramePacer::Poll */
    Result<FramePacingDecision> FramePacer::Poll(const Duration now, const bool cancelled) {
        const auto stamp = now.ToNanoseconds();
        if (auto valid = ValidatePoll(stamp, cancelled); valid.HasError())
            return Result<FramePacingDecision>::Failure(valid.ErrorValue());
        lastPoll_ = stamp;
        if (surface_->state != RenderSurfaceState::Ready)
            return Result<FramePacingDecision>::Success({FramePacingDisposition::Suspended, {}});
        if (policy_.maximumFramesPerSecond == 0)
            return Result<FramePacingDecision>::Success({});
        if (deadline_.has_value() && stamp < *deadline_)
            return Result<FramePacingDecision>::Success(
                {FramePacingDisposition::Wait, Duration::FromNanoseconds(std::min(*deadline_ - stamp, std::int64_t{2'000'000}))});
        const auto period = (Second + policy_.maximumFramesPerSecond - 1) / policy_.maximumFramesPerSecond;
        if (deadline_.has_value() && stamp - *deadline_ >= period)
            ++statistics_.missedDeadlines;
        // Rebase after lateness: never emit a burst of catch-up frames or replace simulation time.
        deadline_ = stamp + period;
        return Result<FramePacingDecision>::Success({});
    }

    /** @copydoc FramePacer::Poll */
    Result<FramePacingDecision> FramePacer::Poll(const Duration now, const CancellationToken &cancellation, const bool closing) {
        return Poll(now, closing || cancellation.IsCancellationRequested());
    }

    /** @copydoc FramePacer::ValidatePresent */
    Result<void> FramePacer::ValidatePresent(const std::uint64_t frame, const Duration start, const Duration end,
                                             const Duration observed) const {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (stopped_)
            return Result<void>::Failure(MakeError(FramePacingErrors::Stopped));
        if (!surface_ || surface_->state != RenderSurfaceState::Ready)
            return Result<void>::Failure(MakeError(FramePacingErrors::InvalidSurface));
        if (frame == 0 || frame <= lastFrame_)
            return Result<void>::Failure(MakeError(FramePacingErrors::StaleEvidence));
        if (start.ToNanoseconds() < 0 || end < start || (lastPresentEnd_ && start < *lastPresentEnd_))
            return Result<void>::Failure(MakeError(FramePacingErrors::InvalidClock));
        if (observed < end || observed.ToNanoseconds() > MaximumClock)
            return Result<void>::Failure(MakeError(FramePacingErrors::InvalidClock));
        return Result<void>::Success();
    }

    /** @copydoc FramePacer::RecordPresent */
    Result<void> FramePacer::RecordPresent(const std::uint64_t frameNumber, const Duration start, const Duration end, const bool succeeded,
                                           const std::optional<NativePresentTiming> &native, const std::optional<Duration> observedAt) {
        const Duration observed = observedAt.value_or(end);
        if (auto valid = ValidatePresent(frameNumber, start, end, observed); valid.HasError())
            return valid;
        std::optional<std::int64_t> interval;
        if (native) {
            if (!succeeded || !ValidNativeIdentity(*native, surface_->surface, frameNumber) || !ValidNativeClock(*native, observed))
                return Result<void>::Failure(MakeError(FramePacingErrors::InvalidNativeTiming));
            auto checked = NativeInterval(*native, lastNative_);
            if (checked.HasError())
                return Result<void>::Failure(checked.ErrorValue());
            interval = checked.Value();
        }
        lastFrame_ = frameNumber;
        lastPresentEnd_ = end;
        statistics_.lastPresentCallDuration = end - start;
        if (succeeded)
            ++statistics_.presentedFrames;
        else
            ++statistics_.failedPresents;
        if (native) {
            lastNative_ = native;
            statistics_.lastNativeDisplayTime = native->displayTime;
            statistics_.discardedNativeObservations = native->discardedObservations;
            if (native->displayOrdinal == 0) {
                intervals_.fill(0);
                intervalCursor_ = 0;
                statistics_.nativeIntervalSamples = 0;
                statistics_.estimatedRefreshHertz.reset();
            }
        }
        if (interval.has_value())
            AppendInterval(*interval);
        return Result<void>::Success();
    }

    /** @copydoc FramePacer::AppendInterval */
    void FramePacer::AppendInterval(const std::int64_t interval) noexcept {
        intervals_[intervalCursor_] = interval;
        intervalCursor_ = (intervalCursor_ + 1) % intervals_.size();
        statistics_.nativeIntervalSamples = std::min(statistics_.nativeIntervalSamples + 1, std::uint32_t{8});
        if (statistics_.nativeIntervalSamples < 4)
            return;
        std::int64_t total = 0;
        for (const auto value : intervals_)
            total += value;
        statistics_.estimatedRefreshHertz = static_cast<double>(Second) * statistics_.nativeIntervalSamples / static_cast<double>(total);
    }

    /** @copydoc FramePacer::Reset */
    Result<void> FramePacer::Reset() {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (stopped_)
            return Result<void>::Failure(MakeError(FramePacingErrors::Stopped));
        ClearBaselines();
        return Result<void>::Success();
    }

    /** @copydoc FramePacer::Statistics */
    Result<FramePacingStatistics> FramePacer::Statistics() const {
        if (auto owner = CheckOwner(); owner.HasError())
            return Result<FramePacingStatistics>::Failure(owner.ErrorValue());
        return Result<FramePacingStatistics>::Success(statistics_);
    }

    /** @copydoc FramePacer::Stop */
    Result<void> FramePacer::Stop() {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        stopped_ = true;
        surface_.reset();
        ClearBaselines();
        return Result<void>::Success();
    }
}  // namespace Horo::Render
