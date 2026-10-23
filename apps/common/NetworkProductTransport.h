#pragma once
/** @file NetworkProductTransport.h @brief Private product transport/admission composition shared by concrete host factories. */
#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Network/NetworkAddress.h"
#include "Horo/Network/NetworkAdmissionController.h"
#include "NetworkProductSession.h"

#include <array>
#include <memory>
#include <vector>

namespace Horo::Application::Internal {
    /** @brief One explicit host-selected existing dispatch binding with a weak gameplay-owner lifetime. */
    struct NetworkProductInboundHandler final {
        Network::InboundHandlerBinding binding;
        std::weak_ptr<Network::IInboundMessageHandler> owner;
    };

    /**
     * @brief Immutable owned host policy/provider capture, made before deferred product factory calls.
     * @details Captures only host-selected inert policy and retained capabilities. Neither Capture
     * nor inspection invokes an authority, selects a backend, registers a service or opens a listener.
     * Feature/channel views point into this immovable shared owner and never into caller storage.
     */
    class NetworkProductAdmission final {
        class ConstructionKey final {
            friend class NetworkProductAdmission;

            ConstructionKey() noexcept {}
        };

    public:
        /** @brief Factory-only stable shared capture; only Capture can create the nonaggregate construction key. */
        NetworkProductAdmission(ConstructionKey, const Network::NetworkAdmissionPolicy &policy,
                                Network::NetworkAdmissionProviders providers, std::shared_ptr<const Network::MessageCodecRegistry> codecs,
                                std::span<const NetworkProductInboundHandler> handlers);
        /**
         * @brief Deep-capture spans and retain the exact canonical codec/provider owners.
         * @param policy Host-selected inert admission policy, borrowed only during this call.
         * @param providers Owned security capabilities; no default authority is installed.
         * @param codecs Same immutable inventory later borrowed by the product dispatcher.
         * @param handlers Explicit existing bindings; their gameplay owners remain weak and may revoke independently.
         * @return Immutable stable capture or typed bounds/configuration failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<const NetworkProductAdmission>> Capture(
            const Network::NetworkAdmissionPolicy &policy, Network::NetworkAdmissionProviders providers,
            std::shared_ptr<const Network::MessageCodecRegistry> codecs, std::span<const NetworkProductInboundHandler> handlers = {});
        NetworkProductAdmission(const NetworkProductAdmission &) = delete;
        NetworkProductAdmission &operator=(const NetworkProductAdmission &) = delete;

        /** @brief Borrow captured inert policy. @return View valid while this capture is retained. */
        [[nodiscard]] const Network::NetworkAdmissionPolicy &Policy() const noexcept {
            return policy_;
        }

        /** @brief Retain authority owners through controller shutdown and native transport detach. @return Immutable bundle. */
        [[nodiscard]] const Network::NetworkAdmissionProviders &Providers() const noexcept {
            return providers_;
        }

        /** @brief Borrow bounded host-selected existing handler bindings. @return Immutable weak-owner bindings. */
        [[nodiscard]] std::span<const NetworkProductInboundHandler> Handlers() const noexcept {
            return handlers_;
        }

        /** @brief Borrow the retained canonical codec owner. @return Exact immutable codec snapshot. */
        [[nodiscard]] const std::shared_ptr<const Network::MessageCodecRegistry> &Codecs() const noexcept {
            return codecs_;
        }

    private:
        std::array<Network::ProtocolFeatureId, Network::MaximumHandshakeFeatures> supported_{};
        std::array<Network::ProtocolFeatureId, Network::MaximumHandshakeFeatures> required_{};
        std::vector<Network::MessageDeliveryChannel> channels_;
        std::vector<NetworkProductInboundHandler> handlers_;
        Network::NetworkAdmissionPolicy policy_;
        Network::NetworkAdmissionProviders providers_;
        std::shared_ptr<const Network::MessageCodecRegistry> codecs_;
    };

    /**
     * @brief Compose the actual product service around one explicitly selected concrete transport.
     * @param transport Unique backend-neutral transport already selected by the application factory.
     * @param role Selected product role; standalone must not construct this service.
     * @param bind Explicit server bind address.
     * @param connect Explicit outgoing endpoint.
     * @param debugger Retained product diagnostics owner.
     * @param sessions Same application lifecycle projection/clock used by the canonical product host.
     * @param admission Optional explicit host capabilities; absence closes every untrusted connection.
     * @return Prepared service or original typed configuration failure. Native Initialize occurs in Prepare.
     * @details Admission controllers/providers are retained before listening. Shutdown terminalizes
     * admission, revokes dispatch, detaches the session borrow, shuts down transport, then releases providers.
     */
    [[nodiscard]] Result<std::unique_ptr<Network::INetworkModeService>> CreateNetworkProductTransport(
        std::unique_ptr<Network::INetworkTransport> transport, Network::NetworkProjectRole role, const Network::NetworkAddress &bind,
        const Network::NetworkAddress &connect, std::shared_ptr<NetworkDebuggerService> debugger,
        std::shared_ptr<ClientSessionOwner> sessions, std::shared_ptr<const NetworkProductAdmission> admission = nullptr);
}  // namespace Horo::Application::Internal
