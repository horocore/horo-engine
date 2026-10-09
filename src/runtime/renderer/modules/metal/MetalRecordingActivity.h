#pragma once

#include <atomic>
#include <cstddef>

namespace Horo::Render::Detail {
    /**
     * @brief Closure/entry fence protecting owner pin release against worker native encoding.
     * @details Worker entry precedes its closed read; owner closure precedes its idle read.
     * Sequential consistency forbids both sides observing the old state. Only the owner
     * closes/observes idle for retirement; worker scopes modify active and observe closure.
     * A closed idle capsule admits no future native access, including delayed queued jobs.
     */
    class MetalRecordingActivity final {
    public:
        MetalRecordingActivity() = default;
        MetalRecordingActivity(const MetalRecordingActivity &) = delete;
        MetalRecordingActivity &operator=(const MetalRecordingActivity &) = delete;

        /** @brief Closes future native access without waiting for already-entered encoders. */
        void Close() noexcept {
            closed_.store(true, std::memory_order_seq_cst);
        }

        /** @brief Reports closure to a worker that has already entered its record scope. */
        [[nodiscard]] bool Closed() const noexcept {
            return closed_.load(std::memory_order_seq_cst);
        }

        /** @brief Reports whether already-entered native recorders are drained after Close. */
        [[nodiscard]] bool Idle() const noexcept {
            return active_.load(std::memory_order_seq_cst) == 0;
        }

        /** @brief Exactly-once nontransferable worker entry, covering every native encoding exit. */
        class RecordScope final {
        public:
            explicit RecordScope(MetalRecordingActivity &activity) noexcept : activity_(activity) {
                activity_.active_.fetch_add(1, std::memory_order_seq_cst);
            }

            RecordScope(const RecordScope &) = delete;
            RecordScope &operator=(const RecordScope &) = delete;
            RecordScope(RecordScope &&) = delete;
            RecordScope &operator=(RecordScope &&) = delete;

            ~RecordScope() {
                activity_.active_.fetch_sub(1, std::memory_order_seq_cst);
            }

            /** @brief Checks closure only after accounting entry; false prohibits any payload/native access. */
            [[nodiscard]] bool CanRecord() const noexcept {
                return !activity_.Closed();
            }

        private:
            MetalRecordingActivity &activity_;
        };

    private:
        std::atomic<std::size_t> active_{0};
        std::atomic<bool> closed_{false};
    };
}  // namespace Horo::Render::Detail
