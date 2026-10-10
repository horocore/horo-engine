#include "UiAnimationOwnerInternal.h"

#include <algorithm>
#include <limits>
#include <memory>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Returns whether two admitted immutable definitions reserve the same actual element/property pair. */
        [[nodiscard]] bool Overlaps(const UiAnimationDefinition &first, const UiAnimationDefinition &second) noexcept {
            for (const auto &track : first.tracks) {
                if (std::ranges::any_of(second.tracks, [&track](const auto &other) {
                    return track.target == other.target && track.property == other.property;
                }))
                    return true;
            }
            return false;
        }
    }  // namespace

    /** @copydoc UiAnimationOwner::AdmitCommand */
    Result<void> UiAnimationOwner::AdmitCommand(const Storage &storage) {
        if (storage.ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (storage.stopped || storage.draining || storage.candidate.admitted || storage.pointerDispatching)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (storage.pendingCommands == storage.limits.commands)
            return Result<void>::Failure(MakeError(UiErrors::AnimationBudgetExceeded));
        if (storage.commandRevision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(UiErrors::ClockOverflow));
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::Start */
    Result<UiAnimationTimelineId> UiAnimationOwner::Start(const UiAnimationId animation) {
        if (!storage_)
            return Result<UiAnimationTimelineId>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (const auto admitted = AdmitCommand(*storage_); admitted.HasError())
            return Result<UiAnimationTimelineId>::Failure(admitted.ErrorValue());
        const auto definition = std::ranges::find(storage_->definition.animations, animation, &UiAnimationDefinition::id);
        if (definition == storage_->definition.animations.end() || definition->time.lifecycle != UiAnimationLifecycle::NonBlocking)
            return Result<UiAnimationTimelineId>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        if (!storage_->binding.bound || !storage_->binding.clocks.domains[static_cast<std::size_t>(definition->time.domain)].available)
            return Result<UiAnimationTimelineId>::Failure(MakeError(UiErrors::ClockUnavailable));
        for (const auto &timeline : storage_->timelines) {
            if (timeline.occupied && !timeline.terminalIssued && timeline.cursor.sample.outcome == UiAnimationOutcome::None &&
                timeline.cancellation == UiAnimationCancellation::None &&
                Overlaps(*definition, storage_->definition.animations[timeline.definition]))
                return Result<UiAnimationTimelineId>::Failure(MakeError(UiErrors::AnimationConflict));
        }
        const auto free = std::ranges::find_if(storage_->timelines, [](const auto &timeline) {
            return (!timeline.occupied || timeline.terminalIssued || timeline.cursor.sample.outcome != UiAnimationOutcome::None) &&
                   timeline.generation != std::numeric_limits<std::uint32_t>::max();
        });
        if (free == storage_->timelines.end())
            return Result<UiAnimationTimelineId>::Failure(MakeError(UiErrors::AnimationStorageExhausted));
        // A new admitted definition explicitly replaces conflicting retained terminal fill; two sampled values must never
        // choose an implicit winner for the same property. Older immutable frame records keep their terminal evidence.
        for (auto &timeline : storage_->timelines) {
            if (&timeline != std::to_address(free) && timeline.occupied &&
                (timeline.terminalIssued || timeline.cursor.sample.outcome != UiAnimationOutcome::None) &&
                Overlaps(*definition, storage_->definition.animations[timeline.definition]))
                timeline.occupied = false;
        }
        const auto slot = static_cast<std::uint32_t>(std::distance(storage_->timelines.begin(), free));
        ++free->generation;
        free->definition = static_cast<std::uint32_t>(std::distance(storage_->definition.animations.begin(), definition));
        free->cursor = {};
        free->clock = storage_->binding.clocks.domains[static_cast<std::size_t>(definition->time.domain)].clock;
        free->origin = {};
        free->pendingStart = true;
        free->terminalIssued = false;
        free->cancellation = UiAnimationCancellation::None;
        free->occupied = true;
        free->required = false;
        free->waiting = false;
        ++storage_->pendingCommands;
        ++storage_->commandRevision;
        return Result<UiAnimationTimelineId>::Success(
            {storage_->binding.source.ownership, storage_->range.FirstSlot() + static_cast<std::uint32_t>(UiTimeDomainCount) + 1 + slot,
             free->generation});
    }

    /** @copydoc UiAnimationOwner::Cancel */
    Result<void> UiAnimationOwner::Cancel(const UiAnimationTimelineId timeline, const UiAnimationCancellation reason) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (storage_->stopped || storage_->draining || storage_->candidate.admitted)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (reason == UiAnimationCancellation::None || reason > UiAnimationCancellation::AccessibilityReplacement)
            return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        const auto first = storage_->range.FirstSlot() + static_cast<std::uint32_t>(UiTimeDomainCount) + 1;
        if (timeline.ownership != storage_->binding.source.ownership || timeline.slot < first ||
            timeline.slot - first >= storage_->timelines.size())
            return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
        auto &entry = storage_->timelines[timeline.slot - first];
        if (!entry.occupied || entry.generation != timeline.generation || entry.terminalIssued ||
            entry.cursor.sample.outcome != UiAnimationOutcome::None)
            return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
        if (entry.required && storage_->route.gate)
            return CancelNavigation(storage_->route.gate->transaction_.Operation(), reason);
        if (entry.cancellation != UiAnimationCancellation::None)
            return entry.cancellation == reason ? Result<void>::Success() : Result<void>::Failure(MakeError(UiErrors::AnimationConflict));
        if (auto admitted = AdmitCommand(*storage_); admitted.HasError())
            return admitted;
        entry.cancellation = reason;
        ++storage_->pendingCommands;
        ++storage_->commandRevision;
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
