#pragma once
/** @file GameplayPersistenceBindingInternal.h @brief Stable existing-registration source indirection and preallocated publication. */
#include "Horo/Gameplay/BehaviorRestore.h"

#include <optional>
#include <thread>
#include <type_traits>

namespace Horo::Gameplay::Detail {
    class GameplayPersistenceSourceSlot final : public IPersistenceSource {
    public:
        GameplayPersistenceSourceSlot(std::shared_ptr<IPersistenceSource> factory, const std::atomic_bool &admission)
            : factory_(factory), current_(std::move(factory)), admission_(&admission), owner_(std::this_thread::get_id()) {}

        [[nodiscard]] const std::shared_ptr<IPersistenceSource> &Factory() const noexcept {
            RequireOwner();
            return factory_;
        }

        [[nodiscard]] bool SameBinding(const BehaviorPersistenceTarget &target) const noexcept {
            if (std::this_thread::get_id() != owner_)
                return false;
            return target_ && target_->SameOwner(target);
        }

        [[nodiscard]] Result<std::vector<std::byte>> CaptureRuntimeState(std::uint64_t maximum) const override {
            if (auto admitted = Admit(); admitted.HasError())
                return Result<std::vector<std::byte>>::Failure(admitted.ErrorValue());
            if (target_ && target_->InactiveRestore())
                return Result<std::vector<std::byte>>::Failure(MakeError(Runtime::SaveErrors::RestoreActivationStale));
            return current_->CaptureRuntimeState(maximum);
        }

        [[nodiscard]] Result<std::unique_ptr<IPreparedPersistenceState>> PrepareRuntimeState(std::span<const std::byte> bytes) override {
            if (auto admitted = Admit(); admitted.HasError())
                return Result<std::unique_ptr<IPreparedPersistenceState>>::Failure(admitted.ErrorValue());
            return current_->PrepareRuntimeState(bytes);
        }

        void CloseNativeSources() noexcept {
            RequireOwner();
            closed_ = true;
            target_.reset();
            current_.reset();
            factory_.reset();
            admission_ = nullptr;
        }

        void BindOrdinary(std::shared_ptr<IPersistenceSource> source, BehaviorPersistenceTarget target) {
            RequireOwner();
            current_ = std::move(source);
            target_ = std::move(target);
        }

        void Swap(std::shared_ptr<IPersistenceSource> &source, std::optional<BehaviorPersistenceTarget> &target) noexcept {
            RequireOwner();
            static_assert(std::is_nothrow_swappable_v<std::optional<BehaviorPersistenceTarget>>);
            current_.swap(source);
            target_.swap(target);
        }

    private:
        void RequireOwner() const noexcept {
            if (std::this_thread::get_id() != owner_)
                std::terminate();
        }

        [[nodiscard]] Result<void> Admit() const {
            if (std::this_thread::get_id() != owner_)
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
            if (closed_)
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreActivationStale));
            if (!admission_->load(std::memory_order_acquire) || (target_ && !target_->CanUse()))
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreActivationStale));
            return Result<void>::Success();
        }

        std::shared_ptr<IPersistenceSource> factory_, current_;
        std::optional<BehaviorPersistenceTarget> target_;
        bool closed_{};
        const std::atomic_bool *admission_;
        const std::thread::id owner_;
    };

    /** @brief Constructed before the sole gate. Publish swaps only; old native source survives through explicit transient retirement. */
    class PreparedGameplaySourceBinding final {
    public:
        PreparedGameplaySourceBinding(std::shared_ptr<GameplayPersistenceSourceSlot> slot, std::shared_ptr<IPersistenceSource> incoming,
                                      BehaviorPersistenceTarget target)
            : slot_(std::move(slot)), incoming_(std::move(incoming)), target_(std::move(target)) {}

        void Publish() noexcept {
            if (std::this_thread::get_id() != owner_)
                std::terminate();
            if (published_)
                std::terminate();
            slot_->Swap(incoming_, target_);
            published_ = true;
        }

    private:
        std::shared_ptr<GameplayPersistenceSourceSlot> slot_;
        std::shared_ptr<IPersistenceSource> incoming_;
        std::optional<BehaviorPersistenceTarget> target_;
        bool published_{};
        const std::thread::id owner_{std::this_thread::get_id()};
    };
}  // namespace Horo::Gameplay::Detail
