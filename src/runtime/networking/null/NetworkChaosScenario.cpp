#include "Horo/Network/NetworkChaosScenario.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::Network {
    namespace NetworkChaosErrors {
        namespace {
            const ErrorDomainId Domain{"horo.network"};
        }

        const ErrorCodeDescriptor Invalid{Domain,
                                          ErrorCode{"network.chaos.invalid"},
                                          ErrorSeverity::Error,
                                          "The deterministic scenario is invalid.",
                                          "Correct the bounded scenario input.",
                                          false,
                                          false};
        const ErrorCodeDescriptor Disabled{Domain,
                                           ErrorCode{"network.chaos.disabled"},
                                           ErrorSeverity::Error,
                                           "Fault injection is disabled in this profile.",
                                           "Select an explicit test or preview profile.",
                                           false,
                                           false};
        const ErrorCodeDescriptor StaleScope{Domain,
                                             ErrorCode{"network.chaos.stale_scope"},
                                             ErrorSeverity::Error,
                                             "The scenario scope is stale.",
                                             "Retire work from the replaced host or session.",
                                             false,
                                             false};
        const ErrorCodeDescriptor CheckpointMismatch{Domain,
                                                     ErrorCode{"network.chaos.checkpoint_mismatch"},
                                                     ErrorSeverity::Error,
                                                     "A deterministic checkpoint did not match.",
                                                     "Inspect the bounded scenario evidence.",
                                                     false,
                                                     false};
    }  // namespace NetworkChaosErrors

    namespace {
        template <typename T> Result<T> Fail(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Validate replacement without touching live transport state or allocating storage. */
        bool ValidReplacement(const TransportImpairmentScenarioV1 &previous, const TransportImpairmentScenarioV1 &next) noexcept {
            return next.contractVersion == 1 && next.revision > previous.revision && next.seed == previous.seed &&
                   next.maximumFragmentBytes == previous.maximumFragmentBytes && next.lossPerTenThousand <= 10'000 &&
                   next.duplicatePerTenThousand <= 10'000 && next.reorderPerTenThousand <= 10'000 &&
                   next.latencyTicks <= std::numeric_limits<std::uint32_t>::max() - next.jitterTicks;
        }

        /** @brief Validate unique direct-index topology before preparing any transport storage. */
        bool ValidTopology(const NetworkChaosScenarioDescriptor &descriptor) {
            for (std::size_t index = 0; index < descriptor.topology.size(); ++index) {
                const auto connection = descriptor.topology[index];
                if (!connection.IsValid() || connection.Slot() >= descriptor.transport.budgetCapacity.maximumConnections)
                    return false;
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (descriptor.topology[prior].Slot() == connection.Slot())
                        return false;
            }
            return true;
        }

        /** @brief Track exact restart generations without opening connections or executing callbacks. */
        bool ValidLifecycleAction(const NetworkChaosAction &action, const std::span<ConnectionHandle> current,
                                  const std::span<bool> active) {
            const auto found = std::ranges::find(current, action.connection);
            if (found == current.end())
                return false;
            const auto index = static_cast<std::size_t>(found - current.begin());
            if (action.kind == NetworkChaosActionKind::Disconnect) {
                if (!active[index])
                    return false;
                active[index] = false;
                return true;
            }
            const auto next = action.connection.NextGeneration();
            if (next.HasError())
                return false;
            current[index] = next.Value();
            active[index] = true;
            return true;
        }

        /** @brief Validate one closed action against the preceding immutable replay state. */
        bool ValidAction(const NetworkChaosAction &action, TransportImpairmentScenarioV1 &impairment, bool &stalled,
                         const std::span<ConnectionHandle> current, const std::span<bool> active) {
            using enum NetworkChaosActionKind;
            switch (action.kind) {
                case Impairment:
                    if (action.connection.IsValid() || !ValidReplacement(impairment, action.impairment))
                        return false;
                    impairment = action.impairment;
                    return true;
                case Stall:
                case Resume: {
                    const bool nextStalled = action.kind == Stall;
                    if (action.connection.IsValid() || nextStalled == stalled)
                        return false;
                    stalled = nextStalled;
                    return true;
                }
                case Disconnect:
                case Restart:
                    return ValidLifecycleAction(action, current, active);
                case Count:
                    return false;
            }
            return false;
        }

        /** @brief Preflight the complete timeline so malformed late actions never partially execute a run. */
        bool ValidTimeline(const NetworkChaosScenarioDescriptor &descriptor) {
            std::array<ConnectionHandle, 16> current{};
            std::array<bool, 16> active{};
            std::ranges::copy(descriptor.topology, current.begin());
            std::ranges::fill(active, true);
            auto impairment = descriptor.transport.scenario;
            std::uint64_t previousTick{};
            bool stalled{};
            for (const auto &action : descriptor.timeline) {
                if (action.tick == 0 || action.tick < previousTick || action.tick > descriptor.durationTicks ||
                    !ValidAction(action, impairment, stalled, std::span{current}.first(descriptor.topology.size()),
                                 std::span{active}.first(descriptor.topology.size())))
                    return false;
                previousTick = action.tick;
            }
            return true;
        }

        /** @brief Check finite, strictly ordered expected checkpoints independently of optional evidence recording. */
        bool ValidCheckpoints(const NetworkChaosScenarioDescriptor &descriptor) {
            std::uint64_t previousTick{};
            for (const auto &checkpoint : descriptor.checkpoints) {
                if (checkpoint.tick <= previousTick || checkpoint.tick > descriptor.durationTicks ||
                    checkpoint.actions > descriptor.timeline.size())
                    return false;
                previousTick = checkpoint.tick;
            }
            return true;
        }
    }  // namespace

    NetworkChaosScenario::NetworkChaosScenario(const NetworkChaosScenarioDescriptor &descriptor, DeterministicTransport transport) noexcept
        : descriptor_(descriptor), transport_(std::move(transport)) {
        std::ranges::copy(descriptor.topology, topology_.begin());
        std::ranges::copy(descriptor.timeline, timeline_.begin());
        std::ranges::copy(descriptor.checkpoints, checkpoints_.begin());
        RebindViews();
    }

    NetworkChaosScenario::NetworkChaosScenario(NetworkChaosScenario &&other) noexcept
        : descriptor_(other.descriptor_), transport_(std::move(other.transport_)), topology_(other.topology_), timeline_(other.timeline_),
          checkpoints_(other.checkpoints_), disconnected_(other.disconnected_), pendingRestarts_(other.pendingRestarts_),
          evidence_(other.evidence_), nextAction_(other.nextAction_), nextCheckpoint_(other.nextCheckpoint_), stalled_(other.stalled_),
          terminal_(other.terminal_) {
        RebindViews();
        other.terminal_ = true;
    }

    void NetworkChaosScenario::RebindViews() noexcept {
        descriptor_.topology = {topology_.data(), descriptor_.topology.size()};
        descriptor_.timeline = {timeline_.data(), descriptor_.timeline.size()};
        descriptor_.checkpoints = {checkpoints_.data(), descriptor_.checkpoints.size()};
    }

    /** @copydoc NetworkChaosScenario::Create */
    Result<NetworkChaosScenario> NetworkChaosScenario::Create(const NetworkChaosScenarioDescriptor &descriptor) {
        if (descriptor.profile == NetworkChaosProfile::Disabled)
            return Fail<NetworkChaosScenario>(NetworkChaosErrors::Disabled);
        if (descriptor.profile >= NetworkChaosProfile::Count || !descriptor.scope.host.IsValid() || !descriptor.scope.session.IsValid() ||
            descriptor.revision == 0 || descriptor.durationTicks == 0 || descriptor.durationTicks > 1'000'000 ||
            descriptor.transport.mode != DeterministicTransportMode::Simulated || descriptor.maximumEventsPerTick == 0 ||
            descriptor.maximumEventsPerTick > 128 || descriptor.topology.empty() || descriptor.topology.size() > 16 ||
            descriptor.timeline.size() > 128 || descriptor.checkpoints.size() > 64 || !ValidTopology(descriptor) ||
            !ValidTimeline(descriptor) || !ValidCheckpoints(descriptor))
            return Fail<NetworkChaosScenario>(NetworkChaosErrors::Invalid);
        auto transport = DeterministicTransport::Create(descriptor.transport);
        if (transport.HasError())
            return Result<NetworkChaosScenario>::Failure(transport.ErrorValue());
        auto prepared = std::move(transport).Value();
        for (const auto connection : descriptor.topology) {
            if (const auto opened = prepared.Open(connection); opened.HasError())
                return Result<NetworkChaosScenario>::Failure(opened.ErrorValue());
        }
        return Result<NetworkChaosScenario>::Success(NetworkChaosScenario{descriptor, std::move(prepared)});
    }

    /** @copydoc NetworkChaosScenario::Send */
    Result<DeterministicSendResult> NetworkChaosScenario::Send(const NetworkChaosScope scope, const ConnectionHandle connection,
                                                               const ChannelId channel, const TransportTrafficClass traffic,
                                                               const std::uint64_t replaceableKey,
                                                               const std::span<const std::byte> payload) {
        if (scope != descriptor_.scope)
            return Fail<DeterministicSendResult>(NetworkChaosErrors::StaleScope);
        if (terminal_)
            return Fail<DeterministicSendResult>(NetworkErrors::TransportShuttingDown);
        return transport_.Send(connection, channel, traffic, replaceableKey, payload);
    }

    std::size_t NetworkChaosScenario::TopologyIndex(const ConnectionHandle connection) const noexcept {
        for (std::size_t index = 0; index < descriptor_.topology.size(); ++index)
            if (topology_[index].Slot() == connection.Slot())
                return index;
        return descriptor_.topology.size();
    }

    Result<void> NetworkChaosScenario::Apply(const NetworkChaosAction &action) {
        using enum NetworkChaosActionKind;
        switch (action.kind) {
            case Impairment:
                return transport_.ReplaceScenario(transport_.Descriptor().scenario.revision, action.impairment);
            case Stall:
                stalled_ = true;
                return Result<void>::Success();
            case Resume:
                stalled_ = false;
                return Result<void>::Success();
            case Disconnect:
            case Restart: {
                const auto index = TopologyIndex(action.connection);
                if (index == descriptor_.topology.size() || pendingRestarts_[index].next.IsValid())
                    return Fail<void>(NetworkChaosErrors::Invalid);
                if (const auto closed = transport_.Close(action.connection); closed.HasError())
                    return Result<void>::Failure(closed.ErrorValue());
                if (action.kind == Restart)
                    pendingRestarts_[index] = {action.connection, action.connection.NextGeneration().Value()};
                return Result<void>::Success();
            }
            case Count:
                return Fail<void>(NetworkChaosErrors::Invalid);
        }
        return Fail<void>(NetworkChaosErrors::Invalid);
    }

    Result<void> NetworkChaosScenario::CompleteRestarts(const std::span<const DeterministicTransportEvent> events) {
        for (const auto &event : events) {
            if (event.kind != DeterministicTransportEventKind::Disconnected)
                continue;
            const auto index = TopologyIndex(event.connection);
            if (index != descriptor_.topology.size())
                disconnected_[index] = event.connection;
        }
        for (std::size_t index = 0; index < descriptor_.topology.size(); ++index) {
            const auto &pending = pendingRestarts_[index];
            if (!pending.next.IsValid() || disconnected_[index] != pending.previous)
                continue;
            // Open clears the transport's terminal slot. Its real event must already be in this or an earlier drain.
            if (const auto opened = transport_.Open(pending.next); opened.HasError())
                return opened;
            pendingRestarts_[index] = {};
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkChaosScenario::Advance */
    Result<std::size_t> NetworkChaosScenario::Advance(const NetworkChaosScope scope, const std::uint64_t tick,
                                                      const std::span<DeterministicTransportEvent> output) {
        if (scope != descriptor_.scope)
            return Fail<std::size_t>(NetworkChaosErrors::StaleScope);
        if (terminal_)
            return Fail<std::size_t>(NetworkErrors::TransportShuttingDown);
        if (tick != evidence_.tick + 1 || tick > descriptor_.durationTicks || output.size() < descriptor_.maximumEventsPerTick)
            return Fail<std::size_t>(NetworkChaosErrors::Invalid);
        while (nextAction_ < descriptor_.timeline.size() && timeline_[nextAction_].tick == tick) {
            if (const auto applied = Apply(timeline_[nextAction_]); applied.HasError()) {
                (void)Shutdown();
                return Result<std::size_t>::Failure(applied.ErrorValue());
            }
            ++nextAction_;
            ++evidence_.actions;
        }
        auto advanced =
            transport_.Advance(tick, stalled_ ? std::span<DeterministicTransportEvent>{} : output.first(descriptor_.maximumEventsPerTick));
        if (advanced.HasError()) {
            (void)Shutdown();
            return advanced;
        }
        if (const auto completed = CompleteRestarts(output.first(advanced.Value())); completed.HasError()) {
            (void)Shutdown();
            return Result<std::size_t>::Failure(completed.ErrorValue());
        }
        evidence_.tick = tick;
        for (const auto &event : output.first(advanced.Value())) {
            if (event.kind == DeterministicTransportEventKind::Packet)
                ++evidence_.packets;
            else
                ++evidence_.disconnects;
        }
        if (nextCheckpoint_ < descriptor_.checkpoints.size() && checkpoints_[nextCheckpoint_].tick == tick) {
            if (const auto &expected = checkpoints_[nextCheckpoint_++]; evidence_.packets != expected.packets ||
                                                                        evidence_.disconnects != expected.disconnects ||
                                                                        evidence_.actions != expected.actions) {
                (void)Shutdown();
                return Fail<std::size_t>(NetworkChaosErrors::CheckpointMismatch);
            }
            ++evidence_.checkpoints;
        }
        if (tick == descriptor_.durationTicks)
            (void)Shutdown();
        return advanced;
    }

    /** @copydoc NetworkChaosScenario::Close */
    Result<std::size_t> NetworkChaosScenario::Close(const NetworkChaosScope scope, const ConnectionHandle connection) {
        if (scope != descriptor_.scope)
            return Fail<std::size_t>(NetworkChaosErrors::StaleScope);
        if (terminal_)
            return Fail<std::size_t>(NetworkErrors::TransportShuttingDown);
        const auto closed = transport_.Close(connection);
        if (closed.HasValue()) {
            const auto index = TopologyIndex(connection);
            if (index != descriptor_.topology.size() && pendingRestarts_[index].previous == connection)
                pendingRestarts_[index] = {};
        }
        return closed;
    }

    /** @copydoc NetworkChaosScenario::Shutdown */
    std::size_t NetworkChaosScenario::Shutdown() noexcept {
        if (terminal_)
            return 0;
        terminal_ = true;
        pendingRestarts_ = {};
        return transport_.Shutdown();
    }

    /** @copydoc NetworkChaosScenario::Evidence */
    NetworkChaosEvidence NetworkChaosScenario::Evidence() const noexcept {
        return descriptor_.recordEvidence ? evidence_ : NetworkChaosEvidence{};
    }

    /** @copydoc NetworkChaosScenario::Descriptor */
    const NetworkChaosScenarioDescriptor &NetworkChaosScenario::Descriptor() const noexcept {
        return descriptor_;
    }
}  // namespace Horo::Network
