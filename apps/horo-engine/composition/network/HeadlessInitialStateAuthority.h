#pragma once

/** @file HeadlessInitialStateAuthority.h @brief Private configured authority capture for secure network launch. */

#include "Horo/Network/NetworkInitialStateStream.h"

#include <memory>
#include <vector>

namespace Horo::Application::Internal {
    namespace HeadlessNetworkDetail {
        struct WorldOwners;
    }

    /** @brief Host-authorized initial recipient and ownership for one exact admitted mapping. */
    struct HeadlessInitialRecipient final {
        Network::NetworkPeerId peer;
        std::uint64_t descriptorGeneration{};
        std::vector<Network::ReplicationInitialOwnership> ownership; /**< Complete canonical object order. */
    };

    /** @brief Explicit project host authority for the gameplay identity behind an authenticated channel. */
    class IHeadlessInitialStateAuthority {
    public:
        virtual ~IHeadlessInitialStateAuthority() = default;
        /** @brief Resolve the exact active authenticated identity and generation; no inference from connection numbers.
         * @param authenticated Current accepted connection, trust and principal evidence.
         * @param world Current exact authority mapping, borrowed only during this invocation.
         * @return Complete peer, negotiated descriptor generation and ordered ownership, or typed denial. */
        [[nodiscard]] virtual Result<HeadlessInitialRecipient> Resolve(const Network::AuthenticationResult &authenticated,
                                                                       const Network::ReplicationWorldReadLease &world) const = 0;
    };

    /** @brief Host proof of the exact negotiated descriptor generation for a replica session. */
    class IHeadlessInitialReplicaAuthority {
    public:
        virtual ~IHeadlessInitialReplicaAuthority() = default;
        /** @brief Resolve the already-authenticated server and negotiated schema without gaining Scene or session mutation authority. */
        [[nodiscard]] virtual Result<std::uint64_t> DescriptorGeneration(const Network::AuthenticationResult &authenticated,
                                                                         const Network::HandshakeSelection &negotiated) const = 0;
    };

    /** @brief Adds canonical capture and optional replica Scene application while retaining existing client recipient admission. */
    class HeadlessInitialStateHost final : public Network::INetworkInitialStateHost {
    public:
        /** @brief Retains exact admitted server role policy and existing client owner without invoking them. */
        HeadlessInitialStateHost(std::shared_ptr<const IHeadlessInitialStateAuthority> authority,
                                 std::shared_ptr<Network::INetworkInitialStateHost> client,
                                 std::shared_ptr<const IHeadlessInitialReplicaAuthority> replica = {}) noexcept;
        /** @brief Bind to one configured application composition before any transport can open its listener. */
        [[nodiscard]] Result<void> BindWorlds(const std::shared_ptr<HeadlessNetworkDetail::WorldOwners> &worlds);

        /** @brief Reports the explicit Scene application selection before any candidate can bind a writer. */
        [[nodiscard]] bool AppliesReplicaTransforms() const noexcept {
            return static_cast<bool>(replica_);
        }

        /** @copydoc Network::INetworkInitialStateHost::ClientRecipient */
        [[nodiscard]] Result<Network::NetworkPeerId> ClientRecipient(const Network::PeerSessionLifecycle &peer) override;
        /** @copydoc Network::INetworkInitialStateHost::Capture */
        [[nodiscard]] Result<std::optional<Network::ReplicationInitialStateSnapshot>> Capture(
            const Network::ReplicationWorldReadLease &world, const Network::ReplicationWorldLifecycle &lifecycle,
            const Network::PeerSessionLifecycle &peer, std::size_t maximumWireBytes) override;
        /** @copydoc Network::INetworkInitialStateHost::PrepareReceiver */
        [[nodiscard]] Result<Network::NetworkInitialStateTarget> PrepareReceiver(
            const Network::ReplicationInboundAuthority &authority, Network::ReplicationWorldLifecycle &world,
            std::span<const Network::ReplicationInitialOwnership> ownership, std::uint64_t now) override;

    private:
        std::weak_ptr<HeadlessNetworkDetail::WorldOwners> worlds_;
        std::shared_ptr<const IHeadlessInitialStateAuthority> authority_;
        std::shared_ptr<Network::INetworkInitialStateHost> client_;
        std::shared_ptr<const IHeadlessInitialReplicaAuthority> replica_;
        bool bound_{};
    };
}  // namespace Horo::Application::Internal
