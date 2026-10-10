#include "UiAnimationAdmission.h"
#include "UiAnimationOwnerInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Qualifies restarted definitions against previously reserved actual property targets. */
        [[nodiscard]] bool Overlaps(const UiAnimationDefinition &first, const UiAnimationDefinition &second) noexcept {
            return std::ranges::any_of(first.tracks, [&](const auto &track) {
                return std::ranges::any_of(second.tracks, [&](const auto &other) {
                    return track.target == other.target && track.property == other.property;
                });
            });
        }
    }  // namespace

    /** @copydoc UiAnimationOwner::RestartReloadTimelines */
    Result<std::uint32_t> UiAnimationOwner::RestartReloadTimelines(const Storage &source, Storage &replacement) {
        auto count = std::uint32_t{};
        for (const auto &old : source.timelines) {
            if (!old.occupied || old.required || old.terminalIssued || old.cursor.sample.outcome != UiAnimationOutcome::None)
                continue;
            const auto id = source.definition.animations[old.definition].id;
            const auto found = std::ranges::find(replacement.definition.animations, id, &UiAnimationDefinition::id);
            if (found == replacement.definition.animations.end())
                continue;
            // A restart is an explicit new admission; altered lifecycle/domain/overlap must still qualify normal commands.
            if (found->time.lifecycle != UiAnimationLifecycle::NonBlocking ||
                !replacement.binding.clocks.domains[static_cast<std::size_t>(found->time.domain)].available)
                return Result<std::uint32_t>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            for (std::uint32_t prior = 0; prior < count; ++prior) {
                if (Overlaps(*found, replacement.definition.animations[replacement.timelines[prior].definition]))
                    return Result<std::uint32_t>::Failure(MakeError(UiErrors::AnimationConflict));
            }
            auto &entry = replacement.timelines[count];
            entry.generation = 1;
            entry.definition = static_cast<std::uint32_t>(std::distance(replacement.definition.animations.begin(), found));
            entry.clock = replacement.binding.clocks.domains[static_cast<std::size_t>(found->time.domain)].clock;
            entry.pendingStart = true;
            entry.occupied = true;
            ++count;
        }
        return Result<std::uint32_t>::Success(count);
    }

    /** @copydoc UiAnimationOwner::PrepareReloadStorage */
    Result<std::shared_ptr<UiAnimationOwner::Storage>> UiAnimationOwner::PrepareReloadStorage(
        Storage &source, UiReloadGeneration &replacement, UiElementSlotAllocator &allocator, RuntimeStyleRegistry registry,
        UiStyleResolver styles, UiAnimationCanvasDefinition definition, const UiAnimationReloadPolicy policy) {
        const auto *canvas = replacement.Canvas(definition.canvas);
        if (!canvas || !canvas->tree.WasIssuedBy(allocator))
            return Result<std::shared_ptr<Storage>>::Failure(MakeError(UiErrors::AnimationTargetStale));
        auto reserved = allocator.Reserve(static_cast<std::uint32_t>(UiTimeDomainCount) + 1 + source.limits.timelines);
        if (reserved.HasError())
            return Result<std::shared_ptr<Storage>>::Failure(reserved.ErrorValue());
        try {
            // The inert private holder has no current generation and admits no operation. Only the successful structural
            // publication transfers the existing sole publisher into this fully prepared replacement.
            auto next =
                std::make_shared<Storage>(UiHotReload{std::shared_ptr<UiHotReload::Storage>{}}, std::move(registry), std::move(styles),
                                          std::move(definition), source.limits, std::move(reserved).Value(), canvas->controls.size());
            if (const auto bound = InitializeBindings(*next, replacement); bound.HasError())
                return Result<std::shared_ptr<Storage>>::Failure(bound.ErrorValue());
            for (std::size_t index = 0; index < UiTimeDomainCount; ++index) {
                const auto fresh = next->binding.clocks.domains[index].clock;
                next->binding.clocks.domains[index] = source.binding.clocks.domains[index];
                next->binding.clocks.domains[index].clock = fresh;
                next->binding.clocks.domains[index].delta = {};
                if (index >= static_cast<std::size_t>(UiTimeDomain::EditorPreview)) {
                    next->binding.clocks.domains[index].sequence = 0;
                    next->binding.clocks.domains[index].sourceRevision = 0;
                }
            }
            next->binding.clocks.updateSequence = source.binding.clocks.updateSequence;
            next->binding.bound = source.binding.bound;
            next->lastSourceFrame = source.lastSourceFrame;
            if (policy == UiAnimationReloadPolicy::Restart) {
                if (const auto restarted = RestartReloadTimelines(source, *next); restarted.HasError())
                    return Result<std::shared_ptr<Storage>>::Failure(restarted.ErrorValue());
            }
            return Result<std::shared_ptr<Storage>>::Success(std::move(next));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<Storage>>::Failure(MakeError(UiErrors::AnimationStorageExhausted));
        }
    }

    /** @copydoc UiAnimationOwner::PrepareReloadOutcomes */
    Result<UiAnimationReloadResult> UiAnimationOwner::PrepareReloadOutcomes(const Storage &source) {
        try {
            UiAnimationReloadResult result;
            result.terminal.reserve(source.timelines.size());
            for (std::size_t index = 0; index < source.timelines.size(); ++index) {
                const auto &timeline = source.timelines[index];
                if (!timeline.occupied || timeline.terminalIssued || timeline.cursor.sample.outcome != UiAnimationOutcome::None)
                    continue;
                auto sample = timeline.cursor.sample;
                sample.state = UiAnimationState::Cancelled;
                sample.outcome = UiAnimationOutcome::Cancelled;
                sample.cancellation = UiAnimationCancellation::Reload;
                sample.newTerminalOutcome = true;
                sample.contributesValue = false;
                const auto slot =
                    source.range.FirstSlot() + static_cast<std::uint32_t>(UiTimeDomainCount) + 1 + static_cast<std::uint32_t>(index);
                result.terminal.push_back({{source.binding.source.ownership, slot, timeline.generation},
                                           source.definition.animations[timeline.definition].id,
                                           sample});
            }
            result.cancelledTimelines = static_cast<std::uint32_t>(result.terminal.size());
            return Result<UiAnimationReloadResult>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<UiAnimationReloadResult>::Failure(MakeError(UiErrors::AnimationStorageExhausted));
        }
    }

    /** @copydoc UiAnimationOwner::Reload */
    Result<UiAnimationReloadResult> UiAnimationOwner::Reload(UiReloadGeneration replacement, UiElementSlotAllocator &allocator,
                                                             RuntimeStyleRegistry registry, UiStyleResolver styles,
                                                             UiAnimationCanvasDefinition definition,
                                                             const UiAnimationReloadAdmission &admission,
                                                             const CancellationToken &cancellation) {
        const auto policy = admission.policy;
        const auto point = admission.point;
        if (!storage_ || policy > UiAnimationReloadPolicy::Restart)
            return Result<UiAnimationReloadResult>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        if (const auto admitted = AdmitCommand(*storage_); admitted.HasError())
            return Result<UiAnimationReloadResult>::Failure(admitted.ErrorValue());
        if (storage_->pointerDispatching)
            return Result<UiAnimationReloadResult>::Failure(MakeError(UiErrors::EventDispatchReentrant));
        if (storage_->route.gate)
            return Result<UiAnimationReloadResult>::Failure(MakeError(UiErrors::AnimationConflict));
        auto prepared = storage_->publisher.Prepare(std::move(replacement), cancellation);
        if (prepared.HasError())
            return Result<UiAnimationReloadResult>::Failure(prepared.ErrorValue());
        auto reservation = std::move(prepared).Value();
        auto next = PrepareReloadStorage(*storage_, *storage_->publisher.AnimationReplacement(reservation), allocator, std::move(registry),
                                         std::move(styles), std::move(definition), policy);
        if (next.HasError())
            return Result<UiAnimationReloadResult>::Failure(next.ErrorValue());
        auto outcomes = PrepareReloadOutcomes(*storage_);
        if (outcomes.HasError())
            return outcomes;
        auto result = std::move(outcomes).Value();
        const auto committed = storage_->publisher.Commit(reservation, point);
        if (committed.HasError())
            return Result<UiAnimationReloadResult>::Failure(committed.ErrorValue());
        result.state = committed.Value();
        for (const auto &timeline : next.Value()->timelines)
            result.restartedTimelines += timeline.occupied ? 1U : 0U;
        storage_->stopped = true;
        if (storage_->resolver.State() == UiStyleResolverState::Active)
            (void)storage_->resolver.BeginRetirement();
        if (storage_->registry.State() == RuntimeStyleRegistryState::Active)
            (void)storage_->registry.BeginRetirement();
        next.Value()->publisher = std::move(storage_->publisher);
        storage_.PublisherPin() = std::move(next).Value();
        return Result<UiAnimationReloadResult>::Success(std::move(result));
    }
}  // namespace Horo::Runtime::Ui
