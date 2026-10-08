#include "UiAnimationIncarnations.h"
#include "UiAnimationOwnerInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime::Ui {
    /** @copydoc UiAnimationOwner::AppendRouteStage */
    Result<void> UiAnimationOwner::AppendRouteStage(Storage &storage, const UiRouteId route, const UiRouteInstanceId scope,
                                                    const bool entering) {
        const auto binding = std::ranges::find(storage.definition.routes, route, &UiAnimationRouteBinding::route);
        if (binding == storage.definition.routes.end())
            return Result<void>::Success();
        const auto animation = entering ? binding->enter : binding->exit;
        if (!animation)
            return Result<void>::Success();
        if (storage.route.stages.size() == storage.limits.timelines)
            return Result<void>::Failure(MakeError(UiErrors::AnimationStorageExhausted));
        const auto definition = std::ranges::find(storage.definition.animations, *animation, &UiAnimationDefinition::id);
        const auto root = storage.publisher.Current()->Canvas(storage.definition.canvas)->tree.Find(binding->root);
        if (definition == storage.definition.animations.end() || root.HasError())
            return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
        storage.route.stages.push_back({static_cast<std::uint32_t>(definition - storage.definition.animations.begin()),
                                        0,
                                        {},
                                        scope,
                                        root.Value(),
                                        binding->maximumWait,
                                        entering ? UiAnimationRoutePhase::Entering : UiAnimationRoutePhase::Exiting});
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::CheckRouteConflicts */
    Result<void> UiAnimationOwner::CheckRouteConflicts(const Storage &storage, const UiAnimationDefinition &definition) {
        for (const auto &timeline : storage.timelines) {
            if (!timeline.occupied || timeline.terminalIssued || timeline.cursor.sample.outcome != UiAnimationOutcome::None)
                continue;
            for (const auto &track : definition.tracks) {
                const auto &other = storage.definition.animations[timeline.definition].tracks;
                if (std::ranges::any_of(other, [&](const auto &value) {
                    return value.target == track.target && value.property == track.property;
                }))
                    return Result<void>::Failure(MakeError(UiErrors::AnimationConflict));
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::RetireRouteTimelines */
    void UiAnimationOwner::RetireRouteTimelines(Storage &storage) noexcept {
        for (auto &timeline : storage.timelines) {
            if (timeline.required) {
                timeline.occupied = false;
                continue;
            }
            if (!timeline.occupied || !timeline.terminalIssued)
                continue;
            const auto &prior = storage.definition.animations[timeline.definition];
            const bool overlaps = std::ranges::any_of(storage.route.stages, [&](const auto &stage) {
                return std::ranges::any_of(storage.definition.animations[stage.definition].tracks, [&](const auto &track) {
                    return std::ranges::any_of(prior.tracks, [&](const auto &value) {
                        return value.target == track.target && value.property == track.property;
                    });
                });
            });
            if (overlaps)
                timeline.occupied = false;
        }
    }

    /** @copydoc UiAnimationOwner::ReserveRouteStages */
    Result<void> UiAnimationOwner::ReserveRouteStages(Storage &storage) {
        if (storage.route.stages.size() > std::numeric_limits<std::uint32_t>::max() - storage.issuedRouteClockGeneration)
            return Result<void>::Failure(MakeError(UiErrors::ClockOverflow));
        for (auto &stage : storage.route.stages) {
            const auto &definition = storage.definition.animations[stage.definition];
            if (const auto conflicts = CheckRouteConflicts(storage, definition); conflicts.HasError())
                return conflicts;
            const auto prior = std::span(storage.route.stages).first(static_cast<std::size_t>(&stage - storage.route.stages.data()));
            const auto free = std::ranges::find_if(storage.timelines, [&](const auto &timeline) {
                const auto index = static_cast<std::uint32_t>(&timeline - storage.timelines.data());
                return (!timeline.occupied || timeline.terminalIssued) &&
                       timeline.generation != std::numeric_limits<std::uint32_t>::max() &&
                       std::ranges::find(prior, index, &Storage::RouteStage::timeline) == prior.end();
            });
            if (free == storage.timelines.end())
                return Result<void>::Failure(MakeError(UiErrors::AnimationStorageExhausted));
            stage.timeline = static_cast<std::uint32_t>(free - storage.timelines.begin());
        }
        const auto clocks = AnimationInternal::ReserveChildIncarnations(storage.issuedRouteClockGeneration,
                                                                        static_cast<std::uint32_t>(storage.route.stages.size()));
        if (!clocks.has_value())
            return Result<void>::Failure(MakeError(UiErrors::ClockOverflow));
        // Every capacity/conflict check completed before any reserved incarnation becomes active.
        RetireRouteTimelines(storage);
        for (auto &stage : storage.route.stages) {
            stage.clock = {storage.binding.source.ownership,
                           storage.range.FirstSlot() + static_cast<std::uint32_t>(UiTimeDomain::ScreenTransition) + 1,
                           *clocks + static_cast<std::uint32_t>(&stage - storage.route.stages.data())};
            auto &timeline = storage.timelines[stage.timeline];
            const auto generation = timeline.generation + 1;
            timeline = {};
            timeline.generation = generation;
            timeline.definition = stage.definition;
            timeline.clock = stage.clock;
            timeline.occupied = true;
            timeline.required = true;
            timeline.waiting = &stage != storage.route.stages.data();
            timeline.pendingStart = true;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::AdmitRouteStages */
    Result<void> UiAnimationOwner::AdmitRouteStages(Storage &storage) {
        const auto *stack = &*storage.publisher.Current()->Canvas(storage.definition.canvas)->routes;
        const auto &gate = *storage.route.gate;
        if (gate.transaction_.preparedRejection_ != UiRouteOperationRejection::None)
            return Result<void>::Success();
        const auto routes = stack->Routes();
        const auto kind = gate.transaction_.request_.kind;
        if (kind == UiRouteOperationKind::Clear) {
            for (auto route = routes.rbegin(); route != routes.rend(); ++route) {
                if (auto stage = AppendRouteStage(storage, route->metadata.id, route->id, false); stage.HasError())
                    return stage;
            }
        } else if (!routes.empty() && kind != UiRouteOperationKind::Push && kind != UiRouteOperationKind::Navigate) {
            if (auto stage = AppendRouteStage(storage, routes.back().metadata.id, routes.back().id, false); stage.HasError())
                return stage;
        }
        if (gate.instance_) {
            if (auto stage = AppendRouteStage(storage, *gate.transaction_.request_.route, *gate.instance_, true); stage.HasError())
                return stage;
        } else if ((kind == UiRouteOperationKind::Pop || kind == UiRouteOperationKind::Back) && routes.size() > 1) {
            const auto &revealed = routes[routes.size() - 2];
            if (auto stage = AppendRouteStage(storage, revealed.metadata.id, revealed.id, true); stage.HasError())
                return stage;
        }
        return ReserveRouteStages(storage);
    }

    /** @copydoc UiAnimationOwner::Navigate */
    Result<UiRouteOperationId> UiAnimationOwner::Navigate(const UiRouteOperationRequest &request) {
        if (!storage_)
            return Result<UiRouteOperationId>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (auto admitted = AdmitCommand(*storage_); admitted.HasError())
            return Result<UiRouteOperationId>::Failure(admitted.ErrorValue());
        auto *canvas = storage_->publisher.Current()->Canvas(storage_->definition.canvas);
        if (!canvas->routes || storage_->route.gate || !storage_->binding.bound)
            return Result<UiRouteOperationId>::Failure(MakeError(UiErrors::AnimationConflict));
        auto prepared = canvas->routes->PrepareAnimation(request, canvas->layoutEngine->PublishedInteraction());
        if (prepared.HasError())
            return Result<UiRouteOperationId>::Failure(prepared.ErrorValue());
        auto &route = storage_->route;
        route.stages.clear();
        route.gate.emplace(std::move(prepared).Value());
        if (auto admitted = AdmitRouteStages(*storage_); admitted.HasError()) {
            route.gate.reset();
            route.stages.clear();
            return Result<UiRouteOperationId>::Failure(admitted.ErrorValue());
        }
        route.stage = 0;
        route.published = false;
        route.elapsed = {};
        route.cancellation = UiAnimationCancellation::None;
        const auto operation = route.gate->transaction_.Operation();
        route.record = UiAnimationRouteRecord{operation};
        ++storage_->pendingCommands;
        ++storage_->commandRevision;
        return Result<UiRouteOperationId>::Success(operation);
    }

    /** @copydoc UiAnimationOwner::CancelNavigation */
    Result<void> UiAnimationOwner::CancelNavigation(const UiRouteOperationId operation, const UiAnimationCancellation reason) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (!storage_->route.gate || storage_->route.gate->transaction_.Operation() != operation)
            return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
        if (reason == UiAnimationCancellation::None || reason > UiAnimationCancellation::AccessibilityReplacement)
            return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        if (storage_->route.cancellation == reason)
            return Result<void>::Success();
        if (auto admitted = AdmitCommand(*storage_); admitted.HasError())
            return admitted;
        storage_->route.cancellation = reason;
        ++storage_->pendingCommands;
        ++storage_->commandRevision;
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
