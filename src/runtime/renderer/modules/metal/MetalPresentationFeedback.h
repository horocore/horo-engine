#pragma once

#include "Horo/Runtime/Render/FramePacing.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace Horo::Render::Detail {
    /** @brief Value-only conversion from Core Animation host seconds to an explicitly sampled host clock.
     * Zero native time means dropped/not displayed. Invalid or >1 ms calibration spans are
     * discarded rather than exposing fabricated display time. Callbacks borrow no clock.
     */
    struct MetalPresentationCalibration final {
        double nativeSeconds{};
        Duration before;
        Duration after;

        [[nodiscard]] std::optional<NativePresentTiming> Translate(RenderSurfaceId surface, std::uint64_t frame,
                                                                   double displayedSeconds) const noexcept {
            if (!ValidCalibration() || !std::isfinite(displayedSeconds) || displayedSeconds <= 0)
                return std::nullopt;
            const auto halfSpan = (after - before).ToNanoseconds() / 2;
            const auto midpoint = before.ToNanoseconds() + halfSpan;
            const double converted = static_cast<double>(midpoint) + (displayedSeconds - nativeSeconds) * 1'000'000'000.0;
            if (!std::isfinite(converted) || converted <= 0 || converted >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
                return std::nullopt;
            return NativePresentTiming{surface, frame, 0, Duration::FromNanoseconds(static_cast<std::int64_t>(converted)),
                                       Duration::FromNanoseconds(halfSpan + 1)};
        }

    private:
        /** @brief Checks one synchronous host/native calibration without accessing callback state. */
        [[nodiscard]] bool ValidCalibration() const noexcept {
            return std::isfinite(nativeSeconds) && nativeSeconds > 0 && before.ToNanoseconds() >= 0 && after >= before &&
                   (after - before) <= Duration::FromMilliseconds(1);
        }
    };

    /** @brief One coalesced native callback value, shared beyond the native runtime lifetime.
     * Owner calls SelectSurface/Poll/Close; arbitrary Metal callback threads call Publish.
     * A mutex protects only surface identity/latest value. Both threads use try_lock, never
     * wait. Contention/coalescing drops feedback explicitly through DroppedCount. Close
     * atomically revokes publication before native teardown; callbacks retain only this
     * state, not a backend, frontend, drawable, clock or window. No callback waits occur.
     */
    class MetalPresentationFeedback final {
    public:
        [[nodiscard]] bool SelectSurface(const RenderSurfaceId surface) noexcept {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock() || closed_.load(std::memory_order_acquire)) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (surface != surface_) {
                surface_ = surface;
                latest_.reset();
                lastPublishedFrame_ = 0;
                lastPublishedTime_ = {};
            }
            return true;
        }

        void Publish(const NativePresentTiming timing) noexcept {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock() || closed_.load(std::memory_order_acquire) || timing.surface != surface_ ||
                timing.frameNumber <= lastPublishedFrame_ || timing.displayTime <= lastPublishedTime_) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (latest_)
                dropped_.fetch_add(1, std::memory_order_relaxed);
            lastPublishedFrame_ = timing.frameNumber;
            lastPublishedTime_ = timing.displayTime;
            latest_ = timing;
        }

        [[nodiscard]] std::optional<NativePresentTiming> Poll() noexcept {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock() || closed_.load(std::memory_order_acquire))
                return std::nullopt;
            return std::exchange(latest_, std::nullopt);
        }

        void Discard() noexcept {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }

        void Close() noexcept {
            closed_.store(true, std::memory_order_release);
        }

        [[nodiscard]] std::uint64_t DroppedCount() const noexcept {
            return dropped_.load(std::memory_order_relaxed);
        }

    private:
        std::mutex mutex_;
        std::atomic<bool> closed_{};
        std::atomic<std::uint64_t> dropped_{};
        RenderSurfaceId surface_;
        std::uint64_t lastPublishedFrame_{};
        Duration lastPublishedTime_;
        std::optional<NativePresentTiming> latest_;
    };
}  // namespace Horo::Render::Detail
