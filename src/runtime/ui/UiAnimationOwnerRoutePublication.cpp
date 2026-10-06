#include "UiAnimationOwnerInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Qualifies only bounded actual retained-tree parent lineage. */
        bool WithinRoot(const UiElementTree &tree, UiElementHandle target, const UiElementId rootId) {
            const auto root = tree.Find(rootId);
            for (std::size_t count = 0; root.HasValue() && count < tree.Size() && target.IsValid(); ++count) {
                if (target == root.Value())
                    return true;
                const auto record = tree.Get(target);
                if (record.HasError())
                    return false;
                target = record.Value().parent;
            }
            return false;
        }
    }  // namespace

    /** @copydoc UiAnimationOwner::ReadCanvas */
    const UiReloadCanvas *UiAnimationOwner::ReadCanvas(const Storage &storage) noexcept {
        const auto *generation = storage.candidate.admitted ? storage.candidate.source.Get()
                                 : storage.currentFrame     ? storage.frames[*storage.currentFrame]->generation.Get()
                                                            : nullptr;
        if (!generation)
            return nullptr;
        const auto canvases = generation->Canvases();
        const auto found = std::ranges::find(canvases, storage.definition.canvas, &UiReloadCanvas::id);
        return found == canvases.end() ? nullptr : &*found;
    }

    /** @copydoc UiAnimationOwner::PrepareRouteClock */
    Result<void> UiAnimationOwner::PrepareRouteClock(Storage &storage) {
        auto &candidate = storage.candidate;
        const auto &route = storage.route;
        candidate.routeStage = route.stage;
        candidate.routeElapsed = route.elapsed;
        candidate.routeCancellation = route.cancellation;
        candidate.routeTerminal = false;
        candidate.routeChanged = false;
        auto &frame = *storage.frames[candidate.frameSlot];
        auto &clock = frame.clocks.domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)];
        clock.available = false;
        clock.delta = {};
        clock.routeStack = {};
        clock.route = {};
        clock.routeOperation = {};
        if (!route.gate || route.stage == route.stages.size())
            return Result<void>::Success();
        const auto &stage = route.stages[route.stage];
        const auto delta = frame.clocks.domains[static_cast<std::size_t>(UiTimeDomain::PresentationUnscaled)].delta;
        if (delta.nanoseconds > std::numeric_limits<std::int64_t>::max() - route.elapsed.nanoseconds)
            return Result<void>::Failure(MakeError(UiErrors::ClockOverflow));
        candidate.routeElapsed.nanoseconds += delta.nanoseconds;
        clock.clock = stage.clock;
        clock.elapsed = candidate.routeElapsed;
        clock.delta = delta;
        clock.sequence = frame.clocks.updateSequence;
        clock.sourceRevision = route.gate->transaction_.Operation().sequence.Value();
        clock.routeStack = storage.publisher.Current()->Canvas(storage.definition.canvas)->routes->Stack();
        clock.route = stage.scope;
        clock.routeOperation = route.gate->transaction_.Operation().sequence;
        clock.continuity = route.elapsed.nanoseconds == 0 ? UiClockContinuity::BaselineReset : UiClockContinuity::Continuous;
        clock.available = true;
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::PrepareRouteProgress */
    Result<void> UiAnimationOwner::PrepareRouteProgress(Storage &storage) {
        auto &candidate = storage.candidate;
        const auto &route = storage.route;
        if (!route.gate)
            return Result<void>::Success();
        if (route.gate->transaction_.preparedRejection_ != UiRouteOperationRejection::None ||
            candidate.routeCancellation != UiAnimationCancellation::None || route.stages.empty()) {
            candidate.routeTerminal = true;
        } else {
            const auto &stage = route.stages[route.stage];
            const auto &timeline = candidate.timelines[stage.timeline];
            if (timeline.cursor.sample.outcome == UiAnimationOutcome::Completed) {
                ++candidate.routeStage;
                candidate.routeElapsed = {};
                candidate.routeTerminal = candidate.routeStage == route.stages.size();
                if (!candidate.routeTerminal) {
                    candidate.timelines[stage.timeline].occupied = false;
                    candidate.timelines[route.stages[candidate.routeStage].timeline].waiting = false;
                }
            }
        }
        candidate.routeChanged = candidate.routeTerminal || !route.published;
        // Route eligibility changes use the actual layout owner's checked interaction revision, even for identical boxes.
        if (candidate.routeChanged)
            return storage.publisher.Current()
                ->Canvas(storage.definition.canvas)
                ->layoutEngine->Invalidate(
                    {{}, storage.publisher.Current()->Canvas(storage.definition.canvas)->tree.Revision(), UiLayoutDirtyKind::All});
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::CanPublishRoute */
    Result<void> UiAnimationOwner::CanPublishRoute(const Storage &storage) {
        if (!storage.route.gate)
            return Result<void>::Success();
        const auto *canvas = ReadCanvas(storage);
        if (!canvas || !canvas->routes)
            return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
        const auto *stack = &*canvas->routes;
        const auto &gate = *storage.route.gate;
        if (!storage.candidate.routeTerminal || storage.candidate.routeCancellation != UiAnimationCancellation::None ||
            gate.transaction_.preparedRejection_ != UiRouteOperationRejection::None)
            return stack->CanCloseAnimation(gate);
        const UiScreenStack::AnimationTerminalProof proof{gate, storage.candidate.layout->Candidate().Descriptor().interaction};
        return stack->CanPublishAnimation(gate, proof);
    }

    /** @copydoc UiAnimationOwner::PublishRouteValidated */
    void UiAnimationOwner::PublishRouteValidated(Storage &storage) noexcept {
        auto &route = storage.route;
        auto &frame = *storage.frames[storage.candidate.frameSlot];
        auto *canvas = storage.publisher.Current()->Canvas(storage.definition.canvas);
        if (!canvas->routes) {
            frame.routes.clear();
            frame.route.reset();
            return;
        }
        auto *stack = &*canvas->routes;
        if (route.gate) {
            const bool stageChanged = route.stage != storage.candidate.routeStage;
            route.published = true;
            route.stage = storage.candidate.routeStage;
            route.elapsed = storage.candidate.routeElapsed;
            route.cancellation = storage.candidate.routeCancellation;
            if (storage.candidate.routeTerminal) {
                const auto rejection = route.gate->transaction_.preparedRejection_;
                if (route.cancellation != UiAnimationCancellation::None || rejection != UiRouteOperationRejection::None) {
                    route.record->terminal = stack->CloseAnimationValidated(*route.gate, rejection == UiRouteOperationRejection::None
                                                                                             ? UiRouteOperationRejection::Cancelled
                                                                                             : rejection);
                    route.record->phase =
                        rejection == UiRouteOperationRejection::None ? UiAnimationRoutePhase::Cancelled : UiAnimationRoutePhase::Rejected;
                    route.record->cancellation = route.cancellation;
                } else {
                    const UiScreenStack::AnimationTerminalProof proof{*route.gate, frame.layout->Descriptor().interaction};
                    route.record->terminal = stack->PublishAnimationValidated(*route.gate, proof);
                    route.record->phase = UiAnimationRoutePhase::Completed;
                }
                auto &child = frame.clocks.domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)];
                child.available = false;
                child.delta = {};
                route.gate.reset();
            } else {
                const auto &stage = route.stages[route.stage];
                route.record->phase = stageChanged ? UiAnimationRoutePhase::Waiting : stage.phase;
                if (stageChanged) {
                    auto &child = frame.clocks.domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)];
                    child.available = false;
                    child.delta = {};
                }
                route.record->scope = stage.scope;
                route.record->root = stage.root;
            }
        }
        frame.routes.assign(stack->Routes().begin(), stack->Routes().end());
        frame.route = route.record;
    }

    /** @copydoc UiAnimationOwner::CancelRouteValidated */
    void UiAnimationOwner::CancelRouteValidated(Storage &storage, const UiAnimationCancellation reason) noexcept {
        if (!storage.route.gate)
            return;
        auto *stack = &*storage.publisher.Current()->Canvas(storage.definition.canvas)->routes;
        storage.route.record->terminal = stack->CloseAnimationValidated(*storage.route.gate, UiRouteOperationRejection::Cancelled);
        storage.route.record->phase = UiAnimationRoutePhase::Cancelled;
        storage.route.record->cancellation = reason;
        storage.route.gate.reset();
        for (auto &timeline : storage.timelines) {
            if (timeline.required) {
                timeline.cancellation = reason;
                timeline.occupied = false;
            }
        }
    }

    /** @copydoc UiAnimationOwner::PreparedRouteTargetEligible */
    bool UiAnimationOwner::PreparedRouteTargetEligible(const Storage &storage, UiElementHandle target) {
        if (!storage.route.gate)
            return RouteTargetEligible(storage, target);
        if (!storage.candidate.routeTerminal)
            return false;
        const auto &gate = *storage.route.gate;
        if (storage.candidate.routeCancellation != UiAnimationCancellation::None ||
            gate.transaction_.preparedRejection_ != UiRouteOperationRejection::None) {
            // Cancellation preserves the exact current stack; this candidate only reopens its presented eligibility.
            const auto *canvas = ReadCanvas(storage);
            if (!canvas)
                return false;
            const auto top = canvas->routes->Top();
            if (!top)
                return false;
            const auto binding = std::ranges::find(storage.definition.routes, top->metadata.id, &UiAnimationRouteBinding::route);
            if (binding == storage.definition.routes.end())
                return true;
            return WithinRoot(canvas->tree, target, binding->root);
        }
        const auto *canvas = ReadCanvas(storage);
        if (!canvas)
            return false;
        const auto kind = gate.transaction_.request_.kind;
        auto route = gate.transaction_.request_.route;
        if (kind == UiRouteOperationKind::Clear)
            return false;
        if (kind == UiRouteOperationKind::Pop || kind == UiRouteOperationKind::Back) {
            const auto routes = canvas->routes->Routes();
            if (routes.size() <= 1)
                return false;
            route = routes[routes.size() - 2].metadata.id;
        }
        const auto binding = std::ranges::find(storage.definition.routes, *route, &UiAnimationRouteBinding::route);
        if (binding == storage.definition.routes.end())
            return true;
        return WithinRoot(canvas->tree, target, binding->root);
    }

    /** @copydoc UiAnimationOwner::RouteTargetEligible */
    bool UiAnimationOwner::RouteTargetEligible(const Storage &storage, UiElementHandle target) {
        if (storage.route.gate)
            return false;
        const auto *canvas = ReadCanvas(storage);
        if (!canvas)
            return false;
        if (!canvas->routes || storage.definition.routes.empty())
            return true;
        const auto top = canvas->routes->Top();
        if (!top)
            return false;
        const auto binding = std::ranges::find(storage.definition.routes, top->metadata.id, &UiAnimationRouteBinding::route);
        if (binding == storage.definition.routes.end())
            return true;
        return WithinRoot(canvas->tree, target, binding->root);
    }
}  // namespace Horo::Runtime::Ui
