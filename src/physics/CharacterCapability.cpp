#include "CharacterWorldInternal.h"

namespace Horo::Character {
    namespace {
        /** @brief Validates a client before borrowing world storage; no borrowed result escapes. */
        template <typename Call>
        auto WithWorld(const std::shared_ptr<CharacterCapabilityState> &state, Call call)
            -> decltype(call(std::declval<CharacterWorld &>())) {
            using Output = decltype(call(std::declval<CharacterWorld &>()));
            if (!state)
                return Output::Failure(MakeError(CharacterErrors::CapabilityUnavailable));
            if (state->ownerThread != std::this_thread::get_id())
                return Output::Failure(MakeError(CharacterErrors::ThreadAffinityViolation));
            if (state->revocation.Token().IsCancellationRequested())
                return Output::Failure(MakeError(CharacterErrors::CapabilityRevoked));
            if (state->world == nullptr)
                return Output::Failure(MakeError(CharacterErrors::CapabilityStale));
            return call(*state->world);
        }
    }  // namespace

    /** @copydoc CharacterCapability::CharacterCapability */
    CharacterCapability::CharacterCapability(std::shared_ptr<CharacterCapabilityState> state) noexcept : state_(std::move(state)) {}

    /** @copydoc CharacterCapability::Identity */
    CharacterCapabilityIdentity CharacterCapability::Identity() const noexcept {
        return state_ ? state_->identity : CharacterCapabilityIdentity{};
    }

    /** @copydoc CharacterCapability::CreateController */
    Result<CharacterControllerHandle> CharacterCapability::CreateController(const CharacterControllerDescriptor &descriptor) const {
        return WithWorld(state_, [&descriptor](CharacterWorld &world) {
            if (world.impl_->ticking.load() || world.impl_->placementActive)
                return Result<CharacterControllerHandle>::Failure(MakeError(CharacterErrors::InvalidState));
            return world.CreateController(descriptor);
        });
    }

    /** @copydoc CharacterCapability::QueueMovementCommand */
    Result<CharacterCommandAdmission> CharacterCapability::QueueMovementCommand(const CharacterMovementRequest &request) const {
        return WithWorld(state_, [this, &request](CharacterWorld &world) {
            return world.QueueScopedMovementCommand(request, state_->revocation.Token());
        });
    }

    /** @copydoc CharacterCapability::ControllerDescriptor */
    Result<CharacterControllerDescriptor> CharacterCapability::ControllerDescriptor(const CharacterControllerHandle &handle) const {
        return WithWorld(state_, [&handle](CharacterWorld &world) {
            return world.ControllerDescriptor(handle);
        });
    }

    /** @copydoc CharacterCapability::ControllerTransform */
    Result<CharacterTransformPublication> CharacterCapability::ControllerTransform(const CharacterControllerHandle &handle) const {
        return WithWorld(state_, [&handle](CharacterWorld &world) {
            return world.ControllerTransform(handle);
        });
    }

    /** @copydoc CharacterCapability::ControllerLocomotionSnapshot */
    Result<CharacterLocomotionSnapshot> CharacterCapability::ControllerLocomotionSnapshot(const CharacterControllerHandle &handle) const {
        return WithWorld(state_, [&handle](CharacterWorld &world) {
            return world.ControllerLocomotionSnapshot(handle);
        });
    }

    /** @copydoc CharacterCapability::Revoke */
    void CharacterCapability::Revoke() const noexcept {
        if (state_)
            state_->revocation.RequestCancellation();
    }

    /** @copydoc CharacterWorld::IssueCapability */
    Result<CharacterCapability> CharacterWorld::IssueCapability(const CancellationToken &revocation) {
        if (impl_->ownerThread != std::this_thread::get_id())
            return Result<CharacterCapability>::Failure(MakeError(CharacterErrors::ThreadAffinityViolation));
        if (impl_->state.load() == CharacterWorldState::Destroyed || impl_->ticking.load() || impl_->placementActive)
            return Result<CharacterCapability>::Failure(MakeError(CharacterErrors::InvalidState));
        if (revocation.IsCancellationRequested())
            return Result<CharacterCapability>::Failure(MakeError(CharacterErrors::CapabilityRevoked));
        const auto issued = impl_->capabilities.Issue(*this, impl_->ownerThread, revocation);
        if (issued.HasError())
            return Result<CharacterCapability>::Failure(issued.ErrorValue());
        return Result<CharacterCapability>::Success(CharacterCapability{issued.Value()});
    }

    /** @copydoc Detail::CharacterCapabilityRegistry::Issue */
    Result<std::shared_ptr<CharacterCapabilityState>> Detail::CharacterCapabilityRegistry::Issue(CharacterWorld &world,
                                                                                                 const std::thread::id ownerThread,
                                                                                                 const CancellationToken &parent) {
        using Output = Result<std::shared_ptr<CharacterCapabilityState>>;
        if (nextGeneration_ == 0)
            return Output::Failure(MakeError(CharacterErrors::GenerationExhausted));
        const auto slot = std::ranges::find_if(clients_, [](const auto &weak) {
            const auto client = weak.lock();
            return !client || client->revocation.Token().IsCancellationRequested();
        });
        if (slot == clients_.end())
            return Output::Failure(MakeError(CharacterErrors::CapacityExceeded));
        try {
            const auto &owner = world.Descriptor();
            auto state = std::make_shared<CharacterCapabilityState>(world,
                                                                    CharacterCapabilityIdentity{owner.sceneGeneration, owner.identity,
                                                                                                owner.physicsWorld, nextGeneration_},
                                                                    ownerThread, parent);
            if (const auto retired = slot->lock())
                retired->world = nullptr;
            *slot = state;
            nextGeneration_ = nextGeneration_ == std::numeric_limits<std::uint64_t>::max() ? 0 : nextGeneration_ + 1;
            return Output::Success(std::move(state));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(CharacterErrors::CapacityExceeded));
        }
    }

    /** @copydoc Detail::CharacterCapabilityRegistry::Retire */
    void Detail::CharacterCapabilityRegistry::Retire() noexcept {
        for (auto &weak : clients_) {
            if (const auto client = weak.lock())
                client->world = nullptr;
            weak.reset();
        }
    }
}  // namespace Horo::Character
