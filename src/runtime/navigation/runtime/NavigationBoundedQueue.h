#pragma once

#include "Horo/Navigation/NavigationRuntimeQueues.h"

#include <atomic>
#include <bit>
#include <limits>
#include <optional>
#include <type_traits>
#include <vector>

// Shared private transport primitive. It executes no work and owns no threads.
namespace Horo::Navigation::Detail {
    constexpr std::size_t MaximumContentionAttempts = 64;

    inline void SaturatingIncrement(std::atomic<std::uint64_t> &counter) noexcept {
        auto value = counter.load();
        for (std::size_t attempt = 0; attempt < MaximumContentionAttempts && value != std::numeric_limits<std::uint64_t>::max();
             ++attempt) {
            if (counter.compare_exchange_weak(value, value + 1U))
                return;
        }
    }

    struct AtomicQueueStats final {
        std::atomic<std::uint64_t> enqueued{};
        std::atomic<std::uint64_t> dequeued{};
        std::atomic<std::uint64_t> rejectedFull{};
        std::atomic<std::uint64_t> rejectedClosed{};

        [[nodiscard]] NavigationQueueStats Snapshot() const noexcept {
            return {
                .enqueued = enqueued.load(),
                .dequeued = dequeued.load(),
                .rejectedFull = rejectedFull.load(),
                .rejectedClosed = rejectedClosed.load(),
            };
        }
    };

    template <typename T> class BoundedMpmcQueue final {
        static_assert(std::is_nothrow_move_constructible_v<T>);

        struct alignas(64) Slot final {
            std::atomic<std::size_t> sequence{};
            std::optional<T> record;
        };

    public:
        explicit BoundedMpmcQueue(const std::size_t capacity) : slots_(capacity), capacity_(capacity), mask_(capacity - 1U) {
            for (std::size_t index = 0; index < capacity; ++index)
                slots_[index].sequence.store(index);
        }

        [[nodiscard]] static bool ValidCapacity(const std::uint32_t capacity) noexcept {
            return capacity >= 2U && capacity <= MaximumNavigationRuntimeQueueSlots && std::has_single_bit(capacity);
        }

        [[nodiscard]] static std::optional<std::size_t> StorageBytes(const std::uint32_t capacity) noexcept {
            if (!ValidCapacity(capacity) || capacity > std::numeric_limits<std::size_t>::max() / sizeof(Slot))
                return std::nullopt;
            return static_cast<std::size_t>(capacity) * sizeof(Slot);
        }

        [[nodiscard]] NavigationQueueEnqueueResult TryPush(T &record) noexcept {
            using enum NavigationQueueEnqueueResult;
            if (closed_.load()) {
                SaturatingIncrement(stats_.rejectedClosed);
                return Closed;
            }

            if (const auto claim = TryClaim(enqueuePosition_, 0); claim.has_value()) {
                claim->slot->record.emplace(std::move(record));
                count_.fetch_add(1U);
                claim->slot->sequence.store(claim->position + 1U);
                SaturatingIncrement(stats_.enqueued);
                return Enqueued;
            }
            SaturatingIncrement(stats_.rejectedFull);
            return Full;
        }

        [[nodiscard]] std::optional<T> TryPop() noexcept {
            const auto claim = TryClaim(dequeuePosition_, 1U);
            if (!claim.has_value())
                return std::nullopt;
            std::optional<T> record{std::move(claim->slot->record)};
            claim->slot->record.reset();
            count_.fetch_sub(1U);
            claim->slot->sequence.store(claim->position + capacity_);
            SaturatingIncrement(stats_.dequeued);
            return record;
        }

        void Close() noexcept {
            closed_.store(true);
        }

        [[nodiscard]] bool IsClosedAndEmpty() const noexcept {
            return closed_.load() && count_.load() == 0;
        }

        [[nodiscard]] NavigationQueueStats Stats() const noexcept {
            return stats_.Snapshot();
        }

    private:
        struct Claim final {
            Slot *slot{};
            std::size_t position{};
        };

        [[nodiscard]] std::optional<Claim> TryClaim(std::atomic<std::size_t> &cursor, const std::size_t sequenceOffset) noexcept {
            auto position = cursor.load();
            for (std::size_t attempt = 0; attempt < MaximumContentionAttempts; ++attempt) {
                Slot &slot = slots_[position & mask_];
                const auto difference =
                    static_cast<std::intptr_t>(slot.sequence.load()) - static_cast<std::intptr_t>(position + sequenceOffset);
                if (difference == 0) {
                    if (cursor.compare_exchange_weak(position, position + 1U))
                        return Claim{.slot = &slot, .position = position};
                    continue;
                }
                if (difference < 0)
                    return std::nullopt;
                position = cursor.load();
            }
            return std::nullopt;
        }

        std::vector<Slot> slots_;
        std::size_t capacity_{};
        std::size_t mask_{};
        alignas(64) std::atomic<std::size_t> enqueuePosition_{};
        alignas(64) std::atomic<std::size_t> dequeuePosition_{};
        alignas(64) std::atomic<std::size_t> count_{};
        std::atomic<bool> closed_{false};
        AtomicQueueStats stats_;
    };

}  // namespace Horo::Navigation::Detail
