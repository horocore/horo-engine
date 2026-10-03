#pragma once

// Target-private dispatch ownership; never installed or exported to consumers.
#include "Horo/Network/RpcGameplayDispatch.h"

#include <functional>
#include <utility>

namespace Horo::Network::Detail {
    /** @brief Closes reentrant admission until the owning dispatch operation returns. */
    class DispatchFlagGuard final {
    public:
        explicit DispatchFlagGuard(bool &flag) noexcept : flag_(flag) {
            flag_.get() = true;
        }

        DispatchFlagGuard(const DispatchFlagGuard &) = delete;
        DispatchFlagGuard &operator=(const DispatchFlagGuard &) = delete;
        DispatchFlagGuard(DispatchFlagGuard &&) = delete;
        DispatchFlagGuard &operator=(DispatchFlagGuard &&) = delete;

        ~DispatchFlagGuard() {
            flag_.get() = false;
        }

    private:
        std::reference_wrapper<bool> flag_;
    };

}  // namespace Horo::Network::Detail

namespace Horo::Network {
    struct RpcGameplayDispatch::Peer final {
        std::shared_ptr<PeerSessionLifecycle> session;
        ConnectionHandle connection;
        NetworkOperationGeneration generation;
        NetworkPeerId identity;
        RpcRemoteRole role;
        NetworkPeerId localRecipient;
    };

    struct RpcGameplayDispatch::Object final {
        NetworkObjectId identity;
        std::shared_ptr<ReplicationRoleState> role;
    };

    struct RpcGameplayDispatch::Binding final {
        RpcId id;
        const RpcDescriptor *descriptor{};
        std::shared_ptr<const void> moduleLease;
        std::weak_ptr<IRpcGameplayHandler> handler;
        std::vector<std::shared_ptr<const IReplicationFieldSerializer>> serializers;
        std::vector<ReplicationSerializerDescriptor> metadata;

        Binding(RpcId identity, const RpcDescriptor *declaration, std::shared_ptr<const void> lease,
                std::weak_ptr<IRpcGameplayHandler> owner, std::vector<std::shared_ptr<const IReplicationFieldSerializer>> codecs,
                std::vector<ReplicationSerializerDescriptor> codecMetadata)
            : id(identity), descriptor(declaration), moduleLease(std::move(lease)), handler(std::move(owner)),
              serializers(std::move(codecs)), metadata(std::move(codecMetadata)) {}

        Binding(const Binding &) = delete;
        Binding &operator=(const Binding &) = delete;

        /** @brief Transfers adapters together with the lease that keeps their module code executable. */
        Binding(Binding &&other) noexcept
            : id(other.id), descriptor(std::exchange(other.descriptor, nullptr)), moduleLease(std::move(other.moduleLease)),
              handler(std::move(other.handler)), serializers(std::move(other.serializers)), metadata(std::move(other.metadata)) {}

        /** @brief Pins callback adapters and code without permitting unsafe memberwise assignment. */
        [[nodiscard]] Binding Pin() const {
            return {id, descriptor, moduleLease, handler, serializers, metadata};
        }

        ~Binding() {
            // Both normal destruction and assignment retirement explicitly drop adapters before code.
            serializers.clear();
            handler.reset();
            moduleLease.reset();
        }

        /** @brief Retires old adapters before their code lease, including vector erase compaction. */
        Binding &operator=(Binding &&other) noexcept {
            if (this != &other) {
                Binding retired(std::move(other));
                std::swap(id, retired.id);
                std::swap(descriptor, retired.descriptor);
                moduleLease.swap(retired.moduleLease);
                handler.swap(retired.handler);
                serializers.swap(retired.serializers);
                metadata.swap(retired.metadata);
            }
            return *this;
        }
    };

    struct RpcGameplayDispatch::Pending final {
        ConnectionHandle connection;
        NetworkOperationGeneration generation;
        NetworkObjectId object;
        RpcId id;
        std::uint64_t sequence{};
        NetworkPeerId recipient;
        Runtime::SceneRuntimeId scene;
        NetworkSessionGeneration session;
        std::vector<ReplicationRuntimeValue> values;
        CancellationToken cancellation;
    };

    struct RpcGameplayDispatch::ReplayScope final {
        ConnectionHandle connection;
        NetworkOperationGeneration generation;
        NetworkObjectId object;
        RpcId id;
        std::uint64_t highestAccepted{};
    };

}  // namespace Horo::Network
