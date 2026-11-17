#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsWorldStateEvents.h"

#include <algorithm>

namespace Horo::Physics::Detail {
    namespace {
        PhysicsEventProjectionPairState Pair(const PhysicsEventRecord &event) noexcept {
            return {event.pair,
                    event.firstMaterial,
                    event.secondMaterial,
                    event.firstSurface,
                    event.secondSurface,
                    event.contact,
                    event.kind == PhysicsEventKind::TriggerEnter || event.kind == PhysicsEventKind::TriggerExit};
        }

        Result<void> Pairs(const CanonicalPhysicsStateWorkspace &workspace, std::span<const PhysicsEventProjectionPairState> source,
                           std::span<PhysicsStateEventPair> output) {
            for (std::size_t index = 0; index < source.size(); ++index) {
                auto mapped = CapturePhysicsStatePair(workspace, source[index]);
                if (mapped.HasError())
                    return Result<void>::Failure(mapped.ErrorValue());
                output[index] = mapped.Value();
            }
            std::ranges::sort(output.first(source.size()), StatePairLess);
            return Result<void>::Success();
        }

        Result<void> Events(const CanonicalPhysicsStateWorkspace &workspace, std::span<const PhysicsEventRecord> source,
                            std::span<PhysicsStatePublishedEvent> output) {
            for (std::size_t index = 0; index < source.size(); ++index) {
                auto mapped = CapturePhysicsStatePair(workspace, Pair(source[index]));
                if (mapped.HasError())
                    return Result<void>::Failure(mapped.ErrorValue());
                output[index] = {source[index].simulationTick, static_cast<std::uint32_t>(index), source[index].kind, mapped.Value()};
            }
            auto used = output.first(source.size());
            std::ranges::sort(used, [](const auto &one, const auto &two) {
                if (StatePairLess(one.pair, two.pair))
                    return true;
                if (StatePairLess(two.pair, one.pair))
                    return false;
                return one.ordinal < two.ordinal;
            });
            // Rows are canonical stable-key order; ordinal retains the ORIGINAL semantic delivery position.
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CapturePhysicsStateEvents */
    Result<PhysicsStateEventCounts> CapturePhysicsStateEvents(const CanonicalPhysicsStateWorkspace &workspace,
                                                              const PhysicsEventProjectionStateView &source,
                                                              std::span<PhysicsStateEventPair> pairs,
                                                              std::span<PhysicsStatePublishedEvent> events) {
        using Outcome = Result<PhysicsStateEventCounts>;
        if (source.pairs.size() > pairs.size() || source.events.size() > events.size())
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (auto mapped = Pairs(workspace, source.pairs, pairs); mapped.HasError())
            return Outcome::Failure(mapped.ErrorValue());
        if (auto mapped = Events(workspace, source.events, events); mapped.HasError())
            return Outcome::Failure(mapped.ErrorValue());
        return Outcome::Success({source.pairs.size(), source.events.size()});
    }
}  // namespace Horo::Physics::Detail
