#pragma once

/** @file GenerationalSlotStorage.h
 * @brief Target-private bounded value storage with non-wrapping slot generations.
 */

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Physics::Detail {
    /** @brief Allocation and retirement limits shared by target-private typed registry wrappers. */
    struct GenerationalSlotStorageLimits final {
        std::uint32_t maximumSlots{};
        std::uint32_t maximumGeneration{std::numeric_limits<std::uint32_t>::max()};
    };

    /** @brief Slot identity returned after a value is installed successfully. */
    struct GenerationalSlot final {
        std::uint32_t index{};
        std::uint32_t generation{};
    };

    /**
     * @brief Owns bounded values and generation mechanics without knowing public handles or error domains.
     * @tparam Value Target-private record whose move and destruction cannot throw.
     */
    template <typename Value> class GenerationalSlotStorage final {
        static_assert(std::is_nothrow_move_constructible_v<Value>, "Slot values must move without throwing.");
        static_assert(std::is_nothrow_destructible_v<Value>, "Slot values must destruct without throwing.");

    public:
        /** @brief Allocates the complete slot table; callers validate limits and translate allocation failures. */
        explicit GenerationalSlotStorage(const GenerationalSlotStorageLimits limits)
            : maximumGeneration_(limits.maximumGeneration), entries_(limits.maximumSlots) {
            if (entries_.empty())
                return;
            for (std::uint32_t slot = 0; slot + 1 < limits.maximumSlots; ++slot)
                entries_[slot].nextFree = slot + 1;
            freeHead_ = 0;
        }

        ~GenerationalSlotStorage() noexcept {
            Drain();
        }

        GenerationalSlotStorage(const GenerationalSlotStorage &) = delete;
        GenerationalSlotStorage &operator=(const GenerationalSlotStorage &) = delete;

        GenerationalSlotStorage(GenerationalSlotStorage &&other) noexcept
            : maximumGeneration_(other.maximumGeneration_), entries_(std::move(other.entries_)),
              freeHead_(std::exchange(other.freeHead_, InvalidSlot)), activeCount_(std::exchange(other.activeCount_, 0)),
              exhaustedCount_(std::exchange(other.exhaustedCount_, 0)) {
            other.maximumGeneration_ = 0;
            other.entries_.clear();
        }

        GenerationalSlotStorage &operator=(GenerationalSlotStorage &&other) noexcept {
            if (this == &other)
                return *this;
            maximumGeneration_ = other.maximumGeneration_;
            entries_ = std::move(other.entries_);
            freeHead_ = std::exchange(other.freeHead_, InvalidSlot);
            activeCount_ = std::exchange(other.activeCount_, 0);
            exhaustedCount_ = std::exchange(other.exhaustedCount_, 0);
            other.maximumGeneration_ = 0;
            other.entries_.clear();
            return *this;
        }

        /** @brief Installs one value when a reusable slot remains. @return Issued slot or empty when full. */
        [[nodiscard]] std::optional<GenerationalSlot> Acquire(Value value) {
            if (freeHead_ == InvalidSlot)
                return std::nullopt;
            const std::uint32_t slot = freeHead_;
            Entry &entry = entries_[slot];
            freeHead_ = entry.nextFree;
            entry.nextFree = InvalidSlot;
            entry.value.emplace(std::move(value));
            ++activeCount_;
            return GenerationalSlot{slot, entry.generation};
        }

        /** @brief Resolves an exact occupied generation. @return Borrow or null for absent, out-of-range, or stale identity. */
        [[nodiscard]] const Value *Resolve(const std::uint32_t index, const std::uint32_t generation) const noexcept {
            if (index >= entries_.size())
                return nullptr;
            const Entry &entry = entries_[index];
            return entry.value.has_value() && entry.generation == generation ? &*entry.value : nullptr;
        }

        /** @brief Resolves an exact occupied generation for owner-thread mutation. @return Borrow or null when stale. */
        [[nodiscard]] Value *Resolve(const std::uint32_t index, const std::uint32_t generation) noexcept {
            if (index >= entries_.size())
                return nullptr;
            Entry &entry = entries_[index];
            return entry.value.has_value() && entry.generation == generation ? &*entry.value : nullptr;
        }

        /** @brief Removes an exact live generation. @return False without mutation when absent or stale. */
        [[nodiscard]] bool Remove(const std::uint32_t index, const std::uint32_t generation) noexcept {
            if (Resolve(index, generation) == nullptr)
                return false;
            Entry &entry = entries_[index];
            entry.value.reset();
            --activeCount_;
            if (entry.generation == maximumGeneration_) {
                ++exhaustedCount_;
                return true;
            }
            ++entry.generation;
            entry.nextFree = freeHead_;
            freeHead_ = index;
            return true;
        }

        /** @brief Destroys every resident value and makes the storage terminal without allocating. */
        void Drain() noexcept {
            for (Entry &entry : entries_) {
                entry.value.reset();
                entry.nextFree = InvalidSlot;
            }
            freeHead_ = InvalidSlot;
            activeCount_ = 0;
        }

        [[nodiscard]] std::size_t Capacity() const noexcept {
            return entries_.size();
        }

        [[nodiscard]] std::size_t ActiveCount() const noexcept {
            return activeCount_;
        }

        [[nodiscard]] bool AllSlotsExhausted() const noexcept {
            return !entries_.empty() && exhaustedCount_ == entries_.size();
        }

    private:
        static constexpr std::uint32_t InvalidSlot = std::numeric_limits<std::uint32_t>::max();

        struct Entry final {
            std::optional<Value> value;
            std::uint32_t generation{1};
            std::uint32_t nextFree{InvalidSlot};
        };

        std::uint32_t maximumGeneration_{};
        std::vector<Entry> entries_;
        std::uint32_t freeHead_{InvalidSlot};
        std::size_t activeCount_{};
        std::size_t exhaustedCount_{};
    };
}  // namespace Horo::Physics::Detail
