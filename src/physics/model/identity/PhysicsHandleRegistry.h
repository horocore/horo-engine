#pragma once

/** @file PhysicsHandleRegistry.h
 * @brief Target-private bounded storage for world-scoped Physics handles and native mappings.
 */

#include "GenerationalSlotStorage.h"
#include "Horo/Physics/PhysicsIdentity.h"
#include "Horo/Physics/PhysicsWorldBudgets.h"

#include <cstddef>
#include <format>
#include <new>
#include <type_traits>
#include <utility>

namespace Horo::Physics::Detail {
    /** @brief Testable registry limits; production uses the full non-wrapping generation range. */
    using PhysicsHandleRegistryLimits = GenerationalSlotStorageLimits;

    /**
     * @brief Owns one typed world-local mapping without exposing its private value across the Physics target.
     *
     * Storage is allocated while the world is still a detached candidate. BindOwner assigns the host-issued
     * world generation during activation without allocation. Removed slots advance monotonically; a slot at
     * the configured generation ceiling is permanently retired instead of wrapping. Resolve returns a borrow
     * that remains valid only until removal, registry movement, or registry destruction.
     *
     * @tparam Handle One of the distinct Horo BodyHandle, ShapeHandle, or ConstraintHandle types.
     * @tparam Value Target-private native mapping or owning lease associated with one live handle.
     */
    template <typename Handle, typename Value> class PhysicsHandleRegistry final {
        static_assert(std::is_nothrow_move_constructible_v<Value>, "Physics registry values must move without throwing.");
        static_assert(std::is_nothrow_destructible_v<Value>, "Physics registry values must be safely destructible during teardown.");

    public:
        /**
         * @brief Allocates the complete bounded slot table for an unpublished world candidate.
         * @param limits Validated maximum slots and non-wrapping generation ceiling.
         * @return Unbound registry, or a stable capacity/descriptor error before publication.
         */
        [[nodiscard]] static Result<PhysicsHandleRegistry> Create(const PhysicsHandleRegistryLimits limits) {
            if (limits.maximumSlots > MaximumPhysicsResourceRecords)
                return Result<PhysicsHandleRegistry>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            if (limits.maximumGeneration == 0)
                return Result<PhysicsHandleRegistry>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Physics registry generation ceiling must be non-zero."));
            try {
                return Result<PhysicsHandleRegistry>::Success(PhysicsHandleRegistry{limits});
            } catch (const std::bad_alloc &) {
                return Result<PhysicsHandleRegistry>::Failure(
                    MakeError(PhysicsErrors::CapacityExceeded, "Unable to allocate the bounded Physics handle registry."));
            }
        }

        PhysicsHandleRegistry(const PhysicsHandleRegistry &) = delete;
        PhysicsHandleRegistry &operator=(const PhysicsHandleRegistry &) = delete;

        PhysicsHandleRegistry(PhysicsHandleRegistry &&other) noexcept
            : storage_(std::move(other.storage_)), owner_(std::exchange(other.owner_, {})) {}

        PhysicsHandleRegistry &operator=(PhysicsHandleRegistry &&other) noexcept {
            if (this == &other)
                return *this;
            storage_ = std::move(other.storage_);
            owner_ = std::exchange(other.owner_, {});
            return *this;
        }

        /**
         * @brief Binds the registry exactly once to the published world generation.
         * @param owner Non-zero, process-unique identity supplied by aggregate scene activation.
         * @return Success, WorldInvalid for malformed identity, or InvalidState after an earlier bind.
         * @post Successful binding performs no allocation and publishes no object handle by itself.
         */
        [[nodiscard]] Result<void> BindOwner(const PhysicsWorldId owner) {
            if (!owner.IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
            if (owner_.IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState, "Physics registry owner is already bound."));
            owner_ = owner;
            return Result<void>::Success();
        }

        /**
         * @brief Installs one already-created private value and issues its exact Horo handle.
         * @param value Native mapping or lease whose move and destruction cannot throw.
         * @return New handle, CapacityExceeded while live slots fill the bound, GenerationExhausted
         * when every slot is permanently retired, or InvalidState before owner binding.
         */
        [[nodiscard]] Result<Handle> Acquire(Value value) {
            if (!owner_.IsValid())
                return Result<Handle>::Failure(MakeError(PhysicsErrors::InvalidState, "Physics registry owner is not active."));
            const std::optional<GenerationalSlot> slot = storage_.Acquire(std::move(value));
            if (!slot)
                return Result<Handle>::Failure(MakeError(FullError()));
            return Result<Handle>::Success(Handle{owner_, {slot->index, slot->generation}});
        }

        /**
         * @brief Resolves one live same-world handle to its target-private value.
         * @param handle Borrowed Horo identity.
         * @return Borrowed mapping, or a stable malformed/foreign/stale/state error before native access.
         */
        [[nodiscard]] Result<const Value *> Resolve(const Handle &handle) const {
            const Result<void> owner = ValidateOwner(handle);
            if (owner.HasError())
                return Result<const Value *>::Failure(owner.ErrorValue());
            const Value *value = storage_.Resolve(handle.slot.index, handle.slot.generation);
            return value ? Result<const Value *>::Success(value) : Result<const Value *>::Failure(HandleError(handle));
        }

        /**
         * @brief Removes the exact live generation and either recycles or permanently retires its slot.
         * @param handle Same-world handle currently resident in this registry.
         * @return Success, or a stable malformed/foreign/stale/state error without changing storage.
         */
        [[nodiscard]] Result<void> Remove(const Handle &handle) {
            const Result<void> owner = ValidateOwner(handle);
            if (owner.HasError())
                return owner;
            return storage_.Remove(handle.slot.index, handle.slot.generation) ? Result<void>::Success()
                                                                              : Result<void>::Failure(HandleError(handle));
        }

        /** @brief Returns whether activation bound a world generation. */
        [[nodiscard]] bool IsBound() const noexcept {
            return owner_.IsValid();
        }

        /** @brief Returns the immutable slot bound allocated during preparation. */
        [[nodiscard]] std::size_t Capacity() const noexcept {
            return storage_.Capacity();
        }

        /** @brief Returns the number of currently resolvable values. */
        [[nodiscard]] std::size_t ActiveCount() const noexcept {
            return storage_.ActiveCount();
        }

    private:
        explicit PhysicsHandleRegistry(const PhysicsHandleRegistryLimits limits) : storage_(limits) {}

        [[nodiscard]] const ErrorCodeDescriptor &FullError() const noexcept {
            return storage_.AllSlotsExhausted() ? PhysicsErrors::GenerationExhausted : PhysicsErrors::CapacityExceeded;
        }

        [[nodiscard]] Result<void> ValidateOwner(const Handle &handle) const {
            if (!owner_.IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState, "Physics registry owner is not active."));
            return ValidatePhysicsHandleOwner(handle, owner_);
        }

        [[nodiscard]] static Error HandleError(const Handle &handle) {
            return MakeError(PhysicsErrors::HandleStale, std::format("Physics handle world {}, slot {}, generation {} is not current.",
                                                                     handle.world.Value(), handle.slot.index, handle.slot.generation));
        }

        GenerationalSlotStorage<Value> storage_;
        PhysicsWorldId owner_;
    };
}  // namespace Horo::Physics::Detail
