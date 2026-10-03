#include "Horo/Destruction/DestructionEventStream.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Destruction {
    using enum DestructionEventStatus;

    namespace {
        /** @brief Checks every present vector component without accepting NaN or infinity. */
        [[nodiscard]] bool FiniteVector(const std::array<float, 3> &value) noexcept {
            return std::ranges::all_of(value, [](const float component) {
                return std::isfinite(component);
            });
        }

        /** @brief Rejects malformed payloads without interpreting absent fields as a source of meaning. */
        [[nodiscard]] bool ValidPayload(const DestructionFactPayload &payload) noexcept {
            return std::isfinite(payload.strength) && payload.strength >= 0.0F && FiniteVector(payload.point) &&
                   FiniteVector(payload.normal) && (payload.hasPoint || payload.point == std::array<float, 3>{}) &&
                   (payload.hasNormal || payload.normal == std::array<float, 3>{});
        }
    }  // namespace

    DestructionEventStream::DestructionEventStream(DestructionWorldId world, const std::uint32_t capacity, const std::uint32_t maximumBatch,
                                                   std::vector<DestructionFact> storage) noexcept
        : world_(world), capacity_(capacity), maximumBatch_(maximumBatch), storage_(std::move(storage)),
          owner_(std::this_thread::get_id()) {}

    /** @copydoc DestructionEventStream::Create */
    std::pair<DestructionEventStatus, std::unique_ptr<DestructionEventStream>> DestructionEventStream::Create(
        const DestructionWorldId world, const std::uint32_t capacity, const std::uint32_t maximumBatch) {
        if (!world.IsValid() || capacity == 0 || capacity > DestructionHardLimits::EventJournalEntries || maximumBatch == 0 ||
            maximumBatch > capacity || maximumBatch > DestructionHardLimits::EventsPerTransition)
            return {Invalid, nullptr};
        std::vector<DestructionFact> storage;
        try {
            storage.resize(capacity);
        } catch (const std::bad_alloc &) {
            return {CapacityExceeded, nullptr};
        }
        auto stream = std::unique_ptr<DestructionEventStream>(
            new (std::nothrow) DestructionEventStream(world, capacity, maximumBatch, std::move(storage)));
        if (!stream)
            return {CapacityExceeded, nullptr};
        return {Ok, std::move(stream)};
    }

    /** @copydoc DestructionEventStream::Tail */
    DestructionEventCursor DestructionEventStream::Tail() const noexcept {
        return {.world = world_, .sequence = tail_};
    }

    /** @copydoc DestructionEventStream::Oldest */
    DestructionEventCursor DestructionEventStream::Oldest() const noexcept {
        return {.world = world_, .sequence = oldest_};
    }

    /** @copydoc DestructionEventStream::OnOwnerThread */
    bool DestructionEventStream::OnOwnerThread() const noexcept {
        return owner_ == std::this_thread::get_id();
    }

    /** @copydoc DestructionEventStream::Reserve */
    std::pair<DestructionEventStatus, DestructionEventReservation> DestructionEventStream::Reserve(
        const DestructionHandle source, const DestructionStateRevision sourceRevision, const std::uint64_t transitionTicket,
        const std::span<const DestructionFact> facts, const DestructionEventCursor requiredCursor) const noexcept {
        const auto count = static_cast<std::uint32_t>(facts.size());
        if (!OnOwnerThread())
            return {WrongThread, DestructionEventReservation{}};
        if (closing_)
            return {ShutdownInProgress, DestructionEventReservation{}};
        if (!source.IsValid() || !sourceRevision.IsValid() || source.world != world_ || requiredCursor.world != world_ ||
            transitionTicket == 0 || count == 0 || facts.size() > maximumBatch_ || requiredCursor.sequence > tail_)
            return {Invalid, DestructionEventReservation{}};
        if (requiredCursor.sequence < oldest_)
            return {Gap, DestructionEventReservation{}};
        if (tail_ > std::numeric_limits<std::uint64_t>::max() - count)
            return {CapacityExceeded, DestructionEventReservation{}};
        if (tail_ + count - requiredCursor.sequence > capacity_)
            return {RequiredConsumerStalled, DestructionEventReservation{}};
        if (const DestructionEventStatus valid = ValidateBatch(source, sourceRevision, transitionTicket, facts); valid != Ok)
            return {valid, DestructionEventReservation{}};
        std::vector<DestructionFact> copy;
        try {
            copy.assign(facts.begin(), facts.end());
        } catch (const std::bad_alloc &) {
            return {CapacityExceeded, DestructionEventReservation{}};
        }
        DestructionEventReservation reservation;
        reservation.source_ = source;
        reservation.nextSequence_ = tail_;
        reservation.count_ = count;
        reservation.facts_ = std::move(copy);
        return {Ok, std::move(reservation)};
    }

    /** @copydoc DestructionEventStream::ValidateBatch */
    DestructionEventStatus DestructionEventStream::ValidateBatch(const DestructionHandle source,
                                                                 const DestructionStateRevision sourceRevision,
                                                                 const std::uint64_t transitionTicket,
                                                                 const std::span<const DestructionFact> facts) const noexcept {
        if (facts.empty() || facts.size() > maximumBatch_ || transitionTicket == 0)
            return Invalid;
        if (const auto successor = AdvanceDestructionStateRevision(sourceRevision);
            successor.HasError() || facts.front().occurrence.stateRevision != successor.Value())
            return StaleRevision;
        std::uint64_t priorTick = lastTick_;
        DestructionStateRevision priorRevision{};
        for (std::size_t index = 0; index < facts.size(); ++index) {
            const DestructionFact &fact = facts[index];
            if (!fact.occurrence.IsValid() || fact.occurrence.source != source || fact.transitionTicket != transitionTicket ||
                fact.committedTick == 0 || fact.committedTick < priorTick || fact.payloadSchema != DestructionFactSchema ||
                !ValidPayload(fact.payload))
                return Invalid;
            if (index == 0) {
                if (fact.occurrence.revisionOrdinal != 0)
                    return Invalid;
            } else if (fact.occurrence.stateRevision != priorRevision || fact.committedTick != priorTick ||
                       fact.occurrence.revisionOrdinal != index)
                return Invalid;
            priorTick = fact.committedTick;
            priorRevision = fact.occurrence.stateRevision;
        }
        return Ok;
    }

    /** @copydoc DestructionEventStream::Publish */
    DestructionEventStatus DestructionEventStream::Publish(DestructionEventReservation &&reservation, const DestructionHandle currentSource,
                                                           const DestructionStateRevision committedRevision) noexcept {
        if (!OnOwnerThread())
            return WrongThread;
        if (closing_)
            return ShutdownInProgress;
        if (reservation.source_.world != world_ || currentSource.world != world_)
            return StaleGeneration;
        if (currentSource != reservation.source_)
            return StaleGeneration;
        if (reservation.nextSequence_ != tail_)
            return ReservationStale;
        if (reservation.facts_.empty() || reservation.count_ == 0 || reservation.count_ > maximumBatch_ ||
            reservation.count_ != reservation.facts_.size())
            return Invalid;
        if (reservation.facts_[0].occurrence.stateRevision != committedRevision)
            return StaleRevision;
        DestructionEventReservation committed = std::move(reservation);
        for (const DestructionFact &fact : committed.facts_) {
            storage_[tail_ % capacity_] = fact;
            ++tail_;
        }
        if (tail_ - oldest_ > capacity_)
            oldest_ = tail_ - capacity_;
        lastTick_ = committed.facts_.back().committedTick;
        return Ok;
    }

    /** @copydoc DestructionEventStream::Read */
    DestructionEventRead DestructionEventStream::Read(const DestructionEventCursor cursor) const noexcept {
        if (!OnOwnerThread())
            return {.status = WrongThread, .next = Oldest()};
        if (cursor.world != world_)
            return {.status = StaleGeneration, .next = Oldest()};
        if (cursor.sequence < oldest_)
            return {.status = Gap, .next = Oldest()};
        if (cursor.sequence > tail_)
            return {.status = Invalid, .next = Tail()};
        if (cursor.sequence == tail_)
            return {.status = Empty, .next = Tail()};
        return {.status = Ok, .next = {.world = world_, .sequence = cursor.sequence + 1}, .fact = storage_[cursor.sequence % capacity_]};
    }

    /** @copydoc DestructionEventStream::BeginShutdown */
    void DestructionEventStream::BeginShutdown() noexcept {
        if (OnOwnerThread())
            closing_ = true;
    }
}  // namespace Horo::Destruction
