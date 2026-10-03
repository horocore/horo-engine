#include "Horo/WorldStreaming/StreamingSchedulerAdmission.h"

#include "Horo/WorldStreaming/WorldStreamingErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        template <typename Entries> [[nodiscard]] auto FindReservation(Entries &entries, const StreamingSchedulerReservationId id) {
            return std::ranges::find_if(entries, [id](const auto &entry) {
                return entry.reservation.id == id;
            });
        }

        /** @brief Validates the immutable caller-owned portion of one admission request. */
        [[nodiscard]] bool IsAdmissionRequestValid(const StreamingCellOperation &operation,
                                                   const std::uint64_t requiredCapacityUnits) noexcept {
            return operation.Handle().IsValid() && operation.State() == StreamingCellOperationState::Queued && requiredCapacityUnits > 0;
        }
    }  // namespace

    /** @copydoc StreamingSchedulerAdmissionLimits::IsValid */
    bool StreamingSchedulerAdmissionLimits::IsValid() const noexcept {
        return concurrentOperations > 0 && concurrentOperations <= MaximumConcurrentOperations && capacityUnits > 0;
    }

    /** @copydoc StreamingSchedulerReservation::IsValid */
    bool StreamingSchedulerReservation::IsValid() const noexcept {
        return owner.IsValid() && id.IsValid() && operation.IsValid() && capacityUnits > 0;
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::Create */
    Result<StreamingSchedulerAdmissionLedger> StreamingSchedulerAdmissionLedger::Create(const StreamingSchedulerLedgerId owner,
                                                                                        const StreamingSchedulerAdmissionLimits limits) {
        if (!owner.IsValid() || !limits.IsValid())
            return Failure<StreamingSchedulerAdmissionLedger>(WorldStreamingErrors::SchedulerAdmissionInvalid);

        StreamingSchedulerAdmissionLedger ledger{owner, limits};
        try {
            ledger.entries_.reserve(limits.concurrentOperations);
        } catch (const std::bad_alloc &) {
            return Failure<StreamingSchedulerAdmissionLedger>(WorldStreamingErrors::SchedulerCapacityExceeded);
        } catch (const std::length_error &) {
            return Failure<StreamingSchedulerAdmissionLedger>(WorldStreamingErrors::SchedulerCapacityExceeded);
        }
        return Result<StreamingSchedulerAdmissionLedger>::Success(std::move(ledger));
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::TryAdmit */
    Result<StreamingSchedulerReservation> StreamingSchedulerAdmissionLedger::TryAdmit(const StreamingCellOperation &operation,
                                                                                      const std::uint64_t requiredCapacityUnits) {
        using enum StreamingSchedulerAdmissionState;
        if (state_ != Accepting)
            return Failure<StreamingSchedulerReservation>(WorldStreamingErrors::SchedulerLifecycleUnavailable);
        if (!IsAdmissionRequestValid(operation, requiredCapacityUnits))
            return Failure<StreamingSchedulerReservation>(WorldStreamingErrors::SchedulerAdmissionInvalid);
        if (entries_.size() >= limits_.concurrentOperations || requiredCapacityUnits > limits_.capacityUnits - reservedCapacityUnits_)
            return Failure<StreamingSchedulerReservation>(WorldStreamingErrors::SchedulerCapacityExceeded);
        if (std::ranges::any_of(entries_, [&operation](const Entry &entry) {
            return entry.operation.Handle() == operation.Handle();
        }))
            return Failure<StreamingSchedulerReservation>(WorldStreamingErrors::SchedulerReservationConflict);
        if (nextReservationValue_ == 0)
            return Failure<StreamingSchedulerReservation>(WorldStreamingErrors::GenerationExhausted);

        const auto admitted = operation.Advance(operation.Handle(), StreamingCellOperationTransition::Admit);
        if (admitted.HasError())
            return Result<StreamingSchedulerReservation>::Failure(admitted.ErrorValue());
        const StreamingSchedulerReservation reservation{
            .owner = owner_,
            .id = StreamingSchedulerReservationId::Create(nextReservationValue_).Value(),
            .operation = operation.Handle(),
            .capacityUnits = requiredCapacityUnits,
        };

        entries_.push_back({.reservation = reservation, .operation = admitted.Value()});
        reservedCapacityUnits_ += requiredCapacityUnits;
        nextReservationValue_ = nextReservationValue_ == std::numeric_limits<std::uint64_t>::max() ? 0 : nextReservationValue_ + 1;
        return Result<StreamingSchedulerReservation>::Success(reservation);
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::Advance */
    Result<StreamingCellOperation> StreamingSchedulerAdmissionLedger::Advance(const StreamingSchedulerReservation &reservation,
                                                                              const StreamingCellOperationTransition transition) {
        const auto index = FindExact(reservation);
        if (index.HasError())
            return Result<StreamingCellOperation>::Failure(index.ErrorValue());
        const auto found = entries_.begin() + static_cast<std::ptrdiff_t>(index.Value());

        const auto successor = found->operation.Advance(found->operation.Handle(), transition);
        if (successor.HasError())
            return Result<StreamingCellOperation>::Failure(successor.ErrorValue());
        found->operation = successor.Value();
        return Result<StreamingCellOperation>::Success(found->operation);
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::Release */
    Result<void> StreamingSchedulerAdmissionLedger::Release(const StreamingSchedulerReservation &reservation) {
        const auto index = FindExact(reservation);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        const auto found = entries_.begin() + static_cast<std::ptrdiff_t>(index.Value());
        if (!found->operation.IsTerminal())
            return Result<void>::Failure(MakeError(WorldStreamingErrors::SchedulerLifecycleUnavailable));

        reservedCapacityUnits_ -= found->reservation.capacityUnits;
        entries_.erase(found);
        using enum StreamingSchedulerAdmissionState;
        if (state_ == Draining && entries_.empty())
            state_ = Closed;
        return Result<void>::Success();
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::Inspect */
    Result<StreamingCellOperation> StreamingSchedulerAdmissionLedger::Inspect(const StreamingSchedulerReservation &reservation) const {
        const auto index = FindExact(reservation);
        if (index.HasError())
            return Result<StreamingCellOperation>::Failure(index.ErrorValue());
        return Result<StreamingCellOperation>::Success(entries_[index.Value()].operation);
    }

    Result<std::size_t> StreamingSchedulerAdmissionLedger::FindExact(const StreamingSchedulerReservation &reservation) const {
        if (!reservation.IsValid())
            return Failure<std::size_t>(WorldStreamingErrors::SchedulerAdmissionInvalid);
        const auto found = FindReservation(entries_, reservation.id);
        if (reservation.owner != owner_ || found == entries_.end() || found->reservation != reservation)
            return Failure<std::size_t>(WorldStreamingErrors::SchedulerReservationStale);
        return Result<std::size_t>::Success(static_cast<std::size_t>(found - entries_.begin()));
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::BeginShutdown */
    void StreamingSchedulerAdmissionLedger::BeginShutdown() noexcept {
        using enum StreamingSchedulerAdmissionState;
        if (state_ == Accepting)
            state_ = entries_.empty() ? Closed : Draining;
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::Owner */
    StreamingSchedulerLedgerId StreamingSchedulerAdmissionLedger::Owner() const noexcept {
        return owner_;
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::Limits */
    StreamingSchedulerAdmissionLimits StreamingSchedulerAdmissionLedger::Limits() const noexcept {
        return limits_;
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::State */
    StreamingSchedulerAdmissionState StreamingSchedulerAdmissionLedger::State() const noexcept {
        return state_;
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::ReservedCount */
    std::size_t StreamingSchedulerAdmissionLedger::ReservedCount() const noexcept {
        return entries_.size();
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::ReservedCapacityUnits */
    std::uint64_t StreamingSchedulerAdmissionLedger::ReservedCapacityUnits() const noexcept {
        return reservedCapacityUnits_;
    }

    StreamingSchedulerAdmissionLedger::StreamingSchedulerAdmissionLedger(const StreamingSchedulerLedgerId owner,
                                                                         const StreamingSchedulerAdmissionLimits limits) noexcept
        : owner_(owner), limits_(limits) {}
}  // namespace Horo::WorldStreaming
