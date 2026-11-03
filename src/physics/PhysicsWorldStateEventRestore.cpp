#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsWorldStateEvents.h"

#include <algorithm>

namespace Horo::Physics::Detail {
    namespace {
        Result<void> Pairs(const CanonicalPhysicsStateWorkspace &workspace, std::span<const PhysicsStateEventPair> source,
                           std::span<PhysicsEventProjectionPairState> output) {
            const PhysicsStateEventPair *previous{};
            for (std::size_t index = 0; index < source.size(); ++index) {
                if (previous && !StatePairLess(*previous, source[index]))
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                auto mapped = RestorePhysicsStatePair(workspace, source[index]);
                if (mapped.HasError())
                    return Result<void>::Failure(mapped.ErrorValue());
                output[index] = mapped.Value();
                previous = &source[index];
            }
            std::ranges::sort(output.first(source.size()), {}, &PhysicsEventProjectionPairState::pair);
            return Result<void>::Success();
        }

        bool ValidEvent(const PhysicsStatePublishedEvent &event, const PhysicsStatePublishedEvent *previous, std::size_t count,
                        std::uint64_t tick) noexcept {
            const bool trigger = event.kind == PhysicsEventKind::TriggerEnter || event.kind == PhysicsEventKind::TriggerExit;
            return event.ordinal < count && event.tick == tick && event.kind <= PhysicsEventKind::TriggerExit &&
                   event.pair.sensor == trigger && (!previous || !StatePairLess(event.pair, previous->pair));
        }

        Result<void> Events(const CanonicalPhysicsStateWorkspace &workspace, const PhysicsStateWorldView &source,
                            std::span<PhysicsEventRecord> output) {
            const PhysicsStatePublishedEvent *previous{};
            for (auto &record : output.first(source.publishedEvents.size()))
                record = {};
            for (std::size_t index = 0; index < source.publishedEvents.size(); ++index) {
                const auto &event = source.publishedEvents[index];
                if (!ValidEvent(event, previous, source.publishedEvents.size(), source.completedTick))
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                if (output[event.ordinal].simulationTick != 0)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                auto mapped = RestorePhysicsStatePair(workspace, event.pair);
                if (mapped.HasError())
                    return Result<void>::Failure(mapped.ErrorValue());
                const auto &pair = mapped.Value();
                output[event.ordinal] = {event.tick,          event.kind,   pair.pair,         pair.firstMaterial,
                                         pair.secondMaterial, pair.contact, pair.firstSurface, pair.secondSurface};
                previous = &event;
            }
            // Dense unique original ordinals reconstruct the actual semantic queue without sorting it by physical handles.
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PreparePhysicsStateEvents */
    Result<void> PreparePhysicsStateEvents(const CanonicalPhysicsStateWorkspace &workspace, const PhysicsStateWorldView &source,
                                           std::span<PhysicsEventProjectionPairState> pairs, std::span<PhysicsEventRecord> events,
                                           PhysicsEventProjection &staging) {
        if (source.previousPairs.size() > pairs.size() || source.publishedEvents.size() > events.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (auto prepared = Pairs(workspace, source.previousPairs, pairs); prepared.HasError())
            return prepared;
        if (auto prepared = Events(workspace, source, events); prepared.HasError())
            return prepared;
        return staging.PrepareState({source.completedTick, source.droppedEventCount, pairs.first(source.previousPairs.size()),
                                     events.first(source.publishedEvents.size())});
    }
}  // namespace Horo::Physics::Detail
