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

    /** @copydoc StreamingConcurrencyPolicy::IsValid */
    bool StreamingConcurrencyPolicy::IsValid() const noexcept {
        constexpr auto maximum = StreamingSchedulerAdmissionLimits::MaximumConcurrentOperations;
        return profile < WorldPartitionProjectProfile::Count && revision.IsValid() && loads <= maximum && activations <= maximum &&
               retirements <= maximum && (loads > 0 || activations > 0 || retirements > 0);
    }

    /** @copydoc StreamingConcurrencyPolicy::Limit */
    Result<std::uint32_t> StreamingConcurrencyPolicy::Limit(const StreamingCellOperationKind kind) const {
        using enum StreamingCellOperationKind;
        switch (kind) {
            case Load:
                return Result<std::uint32_t>::Success(loads);
            case Activate:
                return Result<std::uint32_t>::Success(activations);
            case Retire:
                return Result<std::uint32_t>::Success(retirements);
        }
        return Failure<std::uint32_t>(WorldStreamingErrors::SchedulerConcurrencyUnsupported);
    }

    /** @copydoc StreamingSchedulerAdmissionLimits::IsValid */
    bool StreamingSchedulerAdmissionLimits::IsValid() const noexcept {
        return concurrentOperations > 0 && concurrentOperations <= MaximumConcurrentOperations && capacityUnits > 0 &&
               concurrency.IsValid();
    }

    /** @copydoc StreamingSchedulerReservation::IsValid */
    bool StreamingSchedulerReservation::IsValid() const noexcept {
        return owner.IsValid() && id.IsValid() && operation.IsValid() && capacityUnits > 0;
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::Create */
    Result<StreamingSchedulerAdmissionLedger> StreamingSchedulerAdmissionLedger::Create(const StreamingSchedulerLedgerId owner,
                                                                                        const StreamingSchedulerAdmissionLimits &limits) {
        if (!owner.IsValid() || !limits.concurrency.revision.IsValid())
            return Failure<StreamingSchedulerAdmissionLedger>(WorldStreamingErrors::SchedulerAdmissionInvalid);
        if (limits.concurrency.profile >= WorldPartitionProjectProfile::Count)
            return Failure<StreamingSchedulerAdmissionLedger>(WorldStreamingErrors::SchedulerConcurrencyUnsupported);
        if (!limits.IsValid())
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
                                                                                      const std::uint64_t requiredCapacityUnits,
                                                                                      const StreamingConcurrencyRevision expectedRevision) {
        using enum StreamingSchedulerAdmissionState;
        if (state_ != Accepting)
            return Failure<StreamingSchedulerReservation>(WorldStreamingErrors::SchedulerLifecycleUnavailable);
        if (!expectedRevision.IsValid() || !IsAdmissionRequestValid(operation, requiredCapacityUnits))
            return Failure<StreamingSchedulerReservation>(WorldStreamingErrors::SchedulerAdmissionInvalid);
        if (expectedRevision != limits_.concurrency.revision)
            return Failure<StreamingSchedulerReservation>(WorldStreamingErrors::SchedulerConcurrencyStale);
        const auto stageLimit = limits_.concurrency.Limit(operation.Kind());
        if (stageLimit.HasError())
            return Result<StreamingSchedulerReservation>::Failure(stageLimit.ErrorValue());
        if (stageLimit.Value() == 0)
            return Failure<StreamingSchedulerReservation>(WorldStreamingErrors::SchedulerConcurrencyUnsupported);
        if (ReservedCount(operation.Kind()).Value() >= stageLimit.Value())
            return Failure<StreamingSchedulerReservation>(WorldStreamingErrors::SchedulerCapacityExceeded);
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

    /** @copydoc StreamingSchedulerAdmissionLedger::ReplaceConcurrency */
    Result<void> StreamingSchedulerAdmissionLedger::ReplaceConcurrency(const StreamingConcurrencyRevision expectedRevision,
                                                                       const StreamingConcurrencyPolicy &policy) {
        if (state_ != StreamingSchedulerAdmissionState::Accepting)
            return Failure<void>(WorldStreamingErrors::SchedulerLifecycleUnavailable);
        if (policy.profile >= WorldPartitionProjectProfile::Count)
            return Failure<void>(WorldStreamingErrors::SchedulerConcurrencyUnsupported);
        if (!expectedRevision.IsValid() || !policy.IsValid())
            return Failure<void>(WorldStreamingErrors::SchedulerAdmissionInvalid);
        if (expectedRevision != limits_.concurrency.revision || policy.profile != limits_.concurrency.profile ||
            policy.revision.Value() <= limits_.concurrency.revision.Value())
            return Failure<void>(WorldStreamingErrors::SchedulerConcurrencyStale);
        limits_.concurrency = policy;
        return Result<void>::Success();
    }

    /** @copydoc StreamingSchedulerAdmissionLedger::ReservedCount */
    Result<std::size_t> StreamingSchedulerAdmissionLedger::ReservedCount(const StreamingCellOperationKind kind) const {
        if (const auto limit = limits_.concurrency.Limit(kind); limit.HasError())
            return Result<std::size_t>::Failure(limit.ErrorValue());
        return Result<std::size_t>::Success(static_cast<std::size_t>(std::ranges::count_if(entries_, [kind](const Entry &entry) {
            return entry.operation.Kind() == kind;
        })));
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

    /** @copydoc StreamingSchedulerAdmissionLedger::StreamingSchedulerAdmissionLedger(StreamingSchedulerAdmissionLedger&&) */
    StreamingSchedulerAdmissionLedger::StreamingSchedulerAdmissionLedger(StreamingSchedulerAdmissionLedger &&other) noexcept
        : owner_(other.owner_), limits_(other.limits_), state_(other.state_),
          reservedCapacityUnits_(std::exchange(other.reservedCapacityUnits_, 0)), nextReservationValue_(other.nextReservationValue_),
          entries_(std::move(other.entries_)) {
        other.entries_.clear();
        other.state_ = StreamingSchedulerAdmissionState::Closed;
    }

    StreamingSchedulerAdmissionLedger::StreamingSchedulerAdmissionLedger(const StreamingSchedulerLedgerId owner,
                                                                         const StreamingSchedulerAdmissionLimits &limits) noexcept
        : owner_(owner), limits_(limits) {}
}  // namespace Horo::WorldStreaming
