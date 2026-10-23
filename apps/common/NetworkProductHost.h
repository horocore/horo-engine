#pragma once

#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "NetworkProductSession.h"

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
        std::shared_ptr<ClientSessionOwner> sessions; /**< Required in network modes; retained through teardown. */
    };

    // Application-owned bridge: target admission precedes the canonical runtime lifecycle.
    class NetworkProductHost final {
        struct ConstructionKey final {};

    public:
        [[nodiscard]] static Result<std::unique_ptr<NetworkProductHost>> Create(Clock &clock, const NetworkProductHostInputs &inputs,
                                                                                Network::NetworkModeFactories factories,
                                                                                Network::NetworkTargetDiagnostic *diagnostic = nullptr,
                                                                                std::shared_ptr<NetworkDebuggerService> debugger = nullptr);

        NetworkProductHost(const NetworkProductHost &) = delete;
        NetworkProductHost &operator=(const NetworkProductHost &) = delete;
        NetworkProductHost(ConstructionKey, std::unique_ptr<Runtime::RuntimeHost> runtime, ModeParticipant *mode,
                           std::shared_ptr<NetworkDebuggerService> debugger) noexcept;
        ~NetworkProductHost() noexcept;

        [[nodiscard]] Result<void> Startup();
        [[nodiscard]] Result<void> RunFrame();
        /**
         * @brief Queue bounded replacement worlds at the canonical safe point.
         * @param worlds Replacement world inventory copied synchronously.
         * @param admittedClientSession Exact admitted client generation, when required by the selected mode.
         * @return Success, or typed rejection for invalid/overlapping work or an unconsumed prior failure.
         * @details Consume any prior travel failure before issuing another request. A fresh host starts
         * without a result; a stopped host/session owner cannot restart as an independent session.
         */
        [[nodiscard]] Result<void> RequestTravel(std::span<const Network::NetworkModeWorld> worlds,
                                                 std::optional<Network::NetworkOperationGeneration> admittedClientSession = std::nullopt);
        /**
         * @brief Consume a queued-travel failure even after automatic failure teardown or shutdown.
         * @return Original typed failure once, or null when no failure remains. Shutdown discards
         * unexecuted travel without manufacturing a failure and preserves already recorded errors.
         */
        [[nodiscard]] std::optional<Error> TakeTravelFailure();
        /**
         * @brief Retain the exact canonical lifecycle and admit it using the host-selected session clock.
         * @param world Client world receiving the admitted generation.
         * @param session Owned shared canonical lifecycle, already active under the host trust authority.
         * @param connection Exact transport connection generation bound by that lifecycle.
         * @param generation Exact session generation; replacements require a new transport handle.
         * @return Canonical admission result or typed clock, generation, role or lifecycle error.
         */
        [[nodiscard]] Result<void> AdmitClientSession(Network::NetworkModeWorldKind world,
                                                      std::shared_ptr<Network::PeerSessionLifecycle> session,
                                                      Network::ConnectionHandle connection, Network::NetworkOperationGeneration generation);
        /**
         * @brief Borrow the original retained lifecycle terminal until the next admission or destruction.
         * @return Original terminal, or null while no retained lifecycle has terminalized.
         */
        [[nodiscard]] const Network::PeerSessionTerminalSnapshot *ClientSessionTerminal() const noexcept;
        /** @brief Borrow the original outbound transport-attempt terminal through teardown; never implies a peer/session existed. */
        [[nodiscard]] const ClientConnectionAttemptTerminal *ClientConnectionTerminal() const noexcept;
        [[nodiscard]] Result<void> DisconnectClient(Network::NetworkModeWorldKind world, Network::NetworkOperationGeneration generation);
        [[nodiscard]] Result<Network::NetworkModeRoleView> Role(Network::NetworkModeWorldKind world, Runtime::SceneRuntimeId scene) const;
        void Shutdown() noexcept;

    private:
        std::shared_ptr<NetworkDebuggerService> debugger_;
        std::unique_ptr<Runtime::RuntimeHost> runtime_;
        ModeParticipant *mode_{};  // Borrowed from runtime_'s lifecycle participant.
    };
}  // namespace Horo::Application::Internal
