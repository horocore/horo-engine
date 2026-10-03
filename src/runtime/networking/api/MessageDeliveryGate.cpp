#include "Horo/Network/MessageDeliveryGate.h"

#include "Horo/Network/NetworkErrors.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace Horo::Network {
    namespace {
        constexpr std::size_t MaximumDeliveryChannels = 64;
        constexpr std::uint32_t ReplayWindowBits = 64;

        /** @brief Checks the closed traffic/policy vocabulary before allocating channel state. */
        bool ValidChannel(const MessageDeliveryChannel &channel) noexcept {
            if (channel.delivery >= DeliveryPolicy::Count || channel.traffic >= MessageTrafficClass::Count)
                return false;
            return !channel.replaceable ||
                   (channel.traffic == MessageTrafficClass::Snapshot &&
                    (channel.delivery == DeliveryPolicy::UnreliableUnordered || channel.delivery == DeliveryPolicy::UnreliableSequenced));
        }

        /** @brief Converts an error descriptor into an owned result without losing its typed identity. */
        Result<void> Reject(const ErrorCodeDescriptor &error) {
            return Result<void>::Failure(MakeError(error));
        }
    }  // namespace

    /** @copydoc MessageDeliveryGate::Create */
    Result<MessageDeliveryGate> MessageDeliveryGate::Create(const TransportSelectionEvidence &selection,
                                                            const std::uint64_t expectedRevision, const ConnectionHandle connection,
                                                            const NetworkOperationGeneration sessionGeneration,
                                                            const std::vector<MessageDeliveryChannel> &channels) {
        if (!connection.IsValid() || !sessionGeneration.IsValid() || channels.empty() || channels.size() > MaximumDeliveryChannels)
            return Result<MessageDeliveryGate>::Failure(MakeError(NetworkErrors::MessageDeliveryInvalid));

        MessageDeliveryGate gate;
        gate.selection_ = selection;
        gate.revision_ = expectedRevision;
        gate.connection_ = connection;
        gate.sessionGeneration_ = sessionGeneration;
        gate.channels_.resize(MaximumDeliveryChannels);
        for (const auto &channel : channels) {
            if (!ValidChannel(channel) || channel.channel.Value() >= MaximumDeliveryChannels ||
                gate.channels_[channel.channel.Value()].configured)
                return Result<MessageDeliveryGate>::Failure(MakeError(NetworkErrors::MessageDeliveryInvalid));
            const TransportSendRequirement requirement{channel.delivery, channel.channel, 0, 0};
            if (const auto admitted = AdmitTransportSend(selection, expectedRevision, requirement, TransportAdmissionState::Accepting);
                admitted.HasError())
                return Result<MessageDeliveryGate>::Failure(admitted.ErrorValue());
            auto &state = gate.channels_[channel.channel.Value()];
            state.policy = channel;
            state.configured = true;
        }
        return Result<MessageDeliveryGate>::Success(std::move(gate));
    }

    /** @copydoc MessageDeliveryGate::Shutdown */
    void MessageDeliveryGate::Shutdown() noexcept {
        shuttingDown_ = true;
    }

    /** @brief Applies one policy after all generation, capability, and expiry checks. */
    Result<void> MessageDeliveryGate::Admit(const MessageDeliveryInput &input, const std::uint64_t nowTick) {
        if (shuttingDown_)
            return Reject(NetworkErrors::MessageDeliveryTerminal);
        if (input.connection != connection_ || input.sessionGeneration != sessionGeneration_ || !input.connection.IsValid() ||
            !input.sessionGeneration.IsValid() || nowTick < lastTick_ || input.channel.Value() >= channels_.size() ||
            input.sequence.Value() == 0)
            return Reject(NetworkErrors::MessageDeliveryInvalid);

        auto &state = channels_[input.channel.Value()];
        if (!state.configured || input.delivery != state.policy.delivery || input.traffic != state.policy.traffic)
            return Reject(NetworkErrors::MessageDeliveryInvalid);
        const TransportSendRequirement requirement{input.delivery, input.channel, input.payloadBytes, 0};
        if (const auto transport = AdmitTransportSend(selection_, revision_, requirement, TransportAdmissionState::Accepting);
            transport.HasError())
            return transport;
        if (input.expiresAtTick != 0 && nowTick >= input.expiresAtTick)
            return Reject(NetworkErrors::MessageDeliveryExpired);

        if (const auto replay = AdmitSequence(state, input.sequence.Value()); replay.HasError())
            return replay;
        lastTick_ = nowTick;
        return Result<void>::Success();
    }

    /** @brief Advances one channel replay window only after all metadata and expiry checks succeed. */
    Result<void> MessageDeliveryGate::AdmitSequence(ChannelState &state, const std::uint32_t sequence) {
        if (state.policy.delivery == DeliveryPolicy::ReliableOrdered) {
            if (state.highestSequence == std::numeric_limits<std::uint32_t>::max())
                return Reject(NetworkErrors::MessageCounterExhausted);
            const std::uint32_t expected = state.highestSequence + 1U;
            if (sequence != expected)
                return Reject(sequence <= state.highestSequence ? NetworkErrors::MessageDeliveryDuplicate
                                                                : NetworkErrors::MessageDeliveryOutOfOrder);
        }
        if (state.highestSequence == 0 || sequence > state.highestSequence) {
            const std::uint32_t advance = sequence - state.highestSequence;
            state.seenWindow = (advance >= ReplayWindowBits ? 0 : state.seenWindow << advance) | 1ULL;
            state.highestSequence = sequence;
        } else {
            const std::uint32_t age = state.highestSequence - sequence;
            if (age >= ReplayWindowBits)
                return Reject(NetworkErrors::MessageDeliveryOutOfOrder);
            if ((state.seenWindow & (1ULL << age)) != 0)
                return Reject(NetworkErrors::MessageDeliveryDuplicate);
            if (state.policy.delivery == DeliveryPolicy::UnreliableSequenced || state.policy.replaceable ||
                state.policy.delivery == DeliveryPolicy::ReliableOrdered)
                return Reject(NetworkErrors::MessageDeliveryOutOfOrder);
            state.seenWindow |= 1ULL << age;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Network
