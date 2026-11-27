#pragma once

/** @file NativeAudioFacts.h
 * @brief Prepared callback loss latches and control facts that outlive native callback-source cleanup.
 */

#include "Horo/Audio/Internal/AudioBackend.h"

#include <atomic>
#include <optional>

namespace Horo::Audio::Backend {
    /**
     * @brief First-cause native loss publication with no allocation, lock or borrowed view.
     *
     * Native producers publish the encoded presence and cause atomically. Control reads with acquire.
     * Reset is control-only after native producer detachment or before a fresh source is registered.
     */
    class NativeAudioLossLatch final {
    public:
        /** @brief Preserve the first native cause. @param cause Typed loss cause; an unknown value becomes BackendFailure. */
        void Capture(DeviceLossCause cause) noexcept {
            if (cause > DeviceLossCause::BackendFailure)
                cause = DeviceLossCause::BackendFailure;
            const auto value = static_cast<std::uint8_t>(static_cast<std::uint8_t>(cause) + 1u);
            std::uint8_t expected{};
            (void)encoded_.compare_exchange_strong(expected, value, std::memory_order_release, std::memory_order_relaxed);
        }

        /** @brief Read without consuming the required fact. @return First cause or no native loss. */
        [[nodiscard]] std::optional<DeviceLossCause> Read() const noexcept {
            const auto value = encoded_.load(std::memory_order_acquire);
            return value == 0 ? std::nullopt : std::optional(static_cast<DeviceLossCause>(value - 1));
        }

        /** @brief Clear only after producer detachment and transfer to retained control storage, or before source registration. */
        void Reset() noexcept {
            encoded_.store(0, std::memory_order_relaxed);
        }

    private:
        static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
        std::atomic<std::uint8_t> encoded_{};  // Zero is empty; a known Horo cause is encoded one above its enum value.
    };

    /**
     * @brief One retained loss per admitted device epoch and one coalesced catalog invalidation, without allocation.
     *
     * Only the declared control owner reads or writes this mailbox. Native callbacks latch their own
     * prepared atomic facts; control captures them before native source destruction. New Open is blocked
     * while a loss remains undrained, so a different epoch cannot overwrite that required fact.
     */
    class NativeAudioFacts final {
    public:
        /** @brief Bind this mailbox to one fixed runtime. @param owner Backend runtime owner; invalid ownership admits no facts. */
        explicit NativeAudioFacts(const AudioRuntimeId owner) noexcept : owner_(owner) {}

        NativeAudioFacts(const NativeAudioFacts &) = delete;
        NativeAudioFacts &operator=(const NativeAudioFacts &) = delete;

        /**
         * @brief Retain the first loss for one device; repeated loss of that same admitted epoch coalesces.
         * @param loss Exact Horo output epoch and first native cause.
         * @return False for an invalid identity or different undrained epoch; caller must retain its native source.
         */
        [[nodiscard]] bool CaptureLoss(const DeviceLost loss) noexcept {
            if (!MatchesAudioDeviceEpoch(loss.epoch, loss.epoch) || loss.epoch.device.owner != owner_)
                return false;
            if (loss_)
                return loss_->epoch == loss.epoch;
            loss_ = loss;
            return true;
        }

        /** @brief Coalesce invalidation of an already published catalog. @param revision Latest nonzero catalog revision; zero is ignored.
         */
        void CaptureChange(const std::uint64_t revision) noexcept {
            if (owner_.IsValid() && revision > changeRevision_)
                changeRevision_ = revision;
        }

        /** @brief Read required-fact backpressure before reusing native epoch state. @return True while the host has not accepted the loss.
         */
        [[nodiscard]] bool HasLoss() const noexcept {
            return loss_.has_value();
        }

        /**
         * @brief Drain loss before lower-priority invalidation, retaining anything not admitted by caller storage.
         * @param output Borrowed caller storage.
         * @return Number written; an empty or one-element destination never drops the remaining fact.
         */
        [[nodiscard]] std::size_t Drain(const std::span<Event> output) noexcept {
            std::size_t count{};
            if (loss_ && count < output.size()) {
                output[count++] = {owner_, *loss_};
                loss_.reset();
            }
            if (changeRevision_ != 0 && count < output.size()) {
                output[count++] = {owner_, DevicesChanged{changeRevision_}};
                changeRevision_ = 0;
            }
            return count;
        }

    private:
        const AudioRuntimeId owner_;
        std::optional<DeviceLost> loss_;
        std::uint64_t changeRevision_{};
    };
}  // namespace Horo::Audio::Backend
