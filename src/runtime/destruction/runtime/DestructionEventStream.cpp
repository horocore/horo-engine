#include "Horo/Destruction/DestructionEventStream.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Destruction {
    namespace {
        /** @brief Checks every present vector component without accepting NaN or infinity. */
        [[nodiscard]] bool FiniteVector(const std::array<float, 3> &value) noexcept {
            return std::all_of(value.begin(), value.end(), [](const float component) {
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
                                                   std::unique_ptr<DestructionFact[]> storage) noexcept
        : world_(world), capacity_(capacity), maximumBatch_(maximumBatch), storage_(std::move(storage)),
          owner_(std::this_thread::get_id()) {}

    /** @copydoc DestructionEventStream::Create */
    std::pair<DestructionEventStatus, std::unique_ptr<DestructionEventStream>> DestructionEventStream::Create(
        const DestructionWorldId world, const std::uint32_t capacity, const std::uint32_t maximumBatch) {
        if (!world.IsValid() || capacity == 0 || capacity > DestructionHardLimits::EventJournalEntries || maximumBatch == 0 ||
            maximumBatch > capacity || maximumBatch > DestructionHardLimits::EventsPerTransition)
            return {DestructionEventStatus::Invalid, nullptr};
        auto storage = std::unique_ptr<DestructionFact[]>(new (std::nothrow) DestructionFact[capacity]);
        if (!storage)
            return {DestructionEventStatus::CapacityExceeded, nullptr};
        auto stream = std::unique_ptr<DestructionEventStream>(
            new (std::nothrow) DestructionEventStream(world, capacity, maximumBatch, std::move(storage)));
        if (!stream)
            return {DestructionEventStatus::CapacityExceeded, nullptr};
        return {DestructionEventStatus::Ok, std::move(stream)};
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
        const DestructionHandle source, const std::uint64_t transitionTicket, const std::span<const DestructionFact> facts,
        const DestructionEventCursor requiredCursor) const noexcept {
        const auto count = static_cast<std::uint32_t>(facts.size());
        if (!OnOwnerThread())
            return {DestructionEventStatus::WrongThread, DestructionEventReservation{}};
        if (closing_)
            return {DestructionEventStatus::ShutdownInProgress, DestructionEventReservation{}};
        if (!source.IsValid() || source.world != world_ || requiredCursor.world != world_ || transitionTicket == 0 || count == 0 ||
            facts.size() > maximumBatch_ || requiredCursor.sequence > tail_)
            return {DestructionEventStatus::Invalid, DestructionEventReservation{}};
        if (requiredCursor.sequence < oldest_)
            return {DestructionEventStatus::Gap, DestructionEventReservation{}};
        if (tail_ > std::numeric_limits<std::uint64_t>::max() - count)
            return {DestructionEventStatus::CapacityExceeded, DestructionEventReservation{}};
        if (tail_ + count - requiredCursor.sequence > capacity_)
            return {DestructionEventStatus::RequiredConsumerStalled, DestructionEventReservation{}};
        const DestructionEventStatus valid = ValidateBatch(source, transitionTicket, facts);
        if (valid != DestructionEventStatus::Ok)
            return {valid, DestructionEventReservation{}};
        auto copy = std::unique_ptr<DestructionFact[]>(new (std::nothrow) DestructionFact[count]);
        if (!copy)
            return {DestructionEventStatus::CapacityExceeded, DestructionEventReservation{}};
        std::copy(facts.begin(), facts.end(), copy.get());
        DestructionEventReservation reservation;
        reservation.source_ = source;
        reservation.nextSequence_ = tail_;
        reservation.count_ = count;
        reservation.facts_ = std::move(copy);
        return {DestructionEventStatus::Ok, std::move(reservation)};
    }

    /** @copydoc DestructionEventStream::ValidateBatch */
    DestructionEventStatus DestructionEventStream::ValidateBatch(const DestructionHandle source, const std::uint64_t transitionTicket,
                                                                 const std::span<const DestructionFact> facts) const noexcept {
        if (facts.empty() || facts.size() > maximumBatch_ || transitionTicket == 0)
            return DestructionEventStatus::Invalid;
        if (tail_ > oldest_) {
            const DestructionFact &previous = storage_[(tail_ - 1) % capacity_];
            if (previous.occurrence.source == source && facts.front().occurrence.stateRevision <= previous.occurrence.stateRevision)
                return DestructionEventStatus::StaleRevision;
        }
        std::uint64_t priorTick = lastTick_;
        DestructionStateRevision priorRevision{};
        for (std::size_t index = 0; index < facts.size(); ++index) {
            const DestructionFact &fact = facts[index];
            if (!fact.occurrence.IsValid() || fact.occurrence.source != source || fact.transitionTicket != transitionTicket ||
                fact.committedTick == 0 || fact.committedTick < priorTick || fact.payloadSchema != DestructionFactSchema ||
                !ValidPayload(fact.payload))
                return DestructionEventStatus::Invalid;
            if (index == 0) {
                if (fact.occurrence.revisionOrdinal != 0)
                    return DestructionEventStatus::Invalid;
            } else if (fact.occurrence.stateRevision != priorRevision || fact.committedTick != priorTick ||
                       fact.occurrence.revisionOrdinal != index)
                return DestructionEventStatus::Invalid;
            priorTick = fact.committedTick;
            priorRevision = fact.occurrence.stateRevision;
        }
        return DestructionEventStatus::Ok;
    }

    /** @copydoc DestructionEventStream::Publish */
    DestructionEventStatus DestructionEventStream::Publish(DestructionEventReservation &&reservation,
                                                           const DestructionHandle currentSource) noexcept {
        if (!OnOwnerThread())
            return DestructionEventStatus::WrongThread;
        if (closing_)
            return DestructionEventStatus::ShutdownInProgress;
        if (reservation.source_.world != world_ || currentSource.world != world_)
            return DestructionEventStatus::StaleGeneration;
        if (currentSource != reservation.source_)
            return DestructionEventStatus::StaleGeneration;
        if (reservation.nextSequence_ != tail_)
            return DestructionEventStatus::ReservationStale;
        if (!reservation.facts_ || reservation.count_ == 0 || reservation.count_ > maximumBatch_)
            return DestructionEventStatus::Invalid;
        for (std::uint32_t index = 0; index < reservation.count_; ++index) {
            storage_[tail_ % capacity_] = reservation.facts_[index];
            ++tail_;
        }
        if (tail_ - oldest_ > capacity_)
            oldest_ = tail_ - capacity_;
        lastTick_ = reservation.facts_[reservation.count_ - 1].committedTick;
        reservation = {};
        return DestructionEventStatus::Ok;
    }

    /** @copydoc DestructionEventStream::Read */
    DestructionEventRead DestructionEventStream::Read(const DestructionEventCursor cursor) const noexcept {
        if (!OnOwnerThread())
            return {.status = DestructionEventStatus::WrongThread, .next = Oldest()};
        if (cursor.world != world_)
            return {.status = DestructionEventStatus::StaleGeneration, .next = Oldest()};
        if (cursor.sequence < oldest_)
            return {.status = DestructionEventStatus::Gap, .next = Oldest()};
        if (cursor.sequence > tail_)
            return {.status = DestructionEventStatus::Invalid, .next = Tail()};
        if (cursor.sequence == tail_)
            return {.status = DestructionEventStatus::Empty, .next = Tail()};
        return {.status = DestructionEventStatus::Ok,
                .next = {.world = world_, .sequence = cursor.sequence + 1},
                .fact = storage_[cursor.sequence % capacity_]};
    }

    /** @copydoc DestructionEventStream::BeginShutdown */
    void DestructionEventStream::BeginShutdown() noexcept {
        if (OnOwnerThread())
            closing_ = true;
    }
}  // namespace Horo::Destruction
