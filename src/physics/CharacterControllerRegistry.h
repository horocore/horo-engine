#pragma once

/** @file CharacterControllerRegistry.h
 * @brief Target-private bounded storage for scene-scoped Character controller records.
 */

#include "GenerationalSlotStorage.h"
#include "Horo/Physics/CharacterControllerContracts.h"

#include <cstddef>
#include <format>
#include <type_traits>
#include <utility>

namespace Horo::Character::Detail {
    /** @brief Testable registry limits; production uses the full non-wrapping generation range. */
    using CharacterControllerRegistryLimits = Physics::Detail::GenerationalSlotStorageLimits;

    /** @brief Allocation-free Character registry count snapshot. */
    struct CharacterControllerRegistryStatistics final {
        std::size_t capacity{};
        std::size_t active{};
    };

    /** @brief Owns one bounded mapping from Character handles to target-private controller records. */
    template <typename Value> class CharacterControllerRegistry final {
        static_assert(std::is_nothrow_move_constructible_v<Value>, "Character registry values must move without throwing.");
        static_assert(std::is_nothrow_destructible_v<Value>, "Character registry values must destruct without throwing.");

    public:
        /** @brief Allocates storage for owner generations and limits already validated by CharacterWorld preparation. */
        CharacterControllerRegistry(std::uint64_t sceneGeneration, CharacterWorldId world, CharacterControllerRegistryLimits limits)
            : sceneGeneration_(sceneGeneration), world_(world), storage_(limits) {}

        ~CharacterControllerRegistry() noexcept {
            Drain();
        }

        CharacterControllerRegistry(const CharacterControllerRegistry &) = delete;
        CharacterControllerRegistry &operator=(const CharacterControllerRegistry &) = delete;

        CharacterControllerRegistry(CharacterControllerRegistry &&other) noexcept
            : sceneGeneration_(std::exchange(other.sceneGeneration_, 0)), world_(std::exchange(other.world_, {})),
              storage_(std::move(other.storage_)) {}

        CharacterControllerRegistry &operator=(CharacterControllerRegistry &&) noexcept = delete;

        /** @brief Installs one record and returns its exact scene/world/slot generation. */
        [[nodiscard]] Result<CharacterControllerHandle> Acquire(Value value) {
            const std::optional<Physics::Detail::GenerationalSlot> slot = storage_.Acquire(std::move(value));
            if (!slot)
                return Result<CharacterControllerHandle>::Failure(MakeError(FullError()));
            return Result<CharacterControllerHandle>::Success({sceneGeneration_, world_, {slot->index, slot->generation}});
        }

        /** @brief Resolves one exact live record to a borrow bounded by registry mutation or destruction. */
        [[nodiscard]] Result<const Value *> Resolve(const CharacterControllerHandle &handle) const {
            if (const Result<void> owner = ValidateCharacterControllerHandleOwner(handle, sceneGeneration_, world_); owner.HasError())
                return Result<const Value *>::Failure(owner.ErrorValue());
            return ResolveSlot(handle);
        }

        /** @brief Resolves one exact live record for owner-thread mutation. */
        [[nodiscard]] Result<Value *> ResolveMutable(const CharacterControllerHandle &handle) {
            if (const Result<void> owner = ValidateCharacterControllerHandleOwner(handle, sceneGeneration_, world_); owner.HasError())
                return Result<Value *>::Failure(owner.ErrorValue());
            if (Value *value = storage_.Resolve(handle.slot.index, handle.slot.generation); value != nullptr)
                return Result<Value *>::Success(value);
            return Result<Value *>::Failure(HandleError(handle));
        }

        /** @brief Removes one exact live generation and recycles or permanently retires its slot. */
        [[nodiscard]] Result<void> Remove(const CharacterControllerHandle &handle) {
            if (const Result<void> owner = ValidateCharacterControllerHandleOwner(handle, sceneGeneration_, world_); owner.HasError())
                return owner;
            return RemoveSlot(handle);
        }

        /** @brief Destroys every resident record without allocating; the registry is terminal afterward. */
        void Drain() noexcept {
            storage_.Drain();
        }

        /** @brief Returns current capacity and occupancy together. */
        [[nodiscard]] CharacterControllerRegistryStatistics Statistics() const noexcept {
            return {storage_.Capacity(), storage_.ActiveCount()};
        }

    private:
        [[nodiscard]] Result<const Value *> ResolveSlot(const CharacterControllerHandle &handle) const {
            if (const Value *value = storage_.Resolve(handle.slot.index, handle.slot.generation); value != nullptr)
                return Result<const Value *>::Success(value);
            return Result<const Value *>::Failure(HandleError(handle));
        }

        [[nodiscard]] Result<void> RemoveSlot(const CharacterControllerHandle &handle) {
            return storage_.Remove(handle.slot.index, handle.slot.generation) ? Result<void>::Success()
                                                                              : Result<void>::Failure(HandleError(handle));
        }

        [[nodiscard]] const ErrorCodeDescriptor &FullError() const noexcept {
            return storage_.AllSlotsExhausted() ? CharacterErrors::GenerationExhausted : CharacterErrors::CapacityExceeded;
        }

        [[nodiscard]] static Error HandleError(const CharacterControllerHandle &handle) {
            return MakeError(CharacterErrors::HandleStale,
                             std::format("Character handle scene {}, world {}, slot {}, generation {} is not current.",
                                         handle.sceneGeneration, handle.world.Value(), handle.slot.index, handle.slot.generation));
        }

        std::uint64_t sceneGeneration_{};
        CharacterWorldId world_;
        Physics::Detail::GenerationalSlotStorage<Value> storage_;
    };
}  // namespace Horo::Character::Detail
