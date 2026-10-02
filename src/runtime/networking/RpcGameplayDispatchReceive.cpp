#include "Horo/Network/NetworkErrors.h"
#include "RpcGameplayDispatchState.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Horo::Network {
    using Detail::DispatchFlagGuard;

    namespace {
        struct WireParameter final {
            RpcParameterId id;
            std::span<const std::byte> bytes;
        };

        struct WireInvocation final {
            RpcId id;
            NetworkObjectId object;
            ReplicationSchemaVersion version;
            std::uint64_t sequence{};
            NetworkPeerId recipient;
            std::vector<WireParameter> parameters;
        };

        /** @brief Admits only the descriptor's bounded payload and exact supported delivery semantics. */
        Result<void> ValidateInvocationEnvelope(const RpcDescriptor &descriptor, const std::size_t payloadBytes,
                                                const DeliveryPolicy delivery) {
            if (payloadBytes > descriptor.maximumPayloadBytes)
                return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
            if ((descriptor.delivery == RpcDelivery::ReliableOrdered && delivery != DeliveryPolicy::ReliableOrdered) ||
                (descriptor.delivery == RpcDelivery::Unreliable && delivery != DeliveryPolicy::UnreliableUnordered &&
                 delivery != DeliveryPolicy::UnreliableSequenced))
                return Result<void>::Failure(MakeError(NetworkErrors::TransportDeliveryUnsupported));
            return Result<void>::Success();
        }

        /** @brief Reads fixed little-endian integers from bounded untrusted bytes. */
        template <typename Integer> bool Read(std::span<const std::byte> bytes, std::size_t &offset, Integer &value) noexcept {
            if (offset > bytes.size() || bytes.size() - offset < sizeof(Integer))
                return false;
            value = 0;
            for (std::size_t index = 0; index < sizeof(Integer); ++index)
                value |= static_cast<Integer>(std::to_integer<unsigned>(bytes[offset + index])) << (index * 8U);
            offset += sizeof(Integer);
            return true;
        }

        /** @brief Parses a complete versioned invocation before inspecting live state. */
        [[nodiscard]] Result<WireInvocation> DecodeWire(const std::span<const std::byte> bytes) {
            std::size_t offset = 0;
            std::uint64_t rpc = 0;
            std::uint64_t epoch = 0;
            std::uint64_t slot = 0;
            std::uint64_t sequence = 0;
            std::uint64_t recipient = 0;
            std::uint32_t generation = 0;
            std::uint16_t major = 0;
            std::uint16_t minor = 0;
            std::uint16_t count = 0;
            if (!Read(bytes, offset, rpc) || !Read(bytes, offset, epoch) || !Read(bytes, offset, slot) ||
                !Read(bytes, offset, generation) || !Read(bytes, offset, major) || !Read(bytes, offset, minor) ||
                !Read(bytes, offset, sequence) || !Read(bytes, offset, recipient) || !Read(bytes, offset, count) || count > 64 ||
                sequence == 0)
                return Result<WireInvocation>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
            const auto id = RpcId::Create(rpc);
            const auto authority = ReplicationAuthorityEpoch::Create(epoch);
            if (id.HasError() || authority.HasError() || major == 0)
                return Result<WireInvocation>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
            const auto object = NetworkObjectId::Create(authority.Value(), slot, generation);
            if (object.HasError())
                return Result<WireInvocation>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
            WireInvocation invocation{id.Value(),
                                      object.Value(),
                                      {major, minor},
                                      sequence,
                                      recipient == 0 ? NetworkPeerId{} : NetworkPeerId::Create(recipient).Value(),
                                      {}};
            invocation.parameters.reserve(count);
            std::uint32_t lastId = 0;
            for (std::uint16_t index = 0; index < count; ++index) {
                std::uint32_t parameterId = 0;
                std::uint32_t length = 0;
                if (!Read(bytes, offset, parameterId) || !Read(bytes, offset, length) || parameterId <= lastId || offset > bytes.size() ||
                    bytes.size() - offset < length)
                    return Result<WireInvocation>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
                const auto typedId = RpcParameterId::Create(parameterId);
                if (typedId.HasError())
                    return Result<WireInvocation>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
                invocation.parameters.emplace_back(typedId.Value(), bytes.subspan(offset, length));
                offset += length;
                lastId = parameterId;
            }
            if (offset != bytes.size())
                return Result<WireInvocation>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
            return Result<WireInvocation>::Success(std::move(invocation));
        }
    }  // namespace

    /** @copydoc RpcGameplayDispatch::Handle */
    Result<void> RpcGameplayDispatch::Handle(const MessageEnvelope &) {
        return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
    }

    /** @brief Selects one supplied payload or its explicitly declared version-compatible default. */
    static Result<std::span<const std::byte>> ParameterBytes(const WireInvocation &wire, const RpcParameterDescriptor &parameter,
                                                             std::size_t &supplied) {
        if (supplied < wire.parameters.size()) {
            const auto &input = wire.parameters[supplied];
            if (input.id < parameter.id)
                return Result<std::span<const std::byte>>::Failure(MakeError(NetworkErrors::RpcParameterUnsupported));
            if (input.id == parameter.id) {
                if (parameter.introducedVersion > wire.version)
                    return Result<std::span<const std::byte>>::Failure(MakeError(NetworkErrors::RpcDescriptorIncompatible));
                ++supplied;
                return Result<std::span<const std::byte>>::Success(input.bytes);
            }
        }
        if (parameter.requirement == RpcParameterRequirement::Optional && parameter.canonicalDefault)
            return Result<std::span<const std::byte>>::Success(parameter.canonicalDefault->canonicalBytes);
        return Result<std::span<const std::byte>>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
    }

    /** @brief Validates one owned value's exact representation, retained budget and canonical round trip. */
    static Result<ReplicationRuntimeValue> DecodeValue(const std::span<const std::byte> bytes, const RpcParameterDescriptor &parameter,
                                                       const IReplicationFieldSerializer &serializer,
                                                       const ReplicationSerializerDescriptor &metadata, const std::size_t maximumValueBytes,
                                                       std::size_t &valueBytes) {
        if (bytes.size() > parameter.limits.maximumEncodedBytes)
            return Result<ReplicationRuntimeValue>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        auto decoded = serializer.Decode(bytes);
        if (decoded.HasError())
            return decoded;
        const auto &value = decoded.Value();
        const std::size_t elements = std::visit([]<typename T>(const T &item) -> std::size_t {
            if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<std::byte>>)
                return item.size();
            else
                return 1;
        }, value);
        if (value.index() != static_cast<std::size_t>(metadata.valueKind) || elements > parameter.limits.maximumElementCount)
            return Result<ReplicationRuntimeValue>::Failure(MakeError(NetworkErrors::RpcParameterUnsupported));
        const std::size_t retainedBytes = value.index() >= 4 ? elements : sizeof(std::uint64_t);
        if (retainedBytes > maximumValueBytes - valueBytes)
            return Result<ReplicationRuntimeValue>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        if (const auto canonical = serializer.Encode(value); canonical.HasError() || !std::ranges::equal(canonical.Value(), bytes))
            return Result<ReplicationRuntimeValue>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
        valueBytes += retainedBytes;
        return decoded;
    }

    /** @brief Resolves and canonicalizes all declared values before replay or queue state changes. */
    static Result<std::vector<ReplicationRuntimeValue>> DecodeValues(
        const WireInvocation &wire, const RpcDescriptor &descriptor,
        const std::span<const std::shared_ptr<const IReplicationFieldSerializer>> serializers,
        const std::span<const ReplicationSerializerDescriptor> metadata, const std::size_t maximumValueBytes) {
        if (!descriptor.compatibility.Contains(wire.version) || descriptor.parameters.size() != serializers.size())
            return Result<std::vector<ReplicationRuntimeValue>>::Failure(MakeError(NetworkErrors::RpcDescriptorIncompatible));
        std::vector<ReplicationRuntimeValue> values;
        values.reserve(descriptor.parameters.size());
        std::size_t supplied = 0;
        std::size_t valueBytes = 0;
        for (std::size_t index = 0; index < descriptor.parameters.size(); ++index) {
            const auto &parameter = descriptor.parameters[index];
            const auto bytes = ParameterBytes(wire, parameter, supplied);
            if (bytes.HasError())
                return Result<std::vector<ReplicationRuntimeValue>>::Failure(bytes.ErrorValue());
            auto decoded = DecodeValue(bytes.Value(), parameter, *serializers[index], metadata[index], maximumValueBytes, valueBytes);
            if (decoded.HasError())
                return Result<std::vector<ReplicationRuntimeValue>>::Failure(std::move(decoded).ErrorValue());
            values.push_back(std::move(decoded).Value());
        }
        if (supplied != wire.parameters.size())
            return Result<std::vector<ReplicationRuntimeValue>>::Failure(MakeError(NetworkErrors::RpcParameterUnsupported));
        return Result<std::vector<ReplicationRuntimeValue>>::Success(std::move(values));
    }

    /** @copydoc RpcGameplayDispatch::HandleAdmitted */
    Result<void> RpcGameplayDispatch::HandleAdmitted(const InboundMessageContext &context, const MessageEnvelope &message) {
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (draining_)
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        if (!context.session || context.ownerTick == 0 || context.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        if (pending_.size() == limits_.maximumPending)
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        if (terminals_.accepted == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        if (message.payload.size() > limits_.maximumInvocationBytes)
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        const auto peer = std::ranges::find_if(peers_, [&context](const Peer &entry) {
            return entry.connection == context.connection && entry.generation == context.generation && entry.session == context.session;
        });
        if (peer == peers_.end())
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        if (const auto admitted = peer->session->AdmitGameplay(peer->connection, peer->generation, context.ownerTick); admitted.HasError())
            return admitted;
        if (const auto charged = ChargeWork(*peer, message.payload.size(), context.ownerTick); charged.HasError())
            return charged;
        if (const auto queued = std::ranges::count_if(pending_,
                                                      [&context](const Pending &command) {
            return command.connection == context.connection && command.generation == context.generation;
        });
            static_cast<std::size_t>(queued) >= std::min(limits_.maximumPending, limits_.maximumPendingPerPeer))
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        [[maybe_unused]] const auto self = shared_from_this();
        const DispatchFlagGuard guard{receiving_};
        const auto revision = revocationRevision_;
        try {
            return StageAdmitted(context, message, *peer, revision);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        } catch (const std::invalid_argument &) {
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        } catch (const std::out_of_range &) {
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        } catch (...) {
            // Non-standard adapter exceptions are terminal receipt failures, never queued work.
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        }
    }

    /** @copydoc RpcGameplayDispatch::CheckReplay */
    Result<RpcGameplayDispatch::ReplayScope *> RpcGameplayDispatch::CheckReplay(const Peer &peer, const NetworkObjectId object,
                                                                                const RpcDescriptor &descriptor,
                                                                                const std::uint64_t sequence) {
        if (descriptor.delivery != RpcDelivery::ReliableOrdered)
            return Result<ReplayScope *>::Success(nullptr);
        if (const auto scope = std::ranges::find_if(replay_,
                                                    [&peer, object, &descriptor](const ReplayScope &entry) {
            return entry.connection == peer.connection && entry.generation == peer.generation && entry.object == object &&
                   entry.id == descriptor.id;
        });
            scope != replay_.end()) {
            if (sequence <= scope->highestAccepted)
                return Result<ReplayScope *>::Failure(MakeError(NetworkErrors::MessageDeliveryInvalid));
            return Result<ReplayScope *>::Success(std::to_address(scope));
        }
        if (replay_.size() == limits_.maximumReplayScopes)
            return Result<ReplayScope *>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        return Result<ReplayScope *>::Success(nullptr);
    }

    /** @copydoc RpcGameplayDispatch::StageAdmitted */
    Result<void> RpcGameplayDispatch::StageAdmitted(const InboundMessageContext &context, const MessageEnvelope &message, const Peer &peer,
                                                    const std::uint64_t revision) {
        auto decoded = DecodeWire(message.payload);
        if (decoded.HasError())
            return Result<void>::Failure(std::move(decoded).ErrorValue());
        const auto &wire = decoded.Value();
        auto admitted = AdmitTarget(context, message, peer, wire.id, wire.object, wire.sequence, wire.recipient);
        if (admitted.HasError())
            return Result<void>::Failure(std::move(admitted).ErrorValue());
        const auto &target = admitted.Value();
        auto values = DecodeValues(wire, *target.binding.descriptor, target.binding.serializers, target.binding.metadata,
                                   limits_.maximumInvocationBytes);
        if (values.HasError())
            return Result<void>::Failure(std::move(values).ErrorValue());
        Pending candidate{context.connection,
                          context.generation,
                          wire.object,
                          wire.id,
                          wire.sequence,
                          wire.recipient,
                          target.live.Descriptor().scene,
                          target.live.Descriptor().session,
                          target.role,
                          target.entity,
                          std::move(values).Value(),
                          context.cancellation};
        if (const auto current = RevalidateAdmission(candidate, target, context.ownerTick, revision); current.HasError())
            return current;
        if (const auto allowed = AuthorizePolicy(target.binding, target.peer, target.object, candidate.values); allowed.HasError())
            return allowed;
        if (const auto current = RevalidateAdmission(candidate, target, context.ownerTick, revision); current.HasError())
            return current;
        QueueAccepted(std::move(candidate), target.binding.descriptor->delivery, target.replay);
        return Result<void>::Success();
    }

    /** @copydoc RpcGameplayDispatch::AdmitTarget */
    Result<RpcGameplayDispatch::AdmittedInvocation> RpcGameplayDispatch::AdmitTarget(const InboundMessageContext &context,
                                                                                     const MessageEnvelope &message, const Peer &peer,
                                                                                     const RpcId id, const NetworkObjectId identity,
                                                                                     const std::uint64_t sequence,
                                                                                     const NetworkPeerId recipient) {
        const auto binding = std::ranges::find_if(bindings_, [id](const Binding &entry) {
            return entry.id == id;
        });
        const auto object = std::ranges::find_if(objects_, [identity](const Object &entry) {
            return entry.identity == identity;
        });
        if (binding == bindings_.end() || object == objects_.end() || binding->handler.expired())
            return Result<AdmittedInvocation>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        if (const auto envelope = ValidateInvocationEnvelope(*binding->descriptor, message.payload.size(), context.delivery);
            envelope.HasError())
            return Result<AdmittedInvocation>::Failure(envelope.ErrorValue());
        Runtime::EntityRef entity;
        auto live =
            ValidateLive(peer, *object, *binding->descriptor, recipient, context.ownerTick, Runtime::RuntimePhase::NetworkPoll, entity);
        if (live.HasError())
            return Result<AdmittedInvocation>::Failure(live.ErrorValue());
        const auto scope = CheckReplay(peer, identity, *binding->descriptor, sequence);
        if (scope.HasError())
            return Result<AdmittedInvocation>::Failure(scope.ErrorValue());
        if (const auto charged = ChargeRate(peer, *binding->descriptor, context.ownerTick); charged.HasError())
            return Result<AdmittedInvocation>::Failure(charged.ErrorValue());
        // Adapter callbacks can revoke bindings. Pin their code and storage through decoding.
        const auto pinnedRole = object->role->Snapshot();
        if (pinnedRole.HasError())
            return Result<AdmittedInvocation>::Failure(pinnedRole.ErrorValue());
        return Result<AdmittedInvocation>::Success(
            {binding->Pin(), peer, *object, std::move(live).Value(), entity, pinnedRole.Value(), scope.Value()});
    }

    /** @copydoc RpcGameplayDispatch::RevalidateAdmission */
    Result<void> RpcGameplayDispatch::RevalidateAdmission(const Pending &command, const AdmittedInvocation &target,
                                                          const std::uint64_t nowTick, const std::uint64_t revision) const {
        if (stopped_ || revision != revocationRevision_ || command.cancellation.IsCancellationRequested() || target.live.IsRevoked())
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        Runtime::EntityRef entity;
        if (const auto current = ValidateLive(target.peer, target.object, *target.binding.descriptor, command.recipient, nowTick,
                                              Runtime::RuntimePhase::NetworkPoll, entity);
            current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (const auto currentRole = target.object.role->Snapshot();
            currentRole.HasError() || currentRole.Value() != command.role || entity != command.entity)
            return Result<void>::Failure(MakeError(NetworkErrors::RpcPermissionDenied));
        return Result<void>::Success();
    }

    /** @copydoc RpcGameplayDispatch::QueueAccepted */
    void RpcGameplayDispatch::QueueAccepted(Pending command, const RpcDelivery delivery, ReplayScope *const scope) {
        pending_.emplace_back(std::move(command));
        const auto &accepted = pending_.back();
        ++terminals_.accepted;
        if (delivery == RpcDelivery::ReliableOrdered) {
            if (!scope)
                replay_.emplace_back(accepted.connection, accepted.generation, accepted.object, accepted.id, accepted.sequence);
            else
                scope->highestAccepted = accepted.sequence;
        }
    }

}  // namespace Horo::Network
