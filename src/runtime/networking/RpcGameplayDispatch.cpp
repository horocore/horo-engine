#include "Horo/Network/NetworkErrors.h"
#include "RpcGameplayDispatchState.h"

#include <algorithm>
#include <exception>
#include <new>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Horo::Network {
    using Detail::DispatchFlagGuard;

    /** @copydoc RpcGameplayDispatch::RpcGameplayDispatch */
    RpcGameplayDispatch::RpcGameplayDispatch(ConstructionKey, RpcDescriptorSnapshotPtr descriptors, ReplicationWorldLifecycle &world,
                                             const RpcDispatchLimits &limits, NetworkDebugger *debugger)
        : descriptors_(std::move(descriptors)), world_(world), limits_(limits), owner_(std::this_thread::get_id()), debugger_(debugger),
          diagnosticSource_(debugger ? debugger->Source() : NetworkDiagnosticSource{}) {
        peers_.reserve(limits.maximumPeers);
        objects_.reserve(limits.maximumObjects);
        bindings_.reserve(limits.maximumBindings);
        pending_.reserve(limits.maximumPending);
        replay_.reserve(limits.maximumReplayScopes);
        rates_.reserve(limits.maximumRateScopes);
        work_.reserve(limits.maximumCallerScopes + 1);
        work_.emplace_back();  // The first scope is the immutable global accounting bucket.
    }

    /** @copydoc RpcGameplayDispatch::~RpcGameplayDispatch */
    RpcGameplayDispatch::~RpcGameplayDispatch() {
        Shutdown();
    }

    /** @copydoc RpcGameplayDispatch::Create */
    Result<std::shared_ptr<RpcGameplayDispatch>> RpcGameplayDispatch::Create(RpcDescriptorSnapshotPtr descriptors,
                                                                             ReplicationWorldLifecycle &world,
                                                                             const RpcDispatchLimits &limits, NetworkDebugger *debugger) {
        if (!descriptors || limits.maximumPeers == 0 || limits.maximumPeers > 4096 || limits.maximumBindings == 0 ||
            limits.maximumBindings > 4096 || limits.maximumObjects == 0 || limits.maximumObjects > 65536 || limits.maximumPending == 0 ||
            limits.maximumPending > 4096 || limits.maximumReplayScopes == 0 || limits.maximumReplayScopes > 65536 ||
            limits.maximumPerDrain == 0 || limits.maximumPerDrain > limits.maximumPending || limits.maximumInvocationBytes == 0 ||
            limits.maximumInvocationBytes > 1024 * 1024 || limits.maximumRateScopes == 0 || limits.maximumRateScopes > 65536 ||
            limits.maximumCallerScopes == 0 || limits.maximumCallerScopes > 65536 || limits.ticksPerSecond == 0 ||
            limits.ticksPerSecond > 1000000000 || limits.maximumAttemptsPerSecond == 0 || limits.maximumGlobalAttemptsPerSecond == 0 ||
            limits.maximumBytesPerSecond == 0 || limits.maximumGlobalBytesPerSecond == 0 || limits.maximumPendingPerPeer == 0 ||
            limits.maximumPendingPerPeer > 4096)
            return Result<std::shared_ptr<RpcGameplayDispatch>>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
        try {
            return Result<std::shared_ptr<RpcGameplayDispatch>>::Success(
                std::make_shared<RpcGameplayDispatch>(ConstructionKey{}, std::move(descriptors), world, limits, debugger));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<RpcGameplayDispatch>>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        }
    }

    /** @copydoc RpcGameplayDispatch::CheckOwner */
    Result<void> RpcGameplayDispatch::CheckOwner() const {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkIoWrongThread));
        if (stopped_)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionShuttingDown));
        if (receiving_)
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        return Result<void>::Success();
    }

    /** @copydoc RpcGameplayDispatch::RegisterPeer */
    Result<void> RpcGameplayDispatch::RegisterPeer(const std::shared_ptr<PeerSessionLifecycle> &session, const ConnectionHandle connection,
                                                   const NetworkOperationGeneration generation, const NetworkPeerId peer,
                                                   const RpcRemoteRole role, const std::uint64_t nowTick,
                                                   const NetworkPeerId localRecipient) {
        using enum RpcRemoteRole;
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (!session || !connection.IsValid() || !generation.IsValid() || !peer.IsValid() || (role != Client && role != Authority) ||
            (role == Authority && !localRecipient.IsValid()) || (role == Client && localRecipient.IsValid()))
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        if (const auto admitted = session->AdmitGameplay(connection, generation, nowTick); admitted.HasError())
            return admitted;
        if (std::ranges::any_of(peers_, [connection, peer](const Peer &entry) {
            return entry.connection == connection || entry.identity == peer;
        }))
            return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorConflict));
        if (peers_.size() == limits_.maximumPeers)
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        peers_.emplace_back(session, connection, generation, peer, role, localRecipient);
        return Result<void>::Success();
    }

    /** @copydoc RpcGameplayDispatch::RevokePeer */
    void RpcGameplayDispatch::RevokePeer(const ConnectionHandle connection) noexcept {
        if (std::this_thread::get_id() != owner_)
            return;
        ++revocationRevision_;
        terminals_.cancelled += std::erase_if(pending_, [connection](const Pending &entry) {
            return entry.connection == connection;
        });
        std::erase_if(peers_, [connection](const Peer &entry) {
            return entry.connection == connection;
        });
    }

    /** @copydoc RpcGameplayDispatch::RegisterObject */
    Result<void> RpcGameplayDispatch::RegisterObject(const NetworkObjectId object, const std::shared_ptr<ReplicationRoleState> &role) {
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (!object.IsValid() || !role)
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        if (const auto snapshot = role->Snapshot(); snapshot.HasError() || snapshot.Value().object != object)
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        if (std::ranges::any_of(objects_, [object](const Object &entry) {
            return entry.identity == object;
        }))
            return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorConflict));
        if (objects_.size() == limits_.maximumObjects)
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        objects_.emplace_back(object, role);
        return Result<void>::Success();
    }

    /** @copydoc RpcGameplayDispatch::RevokeObject */
    void RpcGameplayDispatch::RevokeObject(const NetworkObjectId object) noexcept {
        if (std::this_thread::get_id() != owner_)
            return;
        ++revocationRevision_;
        terminals_.cancelled += std::erase_if(pending_, [object](const Pending &entry) {
            return entry.object == object;
        });
        std::erase_if(objects_, [object](const Object &entry) {
            return entry.identity == object;
        });
    }

    /** @copydoc RpcGameplayDispatch::RegisterHandler */
    Result<void> RpcGameplayDispatch::RegisterHandler(const RpcId id, std::shared_ptr<IRpcGameplayHandler> handler,
                                                      const std::span<const std::shared_ptr<const IReplicationFieldSerializer>> serializers,
                                                      std::shared_ptr<const void> moduleLease, const RpcGameplayPolicy &policy) {
        // Move both parameters into ordered local owners: parameter/temporary teardown
        // order must not release module code before the handler's virtual destructor.
        const auto codeLease = std::move(moduleLease);
        const auto handlerPin = std::move(handler);
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (!id.IsValid() || !handlerPin || !codeLease)
            return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
        const auto found = descriptors_->Find(id);
        if (found.HasError())
            return Result<void>::Failure(found.ErrorValue());
        const RpcDescriptor &descriptor = *found.Value();
        if (serializers.size() != descriptor.parameters.size())
            return Result<void>::Failure(MakeError(NetworkErrors::RpcParameterUnsupported));
        if (std::ranges::any_of(bindings_, [id](const Binding &entry) {
            return entry.id == id;
        }))
            return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorConflict));
        if (bindings_.size() == limits_.maximumBindings)
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        [[maybe_unused]] const auto self = shared_from_this();
        const DispatchFlagGuard guard{receiving_};
        const auto revision = revocationRevision_;
        // A metadata callback is external code too. Pin every input before invoking any adapter.
        try {
            if (policy.parameters.size() > descriptor.parameters.size())
                return Result<void>::Failure(MakeError(NetworkErrors::RpcParameterUnsupported));
            Binding binding{id,
                            &descriptor,
                            codeLease,
                            handlerPin,
                            {serializers.begin(), serializers.end()},
                            {},
                            std::make_shared<const RpcGameplayPolicy>(policy)};
            if (const auto captured = CaptureSerializerMetadata(binding, revision); captured.HasError())
                return captured;
            if (const auto valid = ValidatePolicy(binding); valid.HasError())
                return valid;
            bindings_.push_back(std::move(binding));
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        }
        return Result<void>::Success();
    }

    /** @copydoc RpcGameplayDispatch::CaptureSerializerMetadata */
    Result<void> RpcGameplayDispatch::CaptureSerializerMetadata(Binding &binding, const std::uint64_t revision) const {
        binding.metadata.reserve(binding.serializers.size());
        for (std::size_t index = 0; index < binding.serializers.size(); ++index) {
            const auto &serializer = binding.serializers[index];
            if (!serializer)
                return Result<void>::Failure(MakeError(NetworkErrors::RpcParameterUnsupported));
            binding.metadata.push_back(serializer->Descriptor());
            if (stopped_)
                return Result<void>::Failure(MakeError(NetworkErrors::SessionShuttingDown));
            if (revision != revocationRevision_)
                return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
            const auto &metadata = binding.metadata.back();
            const auto &parameter = binding.descriptor->parameters[index];
            if (metadata.valueType != parameter.valueType || metadata.codec != parameter.codec ||
                metadata.owner != binding.descriptor->owner || metadata.valueKind >= ReplicationValueKind::Count ||
                metadata.maximumEncodedBytes < parameter.limits.maximumEncodedBytes ||
                metadata.maximumElementCount < parameter.limits.maximumElementCount)
                return Result<void>::Failure(MakeError(NetworkErrors::RpcParameterUnsupported));
        }
        return Result<void>::Success();
    }

    /** @copydoc RpcGameplayDispatch::RevokeHandler */
    void RpcGameplayDispatch::RevokeHandler(const RpcId id) noexcept {
        if (std::this_thread::get_id() != owner_)
            return;
        ++revocationRevision_;
        terminals_.cancelled += std::erase_if(pending_, [id](const Pending &entry) {
            return entry.id == id;
        });
        std::erase_if(bindings_, [id](const Binding &entry) {
            return entry.id == id;
        });
    }

    /** @copydoc RpcGameplayDispatch::Shutdown */
    void RpcGameplayDispatch::Shutdown() noexcept {
        if (std::this_thread::get_id() != owner_)
            return;
        if (stopped_)
            return;
        stopped_ = true;
        terminals_.cancelled += pending_.size();
        if (debugger_)
            (void)debugger_->Observe(diagnosticSource_,
                                     NetworkRpcRecord{terminals_.accepted, terminals_.succeeded, terminals_.failed, terminals_.cancelled});
        pending_.clear();
        replay_.clear();
        rates_.clear();
        work_.clear();
        bindings_.clear();
        objects_.clear();
        peers_.clear();
    }

    /** @copydoc RpcGameplayDispatch::TerminalTotals */
    Result<RpcTerminalTotals> RpcGameplayDispatch::TerminalTotals() const {
        if (std::this_thread::get_id() != owner_)
            return Result<RpcTerminalTotals>::Failure(MakeError(NetworkErrors::NetworkIoWrongThread));
        return Result<RpcTerminalTotals>::Success(terminals_);
    }
}  // namespace Horo::Network
