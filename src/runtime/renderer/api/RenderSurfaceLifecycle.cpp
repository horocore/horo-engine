#include "Horo/Runtime/Render/RenderSurfaceLifecycle.h"

#include "Horo/Runtime/Render/RenderSurfaceLifecycleErrors.h"

#include <array>
#include <format>
#include <limits>
#include <utility>

namespace Horo::Render {
    namespace {
        [[nodiscard]] constexpr bool IsKnown(const RenderSurfaceCommandKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(RenderSurfaceCommandKind::Close);
        }

        [[nodiscard]] constexpr bool IsKnown(const RenderSurfaceRealization realization) noexcept {
            return static_cast<std::uint8_t>(realization) <= static_cast<std::uint8_t>(RenderSurfaceRealization::Unattached);
        }

        [[nodiscard]] constexpr bool IsKnown(const RenderSurfaceState state) noexcept {
            return static_cast<std::uint8_t>(state) <= static_cast<std::uint8_t>(RenderSurfaceState::Closing);
        }

        [[nodiscard]] constexpr bool IsKnown(const PresentMode mode) noexcept {
            return static_cast<std::uint8_t>(mode) <= static_cast<std::uint8_t>(PresentMode::Immediate);
        }

        [[nodiscard]] bool IsValid(const ResolvedPresentMode &mode) noexcept {
            if (!IsKnown(mode.requested) || !IsKnown(mode.resolved) ||
                static_cast<std::uint8_t>(mode.resolution) > static_cast<std::uint8_t>(PresentModeResolution::DegradedFallback))
                return false;
            if (mode.resolution == PresentModeResolution::Exact)
                return mode.requested == mode.resolved && mode.preferenceIndex == 0;
            return mode.requested != mode.resolved && mode.preferenceIndex > 0 && mode.preferenceIndex < MaximumPresentModeEntries;
        }

        [[nodiscard]] bool IsValid(const RenderSurfaceConfiguration &configuration) noexcept {
            return configuration.extent.IsValid() && configuration.displayRevision != 0 && IsValid(configuration.presentMode);
        }

        [[nodiscard]] bool HasValidShape(const RenderSurfaceCommand &command) noexcept {
            using enum RenderSurfaceCommandKind;
            if (!IsKnown(command.kind) || command.sequence == 0)
                return false;
            if (command.kind == Attach)
                return !command.target.has_value() || IsValid(*command.target);
            if (command.kind == Resize || command.kind == Replace || command.kind == Recover)
                return command.target.has_value() && IsValid(*command.target);
            return !command.target.has_value();
        }

        [[nodiscard]] bool IsAttached(const RenderSurfaceSnapshot &snapshot) noexcept {
            return snapshot.surface.IsAttachedGeneration() && snapshot.state != RenderSurfaceState::Unattached;
        }

        [[nodiscard]] bool CanQueue(const RenderSurfaceSnapshot &snapshot, const RenderSurfaceCommandKind kind) noexcept {
            constexpr std::array permissions{
                std::array{true, false, false, false, false, false},  // Attach
                std::array{false, true, true, true, false, false},    // Resize
                std::array{false, true, true, true, false, false},    // Suspend
                std::array{false, true, true, true, false, false},    // Replace
                std::array{false, true, true, true, false, false},    // Lose
                std::array{false, false, false, false, true, false},  // Recover
                std::array{false, true, true, true, true, false},     // Close
            };
            constexpr std::array requiresAttachment{false, true, true, true, false, false, false};
            static_assert(permissions.size() == static_cast<std::size_t>(RenderSurfaceCommandKind::Close) + 1);
            static_assert(permissions.front().size() == static_cast<std::size_t>(RenderSurfaceState::Closing) + 1);
            static_assert(requiresAttachment.size() == permissions.size());
            if (!IsKnown(kind) || !IsKnown(snapshot.state))
                return false;
            const auto commandIndex = static_cast<std::size_t>(kind);
            const auto stateIndex = static_cast<std::size_t>(snapshot.state);
            return permissions[commandIndex][stateIndex] && (!requiresAttachment[commandIndex] || IsAttached(snapshot));
        }

        [[nodiscard]] bool IsValidOutcome(const RenderSurfaceCommand &command, const RenderSurfaceRealization realization,
                                          const RenderSurfaceState priorState,
                                          const std::optional<RenderSurfaceConfiguration> &priorActive) noexcept {
            constexpr std::array outcomes{
                std::array{true, true, true, false, false},    // Attach
                std::array{true, true, true, true, false},     // Resize
                std::array{false, true, false, false, false},  // Suspend
                std::array{true, true, true, true, false},     // Replace
                std::array{false, false, true, false, false},  // Lose
                std::array{true, true, true, true, false},     // Recover
                std::array{false, false, false, false, true},  // Close
            };
            static_assert(outcomes.size() == static_cast<std::size_t>(RenderSurfaceCommandKind::Close) + 1);
            static_assert(outcomes.front().size() == static_cast<std::size_t>(RenderSurfaceRealization::Unattached) + 1);
            if (!IsKnown(command.kind) || !IsKnown(realization))
                return false;
            if (realization == RenderSurfaceRealization::Ready && !command.target.has_value())
                return false;
            if (realization == RenderSurfaceRealization::PreserveActive)
                return priorState == RenderSurfaceState::Ready && priorActive.has_value();
            return outcomes[static_cast<std::size_t>(command.kind)][static_cast<std::size_t>(realization)];
        }

