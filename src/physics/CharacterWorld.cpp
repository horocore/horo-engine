#include "Horo/Physics/CharacterWorld.h"

#include "CharacterControllerRegistry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Character {
    namespace {
        /** @brief Validates immutable request evidence before attempting queue ownership. */
        [[nodiscard]] Result<void> ValidateAdmissionRequest(const auto &impl, const CharacterMovementRequest &request) {
            if (!impl.acceptingCommands.load())
                return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
            if (const auto valid = ValidateCharacterMovementRequest(request, impl.descriptor.sceneGeneration, impl.descriptor.identity);
                valid.HasError())
                return valid;
            if (request.tick <= impl.closedTick.load())
                return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
            return Result<void>::Success();
        }

        /** @brief Tests whether one queued sequence makes a new request duplicate or globally stale. */
        [[nodiscard]] bool ConflictsWithQueuedCommand(const CharacterMovementRequest &queued,
                                                      const CharacterMovementRequest &request) noexcept {
            if (queued.controller != request.controller)
                return false;
            return (queued.tick == request.tick && queued.sequence == request.sequence) ||
                   (queued.tick < request.tick && queued.sequence >= request.sequence) ||
                   (queued.tick > request.tick && queued.sequence <= request.sequence);
        }

        /** @brief Revalidates lifecycle/order and exact duplication while queue ownership is held. */
        [[nodiscard]] Result<void> ValidateLockedAdmission(const auto &impl, const CharacterMovementRequest &request) {
            if (!impl.acceptingCommands.load())
                return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
            const std::uint32_t slot = request.controller.slot.index;
            if (slot >= impl.controllerGenerations.size() || impl.controllerGenerations[slot] != request.controller.slot.generation)
                return Result<void>::Failure(MakeError(CharacterErrors::HandleStale));
            if (request.tick <= impl.closedTick.load() || request.sequence <= impl.closedSequences[slot])
                return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
            if (std::ranges::any_of(impl.commands, [&request](const CharacterMovementRequest &queued) {
                return ConflictsWithQueuedCommand(queued, request);
            }))
                return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
            return Result<void>::Success();
        }

        /** @brief Target-private owned controller state prepared for later fixed-tick behavior. */
        struct CharacterControllerRecord final {
            CharacterControllerDescriptor descriptor;
            std::optional<CharacterMovementRequest> lastMovement;
            std::uint64_t lastSequence{};
        };

        /** @brief Orders owned commands independently of producer arrival timing. */
        [[nodiscard]] bool CommandLess(const CharacterMovementRequest &left, const CharacterMovementRequest &right) noexcept {
            if (left.tick != right.tick)
                return left.tick < right.tick;
            if (left.controller != right.controller)
                return left.controller < right.controller;
            return left.sequence < right.sequence;
        }

        /** @brief Restores the non-reentrant tick guard on every return path. */
        struct TickGuard final {
            explicit TickGuard(bool &value) noexcept : value_(value), previous_(std::exchange(value, true)) {}

            ~TickGuard() noexcept {
                value_ = previous_;
            }

            TickGuard(const TickGuard &) = delete;
            TickGuard &operator=(const TickGuard &) = delete;

            bool &value_;
            bool previous_{};
        };

        /** @brief Process-owned synchronization and non-wrapping Character world identity source. */
        struct CharacterWorldIdentityAuthority final {
            std::mutex mutex;
            std::uint64_t next{1};
        };

        /** @brief Encapsulates command and publication mutex ownership for one Character world. */
        class CharacterWorldSynchronization final {
        public:
            [[nodiscard]] std::unique_lock<std::mutex> TryLockCommands() {
                return std::unique_lock{commandMutex_, std::try_to_lock};
            }

            [[nodiscard]] std::scoped_lock<std::mutex> LockCommands() const {
                return std::scoped_lock{commandMutex_};
            }

            [[nodiscard]] std::scoped_lock<std::mutex> LockPublication() const {
                return std::scoped_lock{publicationMutex_};
            }

        private:
            mutable std::mutex commandMutex_;
            mutable std::mutex publicationMutex_;
        };

        [[nodiscard]] CharacterWorldIdentityAuthority &WorldIdentityAuthority() {
            static CharacterWorldIdentityAuthority authority;
            return authority;
        }

        [[nodiscard]] Result<CharacterWorldDescriptor> CompleteWorldDescriptor(const CharacterWorldPreparationDescriptor &descriptor) {
            if (const std::array valid{descriptor.sceneGeneration != 0, descriptor.physicsWorld.IsValid(),
                                       descriptor.collisionFilterGeneration != 0, descriptor.originGeneration != 0};
                !std::ranges::all_of(valid, std::identity{})) {
                return Result<CharacterWorldDescriptor>::Failure(MakeError(CharacterErrors::WorldInvalid));
            }
            CharacterWorldIdentityAuthority &authority = WorldIdentityAuthority();
            const std::lock_guard identityLock{authority.mutex};
            if (authority.next == std::numeric_limits<std::uint64_t>::max())
                return Result<CharacterWorldDescriptor>::Failure(MakeError(CharacterErrors::GenerationExhausted));
            const std::uint64_t identityValue = authority.next++;
            const auto identity = CharacterWorldId::Create(identityValue);
            if (identity.HasError())
                return Result<CharacterWorldDescriptor>::Failure(identity.ErrorValue());
            return Result<CharacterWorldDescriptor>::Success({descriptor.sceneGeneration, identity.Value(), descriptor.physicsWorld,
                                                              descriptor.collisionFilterGeneration, descriptor.originGeneration});
        }

        [[nodiscard]] Result<void> RequireOwnerThread(const std::thread::id ownerThread) {
            if (ownerThread != std::this_thread::get_id())
                return Result<void>::Failure(
                    MakeError(CharacterErrors::InvalidState, "Character world mutation requires its preparation thread."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> RequirePreparedMutation(const CharacterWorldState state, const std::thread::id ownerThread) {
            if (state != CharacterWorldState::Prepared)
                return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
            return RequireOwnerThread(ownerThread);
        }
    }  // namespace

    struct CharacterWorld::Impl final {
        Impl(const CharacterWorldDescriptor &owner, const CharacterWorldSettings &worldSettings,
             Detail::CharacterControllerRegistry<CharacterControllerRecord> &&controllerRegistry)
            : descriptor(owner), settings(worldSettings), controllers(std::move(controllerRegistry)),
              controllerGenerations(settings.Values().capacities.maximumControllers),
              closedSequences(settings.Values().capacities.maximumControllers) {
            commands.reserve(settings.Values().capacities.maximumQueuedCommands);
            scratch.reserve(settings.Values().work.maximumCommandsPerTick);
        }

        CharacterWorldDescriptor descriptor;
        CharacterWorldSettings settings;
        Detail::CharacterControllerRegistry<CharacterControllerRecord> controllers;
        std::vector<CharacterMovementRequest> commands;
        std::vector<CharacterMovementRequest> scratch;
        std::vector<std::uint32_t> controllerGenerations;
        std::vector<std::uint64_t> closedSequences;
        CharacterWorldSynchronization synchronization;
        CharacterPublishedTick published;
        std::atomic<std::uint64_t> closedTick{};
        std::atomic<bool> acceptingCommands{};
        std::atomic<std::uint64_t> admittedCommands{};
        std::atomic<std::uint64_t> rejectedCommands{};
        std::atomic<std::uint64_t> completedTicks{};
        std::atomic<std::uint32_t> pendingCommands{};
        std::atomic<std::uint32_t> maximumCommandDepth{};
        std::thread::id ownerThread{std::this_thread::get_id()};
        CharacterWorldState state{CharacterWorldState::Prepared};
        bool ticking{};
    };

    namespace {
        /** @brief Validates a frozen frame completely before queue or controller publication changes. */
        [[nodiscard]] Result<void> ValidateFrozenCommands(auto &impl) {
            for (std::size_t index = 0; index < impl.scratch.size(); ++index) {
                const CharacterMovementRequest &command = impl.scratch[index];
                if (const auto valid = ValidateCharacterMovementRequest(command, impl.descriptor.sceneGeneration, impl.descriptor.identity);
                    valid.HasError())
                    return valid;
                const auto record = impl.controllers.ResolveMutable(command.controller);
                if (record.HasError())
                    return Result<void>::Failure(record.ErrorValue());
                if (command.sequence <= record.Value()->lastSequence)
                    return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
                if (index != 0 && impl.scratch[index - 1].controller == command.controller &&
                    impl.scratch[index - 1].sequence == command.sequence)
                    return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Canonicalizes and removes one validated eligible frame while holding queue ownership. */
        [[nodiscard]] Result<void> FreezeCommandFrame(auto &impl, const CharacterFixedTickInput &input) {
            const auto queueLock = impl.synchronization.LockCommands();
            if (std::ranges::any_of(impl.commands, [&input](const CharacterMovementRequest &command) {
                return command.tick < input.tick;
            }))
                return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
            if (const auto eligible = static_cast<std::size_t>(std::ranges::count_if(impl.commands,
                                                                                     [&input](const CharacterMovementRequest &command) {
                return command.tick == input.tick;
            }));
                eligible > impl.settings.Values().work.maximumCommandsPerTick)
                return Result<void>::Failure(MakeError(CharacterErrors::CapacityExceeded));

            impl.scratch.clear();
            for (const CharacterMovementRequest &command : impl.commands) {
                if (command.tick == input.tick)
                    impl.scratch.push_back(command);
            }
            std::ranges::sort(impl.scratch, CommandLess);
            if (const auto valid = ValidateFrozenCommands(impl); valid.HasError())
                return valid;
            for (const CharacterMovementRequest &command : impl.scratch)
                impl.closedSequences[command.controller.slot.index] = command.sequence;
            std::erase_if(impl.commands, [&input](const CharacterMovementRequest &command) {
                return command.tick == input.tick;
            });
            impl.pendingCommands.store(static_cast<std::uint32_t>(impl.commands.size()));
            impl.closedTick.store(input.tick);
            return Result<void>::Success();
        }

        /** @brief Applies only the final replacement for each controller from one frozen canonical frame. */
        [[nodiscard]] std::uint32_t ApplyCommandFrame(auto &impl, const CharacterFixedTickInput &input) noexcept {
            std::uint32_t applied{};
            for (std::size_t index = 0; index < impl.scratch.size(); ++index) {
                const CharacterMovementRequest &command = impl.scratch[index];
                if (index + 1 != impl.scratch.size() && impl.scratch[index + 1].controller == command.controller)
                    continue;
                auto record = impl.controllers.ResolveMutable(command.controller);
                record.Value()->lastMovement = command;
                record.Value()->lastSequence = command.sequence;
                if (input.observer.movement)
                    input.observer.movement(input.observer.context, command);
                ++applied;
            }
            return applied;
        }

        /** @brief Atomically replaces the coherent Character publication marker. */
        void PublishTick(auto &impl, const CharacterFixedTickInput &input, const std::uint32_t applied) noexcept {
            const auto publicationLock = impl.synchronization.LockPublication();
            impl.published.completedTick = input.tick;
            ++impl.published.publicationRevision;
            impl.published.appliedCommands = applied;
        }

        /** @brief Emits one optional phase observation without exposing pipeline storage. */
        void ObservePhase(const CharacterFixedTickInput &input, const CharacterTickPhase phase) noexcept {
            if (input.observer.phase)
                input.observer.phase(input.observer.context, phase, input.tick);
        }
    }  // namespace

    /** @copydoc CharacterWorld::Prepare */
    Result<std::unique_ptr<CharacterWorld>> CharacterWorld::Prepare(const CharacterWorldPreparationDescriptor &descriptor,
                                                                    const CharacterWorldSettings &settings) {
        const auto completed = CompleteWorldDescriptor(descriptor);
        if (completed.HasError())
            return Result<std::unique_ptr<CharacterWorld>>::Failure(completed.ErrorValue());
        const CharacterWorldDescriptor owner = completed.Value();

        try {
            Detail::CharacterControllerRegistry<CharacterControllerRecord> registry{owner.sceneGeneration,
                                                                                    owner.identity,
                                                                                    {.maximumSlots =
                                                                                         settings.Values().capacities.maximumControllers}};
            auto impl = std::make_unique<Impl>(owner, settings, std::move(registry));
            return Result<std::unique_ptr<CharacterWorld>>::Success(std::unique_ptr<CharacterWorld>{
                new CharacterWorld(std::move(impl))});  // NOSONAR: make_unique cannot access this private constructor.
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<CharacterWorld>>::Failure(
                MakeError(CharacterErrors::CapacityExceeded, "Unable to allocate Character world ownership state."));
        }
    }

    /** @copydoc CharacterWorld::CharacterWorld */
    CharacterWorld::CharacterWorld(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc CharacterWorld::~CharacterWorld */
    CharacterWorld::~CharacterWorld() {
        Shutdown();
    }

    /** @copydoc CharacterWorld::Activate */
    Result<void> CharacterWorld::Activate() {
        if (const auto ready = RequirePreparedMutation(impl_->state, impl_->ownerThread); ready.HasError())
            return ready;
        impl_->state = CharacterWorldState::Active;
        impl_->acceptingCommands.store(true);
        return Result<void>::Success();
    }

    /** @copydoc CharacterWorld::CreateController */
    Result<CharacterControllerHandle> CharacterWorld::CreateController(const CharacterControllerDescriptor &descriptor) {
        if (const auto ready = RequirePreparedMutation(impl_->state, impl_->ownerThread); ready.HasError())
            return Result<CharacterControllerHandle>::Failure(
                MakeError(CharacterErrors::InvalidState, "Controller creation requires prepared owner-thread mutation."));
        if (const auto valid = ValidateCharacterControllerDescriptor(descriptor); valid.HasError())
            return Result<CharacterControllerHandle>::Failure(valid.ErrorValue());
        if (const std::array ownerMatches{descriptor.sceneGeneration == impl_->descriptor.sceneGeneration,
                                          descriptor.characterWorld == impl_->descriptor.identity,
                                          descriptor.physicsWorld == impl_->descriptor.physicsWorld};
            !std::ranges::all_of(ownerMatches, std::identity{})) {
            return Result<CharacterControllerHandle>::Failure(MakeError(CharacterErrors::HandleWorldMismatch));
        }
        if (descriptor.maximumContacts > impl_->settings.Values().work.maximumContactsPerMovement)
            return Result<CharacterControllerHandle>::Failure(
                MakeError(CharacterErrors::CapacityExceeded, "Controller contact capacity exceeds the Character world work budget."));
        auto acquired = impl_->controllers.Acquire(CharacterControllerRecord{descriptor});
        if (acquired.HasValue()) {
            const CharacterControllerHandle handle = acquired.Value();
            impl_->controllerGenerations[handle.slot.index] = handle.slot.generation;
            impl_->closedSequences[handle.slot.index] = 0;
        }
        return acquired;
    }

    /** @copydoc CharacterWorld::DestroyController */
    Result<void> CharacterWorld::DestroyController(const CharacterControllerHandle &handle) {
        if (const auto ready = RequirePreparedMutation(impl_->state, impl_->ownerThread); ready.HasError())
            return Result<void>::Failure(
                MakeError(CharacterErrors::InvalidState, "Controller destruction requires prepared owner-thread mutation."));
        return impl_->controllers.Remove(handle);
    }

    /** @copydoc CharacterWorld::ControllerDescriptor */
    Result<CharacterControllerDescriptor> CharacterWorld::ControllerDescriptor(const CharacterControllerHandle &handle) const {
        if (impl_->state == CharacterWorldState::Destroyed)
            return Result<CharacterControllerDescriptor>::Failure(MakeError(CharacterErrors::InvalidState));
        const auto record = impl_->controllers.Resolve(handle);
        if (record.HasError())
            return Result<CharacterControllerDescriptor>::Failure(record.ErrorValue());
        return Result<CharacterControllerDescriptor>::Success(record.Value()->descriptor);
    }

    /** @copydoc CharacterWorld::QueueMovementCommand */
    Result<CharacterCommandAdmission> CharacterWorld::QueueMovementCommand(const CharacterMovementRequest &request) {
        const auto rejected = [this](const CharacterCommandAdmissionStatus status) {
            impl_->rejectedCommands.fetch_add(1);
            return Result<CharacterCommandAdmission>::Success({status, impl_->pendingCommands.load()});
        };
        if (const auto valid = ValidateAdmissionRequest(*impl_, request); valid.HasError()) {
            impl_->rejectedCommands.fetch_add(1);
            return Result<CharacterCommandAdmission>::Failure(valid.ErrorValue());
        }

        if (auto queueLock = impl_->synchronization.TryLockCommands(); queueLock.owns_lock()) {
            if (const auto valid = ValidateLockedAdmission(*impl_, request); valid.HasError()) {
                impl_->rejectedCommands.fetch_add(1);
                return Result<CharacterCommandAdmission>::Failure(valid.ErrorValue());
            }
            if (impl_->commands.size() == impl_->settings.Values().capacities.maximumQueuedCommands)
                return rejected(CharacterCommandAdmissionStatus::RejectedFull);

            impl_->commands.push_back(request);
            const auto depth = static_cast<std::uint32_t>(impl_->commands.size());
            impl_->pendingCommands.store(depth);
            impl_->maximumCommandDepth.store(std::max(depth, impl_->maximumCommandDepth.load()));
            impl_->admittedCommands.fetch_add(1);
            return Result<CharacterCommandAdmission>::Success({CharacterCommandAdmissionStatus::Deferred, depth});
        }
        return rejected(CharacterCommandAdmissionStatus::RejectedBusy);
    }

    /** @copydoc CharacterWorld::AdvanceFixedTick */
    Result<void> CharacterWorld::AdvanceFixedTick(const CharacterFixedTickInput &input) {
        if (impl_->ownerThread != std::this_thread::get_id() || impl_->state != CharacterWorldState::Active || impl_->ticking)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        if (input.tick == 0 || input.sceneGeneration != impl_->descriptor.sceneGeneration || input.fixedDelta <= Duration{} ||
            input.tick != impl_->closedTick.load() + 1)
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));

        const TickGuard ticking{impl_->ticking};
        if (const auto frozen = FreezeCommandFrame(*impl_, input); frozen.HasError())
            return frozen;

        ObservePhase(input, CharacterTickPhase::FreezeCommands);
        const std::uint32_t applied = ApplyCommandFrame(*impl_, input);
        ObservePhase(input, CharacterTickPhase::ResolveMovement);

        PublishTick(*impl_, input, applied);
        impl_->completedTicks.fetch_add(1);
        ObservePhase(input, CharacterTickPhase::PublishCompletedTick);
        return Result<void>::Success();
    }

    /** @copydoc CharacterWorld::PublishedTick */
    CharacterPublishedTick CharacterWorld::PublishedTick() const noexcept {
        const auto publicationLock = impl_->synchronization.LockPublication();
        return impl_->published;
    }

    /** @copydoc CharacterWorld::TickStatistics */
    CharacterTickStatistics CharacterWorld::TickStatistics() const noexcept {
        return {
            impl_->completedTicks.load(),  impl_->admittedCommands.load(),    impl_->rejectedCommands.load(),
            impl_->pendingCommands.load(), impl_->maximumCommandDepth.load(),
        };
    }

    /** @copydoc CharacterWorld::Shutdown */
    void CharacterWorld::Shutdown() noexcept {
        if (impl_->state == CharacterWorldState::Destroyed)
            return;
        impl_->acceptingCommands.store(false);
        {
            const auto queueLock = impl_->synchronization.LockCommands();
            impl_->commands.clear();
            impl_->scratch.clear();
            impl_->pendingCommands.store(0);
        }
        impl_->controllers.Drain();
        impl_->state = CharacterWorldState::Destroyed;
    }

    /** @copydoc CharacterWorld::State */
    CharacterWorldState CharacterWorld::State() const noexcept {
        return impl_->state;
    }

    /** @copydoc CharacterWorld::Descriptor */
    const CharacterWorldDescriptor &CharacterWorld::Descriptor() const noexcept {
        return impl_->descriptor;
    }

    /** @copydoc CharacterWorld::Settings */
    const CharacterWorldSettings &CharacterWorld::Settings() const noexcept {
        return impl_->settings;
    }

    /** @copydoc CharacterWorld::ActiveControllerCount */
    std::size_t CharacterWorld::ActiveControllerCount() const noexcept {
        return impl_->controllers.Statistics().active;
    }

    /** @copydoc CharacterWorld::ControllerCapacity */
    std::size_t CharacterWorld::ControllerCapacity() const noexcept {
        return impl_->controllers.Statistics().capacity;
    }
}  // namespace Horo::Character
