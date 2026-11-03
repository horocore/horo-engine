#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsEventProjection.h"

#include <algorithm>
#include <cstdlib>
#include <type_traits>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Exactly the two transitions emitted by the existing lifecycle reducer
         * for a same-pair sensor-mode change. */
        bool Transition(PhysicsEventKind previous, PhysicsEventKind next) noexcept {
            using enum PhysicsEventKind;
            return (previous == ContactEnd && next == TriggerEnter) || (previous == TriggerExit && next == ContactBegin);
        }

        /** @brief Reuses complete pair evidence without fabricating a current
         * travel/contact tick. */
        PhysicsEventProjectionPairState Pair(const PhysicsEventRecord &event) noexcept {
            return {event.pair,
                    event.firstMaterial,
                    event.secondMaterial,
                    event.firstSurface,
                    event.secondSurface,
                    event.contact,
                    event.kind == PhysicsEventKind::TriggerEnter || event.kind == PhysicsEventKind::TriggerExit};
        }
    }  // namespace

    /** @copydoc PhysicsEventProjection::ValidStatePair */
    bool PhysicsEventProjection::ValidStatePair(const PhysicsEventProjectionPairState &pair, std::uint64_t tick) noexcept {
        if (!(pair.pair.first < pair.pair.second))
            return false;
        PhysicsContactObservation observation{tick,         pair.pair.first, pair.pair.second,  pair.firstMaterial, pair.secondMaterial,
                                              pair.contact, pair.sensor,     pair.firstSurface, pair.secondSurface};
        if (!ValidObservation(observation))
            return false;
        auto canonical = observation;
        Canonicalize(canonical);
        return canonical == observation;
    }

    /** @copydoc PhysicsEventProjection::PrepareStateEventOrder */
    bool PhysicsEventProjection::PrepareStateEventOrder(std::span<const PhysicsEventRecord> events, std::uint64_t tick) noexcept {
        if (events.size() > stateEventIndices_.size())
            return false;
        auto indexes = std::span{stateEventIndices_}.first(events.size());
        for (std::size_t index = 0; index < events.size(); ++index) {
            const auto &event = events[index];
            if (event.kind > PhysicsEventKind::TriggerExit || event.simulationTick != tick || !ValidStatePair(Pair(event), tick))
                return false;
            indexes[index] = index;
        }
        std::ranges::sort(indexes, [&](std::size_t one, std::size_t two) {
            if (events[one].pair != events[two].pair)
                return events[one].pair < events[two].pair;
            return one < two;
        });
        return true;
    }

    /** @copydoc PhysicsEventProjection::ValidStateEvents */
    bool PhysicsEventProjection::ValidStateEvents(std::span<const PhysicsEventRecord> events, std::uint64_t tick) noexcept {
        if (!PrepareStateEventOrder(events, tick))
            return false;
        const auto indexes = std::span{stateEventIndices_}.first(events.size());
        const PhysicsEventRecord *previous{};
        for (const auto index : indexes) {
            const auto &event = events[index];
            if (previous && event.pair == previous->pair && !Transition(previous->kind, event.kind))
                return false;
            previous = &event;
        }
        return true;
    }

    /** @copydoc PhysicsEventProjection::ReadState */
    PhysicsEventProjectionStateView PhysicsEventProjection::ReadState() const noexcept {
        return {publishedTick_, droppedDuringTick_, previousPairs_, PublishedEvents()};
    }

    /** @copydoc PhysicsEventProjection::PrepareState */
    Result<void> PhysicsEventProjection::PrepareState(const PhysicsEventProjectionStateView &state) {
        if (activeTick_ != 0 || state.tick == 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (state.pairs.size() > maximumInFlightPairs_ || state.events.size() > maximumEvents_)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        const PhysicsEventProjectionPairState *previous{};
        for (const auto &pair : state.pairs) {
            if (!ValidStatePair(pair, state.tick) || (previous && !(previous->pair < pair.pair)))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            previous = &pair;
        }
        if (!ValidStateEvents(state.events, state.tick))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        static_assert(std::is_nothrow_copy_assignable_v<PhysicsEventProjectionPairState>);
        static_assert(std::is_nothrow_copy_assignable_v<PhysicsEventRecord>);
        previousPairs_.assign(state.pairs.begin(), state.pairs.end());
        currentPairs_.clear();
        eventBuffers_[0].assign(state.events.begin(), state.events.end());
        eventBuffers_[1].clear();
        publishedBuffer_ = 0;
        publishedTick_ = state.tick;
        droppedDuringTick_ = state.dropped;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsEventProjection::SwapState */
    void PhysicsEventProjection::SwapState(PhysicsEventProjection &candidate) noexcept {
        if (maximumEvents_ != candidate.maximumEvents_ || maximumInFlightPairs_ != candidate.maximumInFlightPairs_ ||
            overflowPolicy_ != candidate.overflowPolicy_ || activeTick_ || candidate.activeTick_)
            std::abort();
        previousPairs_.swap(candidate.previousPairs_);
        eventBuffers_.swap(candidate.eventBuffers_);
        std::swap(publishedBuffer_, candidate.publishedBuffer_);
        std::swap(publishedTick_, candidate.publishedTick_);
        std::swap(droppedDuringTick_, candidate.droppedDuringTick_);
        currentPairs_.clear();
        callbackWrite_.store(0);
        callbackDropped_.store(0);
        callbackOverflowed_.store(false);
        overflowedDuringTick_ = surfaceConflict_ = false;
    }

    /** @copydoc PhysicsEventProjection::RetainedPayloadBytes */
    std::size_t PhysicsEventProjection::RetainedPayloadBytes() const noexcept {
        return sizeof(*this) + observations_.capacity() * sizeof(PhysicsContactObservation) +
               (previousPairs_.capacity() + currentPairs_.capacity()) * sizeof(PairState) +
               (eventBuffers_[0].capacity() + eventBuffers_[1].capacity()) * sizeof(PhysicsEventRecord) +
               stateEventIndices_.capacity() * sizeof(std::size_t);
    }
}  // namespace Horo::Physics::Detail