        [[nodiscard]] constexpr bool AdvancesGeneration(const RenderSurfaceCommandKind kind,
                                                        const RenderSurfaceRealization realization) noexcept {
            return (realization == RenderSurfaceRealization::Ready || realization == RenderSurfaceRealization::Suspended) &&
                   kind != RenderSurfaceCommandKind::Suspend;
        }

        void PublishRealization(RenderSurfaceSnapshot &snapshot, const RenderSurfaceCommand &command,
                                const RenderSurfaceRealization realization, const RenderSurfaceState priorState,
                                const std::optional<RenderSurfaceConfiguration> &priorActive) {
            using enum RenderSurfaceRealization;
            switch (realization) {
                case Ready:
                    snapshot.state = RenderSurfaceState::Ready;
                    snapshot.active = command.target;
                    break;
                case Suspended:
                    snapshot.state = RenderSurfaceState::Suspended;
                    if (command.kind != RenderSurfaceCommandKind::Suspend)
                        snapshot.active.reset();
                    break;
                case Lost:
                    snapshot.state = RenderSurfaceState::Lost;
                    snapshot.active.reset();
                    break;
                case PreserveActive:
                    snapshot.state = priorState;
                    snapshot.active = priorActive;
                    break;
                case Unattached:
                    snapshot.state = RenderSurfaceState::Unattached;
                    snapshot.active.reset();
                    break;
            }
        }
    }  // namespace

    /** @copydoc RenderSurfaceLifecycle::Create */
    Result<RenderSurfaceLifecycle> RenderSurfaceLifecycle::Create(const std::uint64_t owner) {
        if (owner == 0)
            return Result<RenderSurfaceLifecycle>::Failure(MakeError(RenderSurfaceLifecycleErrors::InvalidIdentity));
        return Result<RenderSurfaceLifecycle>::Success(RenderSurfaceLifecycle{owner, ConstructionKey{}});
    }

    RenderSurfaceLifecycle::RenderSurfaceLifecycle(const std::uint64_t owner, ConstructionKey) noexcept
        : ownerThread_(std::this_thread::get_id()), snapshot_{.surface = {owner, 0}, .revision = 1} {}

    /** @copydoc RenderSurfaceLifecycle::RenderSurfaceLifecycle(RenderSurfaceLifecycle &&) */
    RenderSurfaceLifecycle::RenderSurfaceLifecycle(RenderSurfaceLifecycle &&other) noexcept
        : ownerThread_(std::exchange(other.ownerThread_, {})), snapshot_(std::exchange(other.snapshot_, {})),
          pending_(std::move(other.pending_)), inFlight_(std::move(other.inFlight_)), stateBeforeTransition_(other.stateBeforeTransition_),
          activeBeforeTransition_(std::move(other.activeBeforeTransition_)),
          lastAcceptedSequence_(std::exchange(other.lastAcceptedSequence_, 0)) {
        other.pending_.reset();
        other.inFlight_.reset();
        other.stateBeforeTransition_ = RenderSurfaceState::Unattached;
        other.activeBeforeTransition_.reset();
    }

    /** @copydoc RenderSurfaceLifecycle::Queue */
    Result<RenderSurfaceQueueResult> RenderSurfaceLifecycle::Queue(RenderSurfaceCommand command) {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<RenderSurfaceQueueResult>::Failure(MakeError(RenderSurfaceLifecycleErrors::WrongThread));
        if (!HasValidShape(command))
            return Result<RenderSurfaceQueueResult>::Failure(MakeError(RenderSurfaceLifecycleErrors::InvalidCommand));
        if (command.sequence <= lastAcceptedSequence_)
            return Result<RenderSurfaceQueueResult>::Failure(MakeError(RenderSurfaceLifecycleErrors::StaleRequest));
        if (!CanQueue(snapshot_, command.kind))
            return Result<RenderSurfaceQueueResult>::Failure(MakeError(RenderSurfaceLifecycleErrors::InvalidState));
        if (pending_.has_value() && (pending_->kind == RenderSurfaceCommandKind::Close ||
                                     (pending_->kind == RenderSurfaceCommandKind::Lose && command.kind != RenderSurfaceCommandKind::Close)))
            return Result<RenderSurfaceQueueResult>::Failure(MakeError(RenderSurfaceLifecycleErrors::InvalidState));

        RenderSurfaceQueueResult result;
        if (pending_.has_value()) {
            result.disposition = RenderSurfaceQueueDisposition::Coalesced;
            result.supersededSequence = pending_->sequence;
        }
        pending_ = std::move(command);
        lastAcceptedSequence_ = pending_->sequence;
        return Result<RenderSurfaceQueueResult>::Success(std::move(result));
    }

