#include "Horo/Destruction/DestructionEventDispatcher.h"

#include <algorithm>

namespace Horo::Destruction {
    using enum DestructionEventStatus;

    /** @copydoc DestructionEventDispatcher::ValidBindings */
    bool DestructionEventDispatcher::ValidBindings(const std::span<const DestructionEventBinding> bindings, const bool headless) noexcept {
        if (bindings.empty() || bindings.size() > MaximumBindings)
            return false;
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            const DestructionEventBinding &binding = bindings[index];
            if (binding.factKind > DestructionFactKind::AvailabilityChanged || binding.payloadSchema != DestructionFactSchema ||
                binding.requestSchema == 0 || binding.destination > DestructionDestinationKind::Accessibility ||
                binding.destinationId == 0 || (headless && binding.required && !binding.headlessEligible))
                return false;
            for (std::size_t previous = 0; previous < index; ++previous) {
                const DestructionEventBinding &other = bindings[previous];
                if (other.factKind == binding.factKind && other.destination == binding.destination &&
                    other.destinationId == binding.destinationId && other.layerOrdinal == binding.layerOrdinal)
                    return false;
            }
        }
        return true;
    }

    /** @copydoc DestructionEventDispatcher::Create */
    std::pair<DestructionEventStatus, DestructionEventDispatcher> DestructionEventDispatcher::Create(
        const DestructionWorldId world, const std::uint64_t generation, const std::span<const DestructionEventBinding> bindings,
        const bool headless) noexcept {
        if (!world.IsValid() || generation == 0 || !ValidBindings(bindings, headless))
            return {Invalid, {}};
        DestructionEventDispatcher dispatcher;
        dispatcher.world_ = world;
        dispatcher.generation_ = generation;
        dispatcher.owner_ = std::this_thread::get_id();
        dispatcher.cursor_ = {.world = world, .sequence = 0};
        std::ranges::copy(bindings, dispatcher.bindings_.begin());
        dispatcher.bindingCount_ = bindings.size();
        dispatcher.headless_ = headless;
        return {Ok, dispatcher};
    }

    /** @copydoc DestructionEventDispatcher::Cursor */
    DestructionEventCursor DestructionEventDispatcher::Cursor() const noexcept {
        return cursor_;
    }

    /** @copydoc DestructionEventDispatcher::FindAdapter */
    IDestructionDestinationAdapter *DestructionEventDispatcher::FindAdapter(
        const DestructionEventBinding &binding, const std::span<const DestructionAdapterSlot> adapters) noexcept {
        for (const DestructionAdapterSlot &slot : adapters) {
            if (slot.destination == binding.destination && slot.destinationId == binding.destinationId)
                return slot.adapter;
        }
        return nullptr;
    }

    /** @copydoc DestructionEventDispatcher::Active */
    bool DestructionEventDispatcher::Active(const DestructionEventBinding &binding) const noexcept {
        return !headless_ || binding.headlessEligible;
    }

    /** @copydoc DestructionEventDispatcher::ValidatePreflightInputs */
    DestructionEventStatus DestructionEventDispatcher::ValidatePreflightInputs(
        const std::span<const DestructionFact> facts, const std::span<const DestructionAdapterSlot> adapters) const noexcept {
        if (facts.empty() || facts.size() > DestructionHardLimits::EventsPerTransition || adapters.size() > MaximumBindings)
            return Invalid;
        for (std::size_t index = 0; index < adapters.size(); ++index) {
            const DestructionAdapterSlot &slot = adapters[index];
            if (slot.adapter == nullptr || slot.destination > DestructionDestinationKind::Accessibility || slot.destinationId == 0)
                return Invalid;
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (adapters[previous].destination == slot.destination && adapters[previous].destinationId == slot.destinationId)
                    return Invalid;
            }
        }
        for (const DestructionFact &fact : facts) {
            if (!fact.occurrence.IsValid() || fact.occurrence.source.world != world_ || fact.payloadSchema != DestructionFactSchema ||
                fact.transitionTicket == 0 || fact.transitionTicket != facts.front().transitionTicket ||
                fact.occurrence.source != facts.front().occurrence.source)
                return Invalid;
        }
        return Ok;
    }

    /** @copydoc DestructionEventDispatcher::RequiredRequestCount */
    std::uint32_t DestructionEventDispatcher::RequiredRequestCount(const DestructionEventBinding &binding,
                                                                   const std::span<const DestructionFact> facts) const noexcept {
        std::uint32_t count{};
        for (std::size_t layer = 0; layer < bindingCount_; ++layer) {
            const DestructionEventBinding &candidate = bindings_[layer];
            if (!candidate.required || candidate.destination != binding.destination || candidate.destinationId != binding.destinationId)
                continue;
            for (const DestructionFact &fact : facts) {
                if (fact.occurrence.kind == candidate.factKind)
                    ++count;
            }
        }
        return count;
    }

    /** @copydoc DestructionEventDispatcher::ValidateRequiredAdapters */
    DestructionEventStatus DestructionEventDispatcher::ValidateRequiredAdapters(
        const std::span<const DestructionAdapterSlot> adapters) const noexcept {
        for (std::size_t index = 0; index < bindingCount_; ++index) {
            const DestructionEventBinding &binding = bindings_[index];
            if (!binding.required || !Active(binding))
                continue;
            const IDestructionDestinationAdapter *adapter = FindAdapter(binding, adapters);
            if (adapter == nullptr || !adapter->Supports(binding.requestSchema))
                return ConsumerUnavailable;
        }
        return Ok;
    }

    /** @copydoc DestructionEventDispatcher::ReserveRequiredDestinations */
    DestructionEventStatus DestructionEventDispatcher::ReserveRequiredDestinations(
        const std::span<const DestructionFact> facts, const std::span<const DestructionAdapterSlot> adapters) const noexcept {
        for (std::size_t index = 0; index < bindingCount_; ++index) {
            const DestructionEventBinding &binding = bindings_[index];
            if (!binding.required || !Active(binding))
                continue;
            bool firstForDestination = true;
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (bindings_[previous].required && bindings_[previous].destination == binding.destination &&
                    bindings_[previous].destinationId == binding.destinationId)
                    firstForDestination = false;
            }
            if (!firstForDestination)
                continue;
            IDestructionDestinationAdapter *adapter = FindAdapter(binding, adapters);
            const std::uint32_t count = RequiredRequestCount(binding, facts);
            if (count != 0) {
                const DestructionEventStatus result = adapter->ReserveRequired(facts.front().transitionTicket, count);
                if (result != Ok) {
                    (void)CancelRequired(facts.front().transitionTicket, adapters);
                    return result;
                }
            }
        }
        return Ok;
    }

    /** @copydoc DestructionEventDispatcher::Preflight */
    DestructionEventStatus DestructionEventDispatcher::Preflight(const std::span<const DestructionFact> facts,
                                                                 const std::span<const DestructionAdapterSlot> adapters) const noexcept {
        if (owner_ != std::this_thread::get_id())
            return WrongThread;
        if (closing_)
            return ShutdownInProgress;
        if (const DestructionEventStatus valid = ValidatePreflightInputs(facts, adapters); valid != Ok)
            return valid;
        const DestructionEventStatus available = ValidateRequiredAdapters(adapters);
        return available == Ok ? ReserveRequiredDestinations(facts, adapters) : available;
    }

    /** @copydoc DestructionEventDispatcher::CancelRequired */
    DestructionEventStatus DestructionEventDispatcher::CancelRequired(
        const std::uint64_t transitionTicket, const std::span<const DestructionAdapterSlot> adapters) const noexcept {
        if (owner_ != std::this_thread::get_id())
            return WrongThread;
        if (transitionTicket == 0 || adapters.size() > MaximumBindings)
            return Invalid;
        for (std::size_t index = 0; index < bindingCount_; ++index) {
            const DestructionEventBinding &binding = bindings_[index];
            if (!binding.required || !Active(binding))
                continue;
            IDestructionDestinationAdapter *adapter = FindAdapter(binding, adapters);
            if (adapter != nullptr)
                adapter->CancelRequired(transitionTicket);
        }
        return Ok;
    }

    /** @copydoc DestructionEventDispatcher::CheckPumpSource */
    DestructionEventStatus DestructionEventDispatcher::CheckPumpSource(const DestructionEventStream &stream,
                                                                       const DestructionHandle currentSource) const noexcept {
        if (owner_ != std::this_thread::get_id() || !stream.OnOwnerThread())
            return WrongThread;
        if (closing_)
            return ShutdownInProgress;
        if (stream.Tail().world != world_ || !currentSource.IsValid() || currentSource.world != world_)
            return StaleGeneration;
        return Ok;
    }

    /** @copydoc DestructionEventDispatcher::DeliverLayer */
    DestructionEventStatus DestructionEventDispatcher::DeliverLayer(const DestructionEventBinding &binding, const DestructionFact &fact,
                                                                    const std::span<const DestructionAdapterSlot> adapters,
                                                                    DestructionDispatchResult &result) const noexcept {
        if (binding.factKind != fact.occurrence.kind)
            return Ok;
        if (!Active(binding)) {
            ++result.suppressed;
            return Ok;
        }
        IDestructionDestinationAdapter *adapter = FindAdapter(binding, adapters);
        if (adapter == nullptr || !adapter->Supports(binding.requestSchema)) {
            if (binding.required)
                return ConsumerUnavailable;
            ++result.suppressed;
            return Ok;
        }
        const DestructionDestinationRequest request{.id = {.occurrence = fact.occurrence,
                                                           .bindingGeneration = generation_,
                                                           .destinationId = binding.destinationId,
                                                           .layerOrdinal = binding.layerOrdinal},
                                                    .destination = binding.destination,
                                                    .requestSchema = binding.requestSchema,
                                                    .transitionTicket = fact.transitionTicket,
                                                    .committedTick = fact.committedTick,
                                                    .payload = fact.payload};
        const DestructionEventStatus outcome = adapter->Submit(request);
        if (outcome == Ok || outcome == AlreadyDispatched) {
            ++result.submitted;
            return Ok;
        }
        if (binding.required)
            return outcome;
        ++result.suppressed;
        return Ok;
    }

    /** @copydoc DestructionEventDispatcher::PumpOne */
    DestructionDispatchResult DestructionEventDispatcher::PumpOne(const DestructionEventStream &stream,
                                                                  const DestructionHandle currentSource,
                                                                  const std::span<const DestructionAdapterSlot> adapters) noexcept {
        DestructionDispatchResult result{.status = Invalid, .next = cursor_};
        result.status = CheckPumpSource(stream, currentSource);
        if (result.status != Ok)
            return result;
        const DestructionEventRead read = stream.Read(cursor_);
        if (read.status != Ok) {
            result.status = read.status;
            result.next = read.next;
            return result;
        }
        if (read.fact.occurrence.source != currentSource) {
            result.status = StaleGeneration;
            return result;
        }
        pending_ = true;
        for (; pendingBinding_ < bindingCount_; ++pendingBinding_) {
            result.status = DeliverLayer(bindings_[pendingBinding_], read.fact, adapters, result);
            if (result.status != Ok)
                return result;
        }
        pendingBinding_ = 0;
        pending_ = false;
        cursor_ = read.next;
        result.status = Ok;
        result.next = cursor_;
        return result;
    }

    /** @copydoc DestructionEventDispatcher::Reconcile */
    DestructionEventStatus DestructionEventDispatcher::Reconcile(const DestructionEventStream &stream,
                                                                 const DestructionEventCursor cursor) noexcept {
        if (owner_ != std::this_thread::get_id())
            return WrongThread;
        if (!stream.OnOwnerThread())
            return WrongThread;
        if (closing_)
            return ShutdownInProgress;
        if (cursor.world != world_ || stream.Tail().world != world_)
            return StaleGeneration;
        if (pending_ || cursor.sequence < cursor_.sequence || cursor.sequence < stream.Oldest().sequence ||
            cursor.sequence > stream.Tail().sequence)
            return Invalid;
        cursor_ = cursor;
        return Ok;
    }

    /** @copydoc DestructionEventDispatcher::ReplaceBindings */
    DestructionEventStatus DestructionEventDispatcher::ReplaceBindings(const std::uint64_t generation,
                                                                       const std::span<const DestructionEventBinding> bindings) noexcept {
        if (owner_ != std::this_thread::get_id())
            return WrongThread;
        if (closing_)
            return ShutdownInProgress;
        if (pending_)
            return ReservationStale;
        if (generation <= generation_)
            return StaleRevision;
        if (!ValidBindings(bindings, headless_))
            return Invalid;
        std::ranges::copy(bindings, bindings_.begin());
        bindingCount_ = bindings.size();
        generation_ = generation;
        return Ok;
    }

    /** @copydoc DestructionEventDispatcher::BeginShutdown */
    void DestructionEventDispatcher::BeginShutdown() noexcept {
        if (owner_ != std::this_thread::get_id())
            return;
        closing_ = true;
        pending_ = false;
        pendingBinding_ = 0;
    }
}  // namespace Horo::Destruction
