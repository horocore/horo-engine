#include "Horo/Destruction/DestructionEventDispatcher.h"

#include <algorithm>

namespace Horo::Destruction {
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
            return {DestructionEventStatus::Invalid, {}};
        DestructionEventDispatcher dispatcher;
        dispatcher.world_ = world;
        dispatcher.generation_ = generation;
        dispatcher.owner_ = std::this_thread::get_id();
        dispatcher.cursor_ = {.world = world, .sequence = 0};
        std::copy(bindings.begin(), bindings.end(), dispatcher.bindings_.begin());
        dispatcher.bindingCount_ = bindings.size();
        dispatcher.headless_ = headless;
        return {DestructionEventStatus::Ok, dispatcher};
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

    /** @copydoc DestructionEventDispatcher::Preflight */
    DestructionEventStatus DestructionEventDispatcher::Preflight(const std::span<const DestructionFact> facts,
                                                                 const std::span<const DestructionAdapterSlot> adapters) const noexcept {
        if (owner_ != std::this_thread::get_id())
            return DestructionEventStatus::WrongThread;
        if (closing_)
            return DestructionEventStatus::ShutdownInProgress;
        if (facts.empty() || facts.size() > DestructionHardLimits::EventsPerTransition || adapters.size() > MaximumBindings)
            return DestructionEventStatus::Invalid;
        for (std::size_t index = 0; index < adapters.size(); ++index) {
            const DestructionAdapterSlot &slot = adapters[index];
            if (slot.adapter == nullptr || slot.destination > DestructionDestinationKind::Accessibility || slot.destinationId == 0)
                return DestructionEventStatus::Invalid;
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (adapters[previous].destination == slot.destination && adapters[previous].destinationId == slot.destinationId)
                    return DestructionEventStatus::Invalid;
            }
        }
        for (const DestructionFact &fact : facts) {
            if (!fact.occurrence.IsValid() || fact.occurrence.source.world != world_ || fact.payloadSchema != DestructionFactSchema ||
                fact.transitionTicket == 0 || fact.transitionTicket != facts.front().transitionTicket ||
                fact.occurrence.source != facts.front().occurrence.source)
                return DestructionEventStatus::Invalid;
        }
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
            if (adapter == nullptr)
                return DestructionEventStatus::ConsumerUnavailable;
            if (!adapter->Supports(binding.requestSchema))
                return DestructionEventStatus::ConsumerUnavailable;
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
            if (count != 0) {
                const DestructionEventStatus result = adapter->ReserveRequired(facts.front().transitionTicket, count);
                if (result != DestructionEventStatus::Ok)
                    return result;
            }
        }
        return DestructionEventStatus::Ok;
    }

    /** @copydoc DestructionEventDispatcher::PumpOne */
    DestructionDispatchResult DestructionEventDispatcher::PumpOne(const DestructionEventStream &stream,
                                                                  const DestructionHandle currentSource,
                                                                  const std::span<const DestructionAdapterSlot> adapters) noexcept {
        DestructionDispatchResult result{.status = DestructionEventStatus::Invalid, .next = cursor_};
        if (owner_ != std::this_thread::get_id()) {
            result.status = DestructionEventStatus::WrongThread;
            return result;
        }
        if (closing_) {
            result.status = DestructionEventStatus::ShutdownInProgress;
            return result;
        }
        if (!stream.OnOwnerThread()) {
            result.status = DestructionEventStatus::WrongThread;
            return result;
        }
        if (stream.Tail().world != world_ || !currentSource.IsValid() || currentSource.world != world_) {
            result.status = DestructionEventStatus::StaleGeneration;
            return result;
        }
        const DestructionEventRead read = stream.Read(cursor_);
        if (read.status != DestructionEventStatus::Ok) {
            result.status = read.status;
            result.next = read.next;
            return result;
        }
        if (read.fact.occurrence.source != currentSource) {
            result.status = DestructionEventStatus::StaleGeneration;
            return result;
        }
        pending_ = true;
        for (; pendingBinding_ < bindingCount_; ++pendingBinding_) {
            const DestructionEventBinding &binding = bindings_[pendingBinding_];
            if (binding.factKind != read.fact.occurrence.kind || !Active(binding)) {
                if (binding.factKind == read.fact.occurrence.kind && !Active(binding))
                    ++result.suppressed;
                continue;
            }
            IDestructionDestinationAdapter *adapter = FindAdapter(binding, adapters);
            if (adapter == nullptr) {
                if (binding.required) {
                    result.status = DestructionEventStatus::ConsumerUnavailable;
                    return result;
                }
                ++result.suppressed;
                continue;
            }
            if (!adapter->Supports(binding.requestSchema)) {
                if (binding.required) {
                    result.status = DestructionEventStatus::ConsumerUnavailable;
                    return result;
                }
                ++result.suppressed;
                continue;
            }
            const DestructionDestinationRequest request{.id = {.occurrence = read.fact.occurrence,
                                                               .bindingGeneration = generation_,
                                                               .destinationId = binding.destinationId,
                                                               .layerOrdinal = binding.layerOrdinal},
                                                        .destination = binding.destination,
                                                        .requestSchema = binding.requestSchema,
                                                        .transitionTicket = read.fact.transitionTicket,
                                                        .committedTick = read.fact.committedTick,
                                                        .payload = read.fact.payload};
            const DestructionEventStatus outcome = adapter->Submit(request);
            if (outcome == DestructionEventStatus::Ok || outcome == DestructionEventStatus::AlreadyDispatched) {
                ++result.submitted;
                continue;
            }
            if (binding.required) {
                result.status = outcome;
                return result;
            }
            ++result.suppressed;
        }
        pendingBinding_ = 0;
        pending_ = false;
        cursor_ = read.next;
        result.status = DestructionEventStatus::Ok;
        result.next = cursor_;
        return result;
    }

    /** @copydoc DestructionEventDispatcher::Reconcile */
    DestructionEventStatus DestructionEventDispatcher::Reconcile(const DestructionEventStream &stream,
                                                                 const DestructionEventCursor cursor) noexcept {
        if (owner_ != std::this_thread::get_id())
            return DestructionEventStatus::WrongThread;
        if (!stream.OnOwnerThread())
            return DestructionEventStatus::WrongThread;
        if (closing_)
            return DestructionEventStatus::ShutdownInProgress;
        if (cursor.world != world_ || stream.Tail().world != world_)
            return DestructionEventStatus::StaleGeneration;
        if (pending_ || cursor.sequence < cursor_.sequence || cursor.sequence < stream.Oldest().sequence ||
            cursor.sequence > stream.Tail().sequence)
            return DestructionEventStatus::Invalid;
        cursor_ = cursor;
        return DestructionEventStatus::Ok;
    }

    /** @copydoc DestructionEventDispatcher::ReplaceBindings */
    DestructionEventStatus DestructionEventDispatcher::ReplaceBindings(const std::uint64_t generation,
                                                                       const std::span<const DestructionEventBinding> bindings) noexcept {
        if (owner_ != std::this_thread::get_id())
            return DestructionEventStatus::WrongThread;
        if (closing_)
            return DestructionEventStatus::ShutdownInProgress;
        if (pending_)
            return DestructionEventStatus::ReservationStale;
        if (generation <= generation_)
            return DestructionEventStatus::StaleRevision;
        if (!ValidBindings(bindings, headless_))
            return DestructionEventStatus::Invalid;
        std::copy(bindings.begin(), bindings.end(), bindings_.begin());
        bindingCount_ = bindings.size();
        generation_ = generation;
        return DestructionEventStatus::Ok;
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