    /** @copydoc RenderSurfaceLifecycle::BeginFrameBoundary */
    Result<RenderSurfaceTransition> RenderSurfaceLifecycle::BeginFrameBoundary() {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<RenderSurfaceTransition>::Failure(MakeError(RenderSurfaceLifecycleErrors::WrongThread));
        if (inFlight_.has_value())
            return Result<RenderSurfaceTransition>::Failure(MakeError(RenderSurfaceLifecycleErrors::TransitionBusy));
        if (!pending_.has_value())
            return Result<RenderSurfaceTransition>::Failure(MakeError(RenderSurfaceLifecycleErrors::NoPendingRequest));
        if (snapshot_.revision >= std::numeric_limits<std::uint64_t>::max() - 1)
            return Result<RenderSurfaceTransition>::Failure(MakeError(RenderSurfaceLifecycleErrors::RevisionExhausted));
        if (!CanQueue(snapshot_, pending_->kind)) {
            const std::uint64_t invalidatedSequence = pending_->sequence;
            pending_.reset();
            return Result<RenderSurfaceTransition>::Failure(MakeError(RenderSurfaceLifecycleErrors::PendingRequestInvalidated,
                                                                      std::format("Queued surface command sequence {} is incompatible "
                                                                                  "with the realized surface state.",
                                                                                  invalidatedSequence)));
        }

        stateBeforeTransition_ = snapshot_.state;
        activeBeforeTransition_ = snapshot_.active;
        inFlight_ = RenderSurfaceTransition{snapshot_.surface, snapshot_.revision, std::move(*pending_)};
        pending_.reset();
        snapshot_.candidateSequence = inFlight_->command.sequence;
        ++snapshot_.revision;
        using enum RenderSurfaceCommandKind;
        if (inFlight_->command.kind == Close)
            snapshot_.state = RenderSurfaceState::Closing;
        else if (inFlight_->command.kind == Lose)
            snapshot_.state = RenderSurfaceState::Lost;
        else if (inFlight_->command.kind == Suspend)
            snapshot_.state = RenderSurfaceState::Suspended;
        else
            snapshot_.state = RenderSurfaceState::Reconfiguring;
        return Result<RenderSurfaceTransition>::Success(*inFlight_);
    }

    /** @copydoc RenderSurfaceLifecycle::Complete */
    Result<RenderSurfaceSnapshot> RenderSurfaceLifecycle::Complete(const RenderSurfaceTransition &transition,
                                                                   const RenderSurfaceRealization realization) {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<RenderSurfaceSnapshot>::Failure(MakeError(RenderSurfaceLifecycleErrors::WrongThread));
        if (!inFlight_.has_value() || transition != *inFlight_)
            return Result<RenderSurfaceSnapshot>::Failure(MakeError(RenderSurfaceLifecycleErrors::StaleTransition));
        if (!IsValidOutcome(transition.command, realization, stateBeforeTransition_, activeBeforeTransition_))
            return Result<RenderSurfaceSnapshot>::Failure(MakeError(RenderSurfaceLifecycleErrors::InvalidOutcome));
        if (snapshot_.revision == std::numeric_limits<std::uint64_t>::max())
            return Result<RenderSurfaceSnapshot>::Failure(MakeError(RenderSurfaceLifecycleErrors::RevisionExhausted));

        if (AdvancesGeneration(transition.command.kind, realization)) {
            if (snapshot_.surface.generation == std::numeric_limits<std::uint64_t>::max())
                return Result<RenderSurfaceSnapshot>::Failure(MakeError(RenderSurfaceLifecycleErrors::GenerationExhausted));
            ++snapshot_.surface.generation;
        }

        PublishRealization(snapshot_, transition.command, realization, stateBeforeTransition_, activeBeforeTransition_);
        snapshot_.candidateSequence.reset();
        ++snapshot_.revision;
        inFlight_.reset();
        return Result<RenderSurfaceSnapshot>::Success(snapshot_);
    }

    /** @copydoc RenderSurfaceLifecycle::Snapshot */
    Result<RenderSurfaceSnapshot> RenderSurfaceLifecycle::Snapshot() const {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<RenderSurfaceSnapshot>::Failure(MakeError(RenderSurfaceLifecycleErrors::WrongThread));
        return Result<RenderSurfaceSnapshot>::Success(snapshot_);
    }

    /** @copydoc RenderSurfaceLifecycle::HasTransitionInFlight */
    Result<bool> RenderSurfaceLifecycle::HasTransitionInFlight() const {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<bool>::Failure(MakeError(RenderSurfaceLifecycleErrors::WrongThread));
        return Result<bool>::Success(inFlight_.has_value());
    }

    /** @copydoc RenderSurfaceLifecycle::HasPendingRequest */
    Result<bool> RenderSurfaceLifecycle::HasPendingRequest() const {
        if (ownerThread_ != std::this_thread::get_id())
            return Result<bool>::Failure(MakeError(RenderSurfaceLifecycleErrors::WrongThread));
        return Result<bool>::Success(pending_.has_value());
    }
}  // namespace Horo::Render
