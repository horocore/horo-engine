#include "Horo/Network/DeterministicTransport.h"

#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkMetrics.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Network {
    namespace {
        [[nodiscard]] bool ValidRate(const std::uint16_t value) noexcept {
            return value <= 10'000;
        }

        template <typename T> [[nodiscard]] Result<T> Fail(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool ValidScenario(const DeterministicTransportDescriptor &descriptor) noexcept {
            const auto &scenario = descriptor.scenario;
            return scenario.contractVersion == 1 && scenario.revision != 0 && scenario.seed != 0 && scenario.maximumFragmentBytes != 0 &&
                   scenario.maximumFragmentBytes <= descriptor.maximumPayloadBytes && ValidRate(scenario.lossPerTenThousand) &&
                   ValidRate(scenario.duplicatePerTenThousand) && ValidRate(scenario.reorderPerTenThousand) &&
                   scenario.latencyTicks <= std::numeric_limits<std::uint32_t>::max() - scenario.jitterTicks;
        }

        [[nodiscard]] bool ValidCapacity(const DeterministicTransportDescriptor &descriptor) noexcept {
            return descriptor.maximumScheduledDeliveries != 0 && descriptor.maximumPayloadBytes != 0 && descriptor.maximumChannels != 0 &&
                   descriptor.maximumScheduledDeliveries <= std::numeric_limits<std::uint32_t>::max() &&
                   descriptor.budgetCapacity.maximumConnections <=
                       std::numeric_limits<std::size_t>::max() - descriptor.maximumScheduledDeliveries &&
                   descriptor.maximumScheduledDeliveries + descriptor.budgetCapacity.maximumConnections <=
                       std::numeric_limits<std::size_t>::max() / descriptor.scenario.maximumFragmentBytes;
        }

        [[nodiscard]] bool ValidDescriptor(const DeterministicTransportDescriptor &descriptor) noexcept {
            return descriptor.mode < DeterministicTransportMode::Count && ValidScenario(descriptor) && ValidCapacity(descriptor);
        }

        [[nodiscard]] DeterministicSendOutcome MapAdmission(const TransportBudgetAdmission admission) noexcept {
            using enum TransportBudgetAdmission;
            switch (admission) {
                case Replaced:
                    return DeterministicSendOutcome::Replaced;
                case DroppedReplaceable:
                    return DeterministicSendOutcome::DroppedReplaceable;
                case ConnectionMustClose:
                    return DeterministicSendOutcome::ConnectionMustClose;
                case Enqueued:
                case Count:
                    return DeterministicSendOutcome::Count;
            }
            return DeterministicSendOutcome::Count;
        }
    }  // namespace

    DeterministicTransport::DeterministicTransport(DeterministicTransportDescriptor descriptor, TransportBudgetController budget,
                                                   std::unique_ptr<ScheduledDelivery[]> deliveries,
                                                   std::unique_ptr<std::byte[]> payloadStorage, NetworkMetrics *metrics) noexcept
        : descriptor_(std::move(descriptor)), budget_(std::move(budget)), deliveries_(std::move(deliveries)),
          payloadStorage_(std::move(payloadStorage)), randomState_(descriptor_.scenario.seed), metrics_(metrics) {}

    /** @copydoc DeterministicTransport::Create */
    Result<DeterministicTransport> DeterministicTransport::Create(const DeterministicTransportDescriptor &descriptor,
                                                                  NetworkMetrics *metrics) {
        if (!ValidDescriptor(descriptor))
            return Fail<DeterministicTransport>(NetworkErrors::TransportBudgetInvalid);
        auto budget = TransportBudgetController::Create(descriptor.budgetCapacity, descriptor.budgetPolicy);
        if (budget.HasError())
            return Result<DeterministicTransport>::Failure(budget.ErrorValue());
        try {
            const auto deliveryCapacity = descriptor.maximumScheduledDeliveries + descriptor.budgetCapacity.maximumConnections;
            auto deliveries = std::make_unique<ScheduledDelivery[]>(deliveryCapacity);
            auto payloadStorage = std::make_unique<std::byte[]>(deliveryCapacity * descriptor.scenario.maximumFragmentBytes);
            return Result<DeterministicTransport>::Success(
                DeterministicTransport{descriptor, std::move(budget).Value(), std::move(deliveries), std::move(payloadStorage), metrics});
        } catch (const std::bad_alloc &) {
            return Fail<DeterministicTransport>(NetworkErrors::TransportBudgetCapacityExceeded);
        }
    }

    /** @copydoc DeterministicTransport::Open */
    Result<void> DeterministicTransport::Open(const ConnectionHandle connection, const TransportAdmissionState state) {
        if (descriptor_.mode == DeterministicTransportMode::RejectAll)
            return Fail<void>(NetworkErrors::TransportCapabilityUnavailable);
        auto opened = budget_.OpenConnection(connection, shuttingDown_ ? TransportAdmissionState::ShuttingDown : state);
        if (opened.HasValue())
            deliveries_[descriptor_.maximumScheduledDeliveries + connection.Slot()] = {};
        return opened;
    }

    std::uint64_t DeterministicTransport::NextRandom() noexcept {
        randomState_ ^= randomState_ >> 12U;
        randomState_ ^= randomState_ << 25U;
        randomState_ ^= randomState_ >> 27U;
        return randomState_ * 2'685'821'657'736'338'717ULL;
    }

    bool DeterministicTransport::Draw(const std::uint16_t rate) noexcept {
        return rate != 0 && NextRandom() % 10'000U < rate;
    }

    std::uint64_t DeterministicTransport::DeliveryTick() noexcept {
        const auto &scenario = descriptor_.scenario;
        std::uint64_t delay = scenario.latencyTicks;
        if (scenario.jitterTicks != 0) {
            const auto width = static_cast<std::uint64_t>(scenario.jitterTicks) * 2U + 1U;
            const auto sample = NextRandom() % width;
            if (sample < scenario.jitterTicks)
                delay -= std::min(delay, static_cast<std::uint64_t>(scenario.jitterTicks) - sample);
            else
                delay += sample - scenario.jitterTicks;
        }
        if (Draw(scenario.reorderPerTenThousand))
            ++delay;
        return tick_ > std::numeric_limits<std::uint64_t>::max() - delay ? std::numeric_limits<std::uint64_t>::max() : tick_ + delay;
    }

    std::size_t DeterministicTransport::DeliveryCapacity() const noexcept {
        return descriptor_.maximumScheduledDeliveries + descriptor_.budgetCapacity.maximumConnections;
    }

    std::size_t DeterministicTransport::FreeDeliveries() const noexcept {
        std::size_t count{};
        for (std::size_t index = 0; index < descriptor_.maximumScheduledDeliveries; ++index)
            count += deliveries_[index].occupied ? 0U : 1U;
        return count;
    }

    std::size_t DeterministicTransport::MatchingDeliveries(const ConnectionHandle connection,
                                                           const std::uint64_t replaceableKey) const noexcept {
        std::size_t count{};
        for (std::size_t index = 0; index < descriptor_.maximumScheduledDeliveries; ++index) {
            const auto &delivery = deliveries_[index];
            count += delivery.occupied && delivery.connection == connection && delivery.replaceableKey == replaceableKey ? 1U : 0U;
        }
        return count;
    }

    void DeterministicTransport::DiscardMatching(const ConnectionHandle connection, const std::uint64_t replaceableKey) noexcept {
        for (std::size_t index = 0; index < descriptor_.maximumScheduledDeliveries; ++index) {
            auto &delivery = deliveries_[index];
            if (delivery.occupied && delivery.connection == connection && delivery.replaceableKey == replaceableKey)
                delivery.occupied = false;
        }
    }

    void DeterministicTransport::DiscardTicket(const TransportQueueTicket ticket) noexcept {
        for (std::size_t index = 0; index < descriptor_.maximumScheduledDeliveries; ++index) {
            if (deliveries_[index].occupied && deliveries_[index].ticket == ticket)
                deliveries_[index].occupied = false;
        }
    }

    Result<void> DeterministicTransport::ScheduleCopy(const ConnectionHandle connection, const ChannelId channel,
                                                      const TransportQueueTicket ticket, const std::uint64_t replaceableKey,
                                                      const std::span<const std::byte> payload, const std::size_t fragmentCount) {
        std::size_t sourceOffset{};
        for (std::size_t fragment = 0; fragment < fragmentCount; ++fragment) {
            std::size_t slot{};
            while (slot < descriptor_.maximumScheduledDeliveries && deliveries_[slot].occupied)
                ++slot;
            if (slot == descriptor_.maximumScheduledDeliveries)
                return Fail<void>(NetworkErrors::TransportBudgetCapacityExceeded);
            const auto bytes = std::min(descriptor_.scenario.maximumFragmentBytes, payload.size() - sourceOffset);
            auto *destination = payloadStorage_.get() + slot * descriptor_.scenario.maximumFragmentBytes;
            std::copy_n(payload.data() + sourceOffset, bytes, destination);
            deliveries_[slot] = {
                .dueTick = DeliveryTick(),
                .sequence = ++sequence_,
                .connection = connection,
                .channel = channel,
                .ticket = ticket,
                .replaceableKey = replaceableKey,
                .bytes = bytes,
                .fragmentIndex = fragment,
                .fragmentCount = fragmentCount,
                .kind = DeterministicTransportEventKind::Packet,
                .occupied = true,
            };
            sourceOffset += bytes;
        }
        return Result<void>::Success();
    }

    Result<DeterministicTransport::ImpairmentPlan> DeterministicTransport::PlanSend(const ConnectionHandle connection,
                                                                                    const TransportTrafficClass traffic,
                                                                                    const std::uint64_t replaceableKey,
                                                                                    const std::size_t payloadBytes) {
        ImpairmentPlan plan{
            .randomBeforeAdmission = randomState_,
            .fragmentCount = (payloadBytes + descriptor_.scenario.maximumFragmentBytes - 1) / descriptor_.scenario.maximumFragmentBytes,
            .copyCount = 1,
            .lost = descriptor_.mode == DeterministicTransportMode::Simulated && Draw(descriptor_.scenario.lossPerTenThousand),
        };
        if (descriptor_.mode == DeterministicTransportMode::Simulated && Draw(descriptor_.scenario.duplicatePerTenThousand))
            plan.copyCount = 2;
        if (!plan.lost && plan.fragmentCount > descriptor_.maximumScheduledDeliveries / plan.copyCount) {
            randomState_ = plan.randomBeforeAdmission;
            return Fail<ImpairmentPlan>(NetworkErrors::TransportBudgetCapacityExceeded);
        }
        const auto required = plan.lost ? 0U : plan.fragmentCount * plan.copyCount;
        if (const auto reclaimable =
                traffic == TransportTrafficClass::ReplaceableState ? MatchingDeliveries(connection, replaceableKey) : 0U;
            required > FreeDeliveries() + reclaimable) {
            randomState_ = plan.randomBeforeAdmission;
            return Fail<ImpairmentPlan>(NetworkErrors::TransportBudgetCapacityExceeded);
        }
        return Result<ImpairmentPlan>::Success(plan);
    }

    Result<DeterministicSendResult> DeterministicTransport::ScheduleAdmitted(const ConnectionHandle connection, const ChannelId channel,
                                                                             const std::uint64_t replaceableKey,
                                                                             const std::span<const std::byte> payload,
                                                                             const ImpairmentPlan &plan,
                                                                             const TransportBudgetDecision &decision) {
        using enum TransportBudgetAdmission;
        if (decision.admission != Enqueued && decision.admission != Replaced) {
            randomState_ = plan.randomBeforeAdmission;
            return Result<DeterministicSendResult>::Success({MapAdmission(decision.admission), 0, 0});
        }
        if (decision.admission == Replaced)
            DiscardMatching(connection, replaceableKey);
        if (plan.lost) {
            static_cast<void>(budget_.Complete(decision.ticket));
            return Result<DeterministicSendResult>::Success({DeterministicSendOutcome::SimulatedLoss, 0, 0});
        }

        for (std::size_t copy = 0; copy < plan.copyCount; ++copy) {
            if (auto scheduled = ScheduleCopy(connection, channel, decision.ticket, replaceableKey, payload, plan.fragmentCount);
                scheduled.HasError()) {
                DiscardTicket(decision.ticket);
                static_cast<void>(budget_.Complete(decision.ticket));
                return Result<DeterministicSendResult>::Failure(scheduled.ErrorValue());
            }
        }
        auto outcome = DeterministicSendOutcome::Scheduled;
        if (plan.copyCount == 2)
            outcome = DeterministicSendOutcome::ScheduledWithDuplicate;
        else if (decision.admission == Replaced)
            outcome = DeterministicSendOutcome::Replaced;
        return Result<DeterministicSendResult>::Success({outcome, plan.fragmentCount, plan.copyCount});
    }

    /** @copydoc DeterministicTransport::Send */
    Result<DeterministicSendResult> DeterministicTransport::Send(const ConnectionHandle connection, const ChannelId channel,
                                                                 const TransportTrafficClass traffic, const std::uint64_t replaceableKey,
                                                                 const std::span<const std::byte> payload,
                                                                 const TransportAdmissionState state) {
        if (descriptor_.mode == DeterministicTransportMode::RejectAll)
            return Fail<DeterministicSendResult>(NetworkErrors::TransportCapabilityUnavailable);
        if (shuttingDown_)
            return Fail<DeterministicSendResult>(NetworkErrors::TransportShuttingDown);
        if (state == TransportAdmissionState::Cancelled)
            return Fail<DeterministicSendResult>(NetworkErrors::TransportOperationCancelled);
        if (state == TransportAdmissionState::ShuttingDown)
            return Fail<DeterministicSendResult>(NetworkErrors::TransportShuttingDown);
        if (state != TransportAdmissionState::Accepting)
            return Fail<DeterministicSendResult>(NetworkErrors::TransportBudgetInvalid);
        if (tick_ == 0 || channel.Value() >= descriptor_.maximumChannels || payload.empty() ||
            payload.size() > descriptor_.maximumPayloadBytes)
            return Fail<DeterministicSendResult>(NetworkErrors::TransportBudgetInvalid);

        auto plan = PlanSend(connection, traffic, replaceableKey, payload.size());
        if (plan.HasError())
            return Result<DeterministicSendResult>::Failure(plan.ErrorValue());

        auto admitted = budget_.Admit({connection, traffic, replaceableKey, payload.size()}, state);
        if (admitted.HasError()) {
            randomState_ = plan.Value().randomBeforeAdmission;
            return Result<DeterministicSendResult>::Failure(admitted.ErrorValue());
        }
        auto result = ScheduleAdmitted(connection, channel, replaceableKey, payload, plan.Value(), admitted.Value());
        if (result.HasValue() && metrics_ && metrics_->IsCollecting()) {
            using enum DeterministicSendOutcome;
            const auto outcome = result.Value().outcome;
            if (outcome == Scheduled || outcome == ScheduledWithDuplicate || outcome == Replaced || outcome == SimulatedLoss)
                (void)metrics_->RecordMessage(NetworkMetricDirection::Sent, NetworkMetricCategory::Transport, payload.size());
            if (outcome == SimulatedLoss)
                (void)metrics_->RecordLoss();
            if (outcome == DroppedReplaceable || outcome == ConnectionMustClose)
                (void)metrics_->RecordDrop(NetworkMetricDrop::Capacity);
        }
        return result;
    }

    std::size_t DeterministicTransport::FindNextDue(const std::uint64_t tick) const noexcept {
        auto selected = DeliveryCapacity();
        for (std::size_t index = 0; index < DeliveryCapacity(); ++index) {
            const auto &candidate = deliveries_[index];
            if (!candidate.occupied || candidate.dueTick > tick)
                continue;
            if (selected == DeliveryCapacity() || candidate.dueTick < deliveries_[selected].dueTick ||
                (candidate.dueTick == deliveries_[selected].dueTick && candidate.sequence < deliveries_[selected].sequence))
                selected = index;
        }
        return selected;
    }

    void DeterministicTransport::ReleaseDelivery(const std::size_t index) noexcept {
        const auto ticket = deliveries_[index].ticket;
        deliveries_[index].occupied = false;
        if (!ticket.IsValid())
            return;
        for (std::size_t candidate = 0; candidate < descriptor_.maximumScheduledDeliveries; ++candidate) {
            if (deliveries_[candidate].occupied && deliveries_[candidate].ticket == ticket)
                return;
        }
        static_cast<void>(budget_.Complete(ticket));
    }

    /** @copydoc DeterministicTransport::Advance */
    Result<std::size_t> DeterministicTransport::Advance(const std::uint64_t tick, const std::span<DeterministicTransportEvent> output) {
        if (shuttingDown_)
            return Fail<std::size_t>(NetworkErrors::TransportShuttingDown);
        if (auto advanced = budget_.BeginTick(tick); advanced.HasError())
            return Result<std::size_t>::Failure(advanced.ErrorValue());
        tick_ = tick;
        std::size_t written{};
        const bool observe = metrics_ && metrics_->IsCollecting();
        while (written < output.size()) {
            const auto index = FindNextDue(tick);
            if (index == DeliveryCapacity())
                break;
            const auto &delivery = deliveries_[index];
            output[written++] = {
                delivery.kind,          delivery.connection,
                delivery.channel,       {payloadStorage_.get() + index * descriptor_.scenario.maximumFragmentBytes, delivery.bytes},
                delivery.fragmentIndex, delivery.fragmentCount,
                delivery.sequence,
            };
            if (observe && delivery.kind == DeterministicTransportEventKind::Packet)
                (void)metrics_->RecordMessage(NetworkMetricDirection::Received, NetworkMetricCategory::Transport, delivery.bytes);
            ReleaseDelivery(index);
        }
        if (observe) {
            const auto snapshot = budget_.Snapshot();
            (void)metrics_->RecordLoss(0);
            (void)metrics_->SetQueueDepth(NetworkMetricQueue::Outbound, snapshot.queuedMessages);
            (void)metrics_->SetActiveConnections(snapshot.activeConnections);
        }
        return Result<std::size_t>::Success(written);
    }

    /** @copydoc DeterministicTransport::Close */
    Result<std::size_t> DeterministicTransport::Close(const ConnectionHandle connection) {
        if (shuttingDown_)
            return Fail<std::size_t>(NetworkErrors::TransportShuttingDown);
        std::size_t discarded{};
        for (std::size_t index = 0; index < descriptor_.maximumScheduledDeliveries; ++index) {
            auto &delivery = deliveries_[index];
            if (delivery.occupied && delivery.connection == connection) {
                delivery.occupied = false;
                ++discarded;
            }
        }
        if (auto closed = budget_.CloseConnection(connection); closed.HasError())
            return Result<std::size_t>::Failure(closed.ErrorValue());
        if (metrics_ && metrics_->IsCollecting()) {
            (void)metrics_->RecordDrop(NetworkMetricDrop::Cancelled, discarded);
            (void)metrics_->SetActiveConnections(budget_.Snapshot().activeConnections);
        }
        auto &event = deliveries_[descriptor_.maximumScheduledDeliveries + connection.Slot()];
        if (event.connection == connection && event.kind == DeterministicTransportEventKind::Disconnected)
            return Result<std::size_t>::Success(discarded);
        event = {.dueTick = tick_,
                 .sequence = ++sequence_,
                 .connection = connection,
                 .kind = DeterministicTransportEventKind::Disconnected,
                 .occupied = true};
        return Result<std::size_t>::Success(discarded);
    }

    /** @copydoc DeterministicTransport::Shutdown */
    std::size_t DeterministicTransport::Shutdown() noexcept {
        if (shuttingDown_)
            return 0;
        shuttingDown_ = true;
        std::size_t discarded{};
        for (std::size_t index = 0; index < DeliveryCapacity(); ++index) {
            discarded += deliveries_[index].occupied ? 1U : 0U;
            deliveries_[index].occupied = false;
        }
        static_cast<void>(budget_.Shutdown());
        if (metrics_ && metrics_->IsCollecting()) {
            (void)metrics_->RecordDrop(NetworkMetricDrop::Cancelled, discarded);
            (void)metrics_->SetQueueDepth(NetworkMetricQueue::Outbound, 0);
            (void)metrics_->SetActiveConnections(0);
        }
        return discarded;
    }
}  // namespace Horo::Network
