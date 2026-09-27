#pragma once

#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Runtime/RuntimeHost.h"

#include <memory>
#include <optional>
#include <span>

namespace Horo::Application::Internal {
    class ModeParticipant;

    struct NetworkProductHostInputs final {
        const Network::NetworkProjectSettings &project;
        const Network::NetworkProductCapabilityManifest &product;
        const Network::NetworkTargetPackageInventory &inventory;
        const Network::NetworkTargetHostFacts &hostFacts;
        const Network::NetworkTargetRequirements &requirements;
        const Network::NetworkTargetSelection &selection;
        std::uint64_t hostGeneration{};
        std::span<const Network::NetworkModeWorld> worlds;
        Network::NetworkModePresentation presentation{};
    };

    // Application-owned bridge: target admission precedes the canonical runtime lifecycle.
    class NetworkProductHost final {
        struct ConstructionKey final {};

    public:
        [[nodiscard]] static Result<std::unique_ptr<NetworkProductHost>> Create(Clock &clock, const NetworkProductHostInputs &inputs,
                                                                                Network::NetworkModeFactories factories,
                                                                                Network::NetworkTargetDiagnostic *diagnostic = nullptr);

        NetworkProductHost(const NetworkProductHost &) = delete;
        NetworkProductHost &operator=(const NetworkProductHost &) = delete;
        NetworkProductHost(ConstructionKey, std::unique_ptr<Runtime::RuntimeHost> runtime, ModeParticipant *mode) noexcept;
        ~NetworkProductHost() noexcept;

        [[nodiscard]] Result<void> Startup();
        [[nodiscard]] Result<void> RunFrame();
        [[nodiscard]] Result<void> RequestTravel(std::span<const Network::NetworkModeWorld> worlds,
                                                 std::optional<Network::NetworkOperationGeneration> admittedClientSession = std::nullopt);
        [[nodiscard]] std::optional<Error> TakeTravelFailure();
        [[nodiscard]] Result<void> AdmitClientSession(Network::NetworkModeWorldKind world, const Network::PeerSessionLifecycle &session,
                                                      Network::ConnectionHandle connection, Network::NetworkOperationGeneration generation,
                                                      std::uint64_t nowTick);
        [[nodiscard]] Result<void> DisconnectClient(Network::NetworkModeWorldKind world, Network::NetworkOperationGeneration generation);
        [[nodiscard]] Result<Network::NetworkModeRoleView> Role(Network::NetworkModeWorldKind world, Runtime::SceneRuntimeId scene) const;
        void Shutdown() noexcept;

    private:
        std::unique_ptr<Runtime::RuntimeHost> runtime_;
        ModeParticipant *mode_{};  // Borrowed from runtime_'s lifecycle participant.
    };
}  // namespace Horo::Application::Internal
