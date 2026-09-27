#pragma once

/**
 * @file NetworkModeComposition.h
 * @brief Host-owned, generation-fenced composition of the four runtime network modes.
 */

#include "Horo/Network/NetworkTargetCapabilities.h"
#include "Horo/Network/PeerSessionLifecycle.h"
#include "Horo/Network/ReplicationRoles.h"
#include "Horo/Runtime/FrameScheduler.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace Horo::Network {
    /** @brief A participant's owner; listen-server worlds are always distinct. */
    enum class NetworkModeWorldKind : std::uint8_t {
        Standalone,
        AuthorityServer,
        Client,
        Count
    };

    /** @brief Closed service vocabulary selected before any factory is invoked. */
    enum class NetworkModeServiceKind : std::uint8_t {
        Transport,
        Session,
        Replication,
        Scene,
        Physics,
        Audio,
        Renderer,
        Gui,
        Input,
        LocalPlayer,
        Count,
    };

    /** @brief Optional presentation features installed by this host, not authority evidence. */
    struct NetworkModePresentation final {
        bool audio{};
        bool renderer{};
        bool gui{};
        bool input{};
        bool localPlayer{};
    };

    /** @brief Unpublished identity supplied by the Scene owner for one world candidate. */
    struct NetworkModeWorld final {
        NetworkModeWorldKind kind{NetworkModeWorldKind::Count};
        Runtime::SceneRuntimeId scene{};
        ReplicationAuthorityEpoch authority{}; /**< Present only for an authority world. */
    };

    /** @brief Immutable validated mode and exact world roles for one host generation. */
    struct NetworkModePlan final {
        NetworkProjectRole mode{NetworkProjectRole::Count};
        std::uint64_t hostGeneration{};
        NetworkProductBuildId build{};
        NetworkProductCapabilityRevision productRevision{};
        NetworkHostCapabilityRevision hostRevision{};
        NetworkProjectSettingsRevision projectRevision{};
        NetworkTransportProviderId provider{}; /**< Absent only for standalone. */
        ProtocolVersion protocolVersion{};     /**< Absent only for standalone. */
        NetworkModePresentation presentation{};
        std::array<NetworkModeWorld, 2> worlds{};
        std::size_t worldCount{};
    };

    /**
     * @brief Resolve one complete mode plan from exact admitted package/host/project evidence.
     * @param assessment Pure target assessment for the selected invocation.
     * @param selection Exact selected role and revisions used by the assessment.
     * @param hostGeneration Non-zero current host generation.
     * @param worlds One or two distinct unpublished Scene identities in mode order.
     * @param presentation Explicit optional host capabilities; forbidden in dedicated mode.
     * @return Inert plan or typed failure, without invoking a factory or publishing a role.
     */
    [[nodiscard]] Result<NetworkModePlan> ResolveNetworkModePlan(const NetworkTargetAssessment &assessment,
                                                                 const NetworkTargetSelection &selection, std::uint64_t hostGeneration,
                                                                 std::span<const NetworkModeWorld> worlds,
                                                                 NetworkModePresentation presentation = {});

    /** @brief Factory request, with Count world kind for process-scoped services. */
    struct NetworkModeServiceRequest final {
        NetworkModeServiceKind service{NetworkModeServiceKind::Count};
        NetworkProjectRole mode{NetworkProjectRole::Count};
        NetworkTransportProviderId provider{};
        NetworkModeWorldKind world{NetworkModeWorldKind::Count};
        Runtime::SceneRuntimeId scene{};
    };

    /** @brief A borrowed host service facade; concrete backend and gameplay types stay at the host boundary. */
    class INetworkModeService {
    public:
        virtual ~INetworkModeService() = default;
        /** @brief Prepare owned state without external publication. */
        [[nodiscard]] virtual Result<void> Prepare() = 0;
        /** @brief Activate after all services prepared; world publication remains host-controlled. */
        [[nodiscard]] virtual Result<void> Activate() = 0;
        /** @brief Participate in a canonical non-fixed runtime phase without defining an alternate schedule. */
        [[nodiscard]] virtual Result<void> RunPhase(Runtime::RuntimePhase phase) = 0;
        /** @brief Advance one exact host-issued fixed tick. */
        [[nodiscard]] virtual Result<void> RunFixedTick(const Runtime::FixedStepContext &context) = 0;
        /** @brief Revoke admission and release state; safe after partial preparation and repeated calls. */
        virtual void Shutdown() noexcept = 0;
    };

    /** @brief Host-owned factory; no concrete transport is linked by NetworkRuntime itself. */
    using NetworkModeServiceFactory = std::function<Result<std::unique_ptr<INetworkModeService>>(const NetworkModeServiceRequest &)>;

    /** @brief Exact factories supplied at the application composition root. */
    struct NetworkModeFactories final {
        std::array<NetworkModeServiceFactory, static_cast<std::size_t>(NetworkModeServiceKind::Count)> services{};
    };

    /** @brief Immutable gameplay role evidence, never a transferable object authority grant. */
    struct NetworkModeRoleView final {
        std::uint64_t hostGeneration{};
        Runtime::SceneRuntimeId scene{};
        ReplicationExecutionRole role{ReplicationExecutionRole::Count};
        std::optional<NetworkOperationGeneration> session;
        ReplicationAuthorityEpoch authority{};
    };

    /**
     * @brief One owner-thread mode lifecycle; each mode shares the same service and phase path.
     * @details Factories are invoked only after a complete plan and factory set validate. Views publish only
     * after every participant activates. Shutdown revokes views before destroying services in reverse order.
     */
    class NetworkModeComposition final {
    public:
        /** @brief Create a stopped composition without constructing services. */
        [[nodiscard]] static Result<NetworkModeComposition> Create(NetworkModePlan plan, NetworkModeFactories factories);
        NetworkModeComposition(const NetworkModeComposition &) = delete;
        NetworkModeComposition &operator=(const NetworkModeComposition &) = delete;
        NetworkModeComposition(NetworkModeComposition &&) noexcept;
        NetworkModeComposition &operator=(NetworkModeComposition &&) = delete;
        ~NetworkModeComposition() noexcept;

        /** @brief Construct, prepare and activate exactly the selected service set, or unwind it. */
        [[nodiscard]] Result<void> Start();
        /** @brief Dispatch the canonical runtime phase to currently active participants. */
        [[nodiscard]] Result<void> RunPhase(Runtime::RuntimePhase phase);
        /** @brief Dispatch one exact canonical fixed tick to active participants. */
        [[nodiscard]] Result<void> RunFixedTick(const Runtime::FixedStepContext &context);
        /** @brief Publish a client session only after the peer lifecycle admits exact active evidence. */
        [[nodiscard]] Result<void> AdmitClientSession(NetworkModeWorldKind world, const PeerSessionLifecycle &session,
                                                      ConnectionHandle connection, NetworkOperationGeneration generation,
                                                      std::uint64_t nowTick);
        /** @brief Revoke only the matching client session; never promote it to standalone or transfer server authority. */
        [[nodiscard]] Result<void> DisconnectClient(NetworkModeWorldKind world, NetworkOperationGeneration generation);
        /** @brief Atomically replace Scene generations for all worlds at the host lifecycle safe point. */
        [[nodiscard]] Result<void> Travel(std::span<const NetworkModeWorld> replacements, Runtime::RuntimePhase phase,
                                          std::optional<NetworkOperationGeneration> admittedClientSession = std::nullopt);
        /** @brief Acquire current world-scoped role evidence; stale Scene identities fail. */
        [[nodiscard]] Result<NetworkModeRoleView> Role(NetworkModeWorldKind world, Runtime::SceneRuntimeId scene) const;
        /** @brief Revalidate a copied role view before a privileged owner operation. */
        [[nodiscard]] Result<void> ValidateRole(const NetworkModeRoleView &view) const;
        /** @brief Idempotently revoke views before releasing all participants. */
        void Shutdown() noexcept;
        /** @brief Whether this host has published all selected worlds. */
        [[nodiscard]] bool Active() const noexcept;

    private:
        struct Entry final {
            NetworkModeServiceRequest request{};
            std::unique_ptr<INetworkModeService> service;
        };

        NetworkModeComposition(NetworkModePlan plan, NetworkModeFactories factories) noexcept;
        [[nodiscard]] Result<void> BuildEntries();
        void ReleaseEntries() noexcept;
        [[nodiscard]] std::size_t WorldIndex(NetworkModeWorldKind kind) const noexcept;
        [[nodiscard]] Result<void> AppendTravelEntry(std::array<Entry, 5> &prepared, std::size_t &count,
                                                     const NetworkModeServiceRequest &request);
        [[nodiscard]] Result<std::size_t> PrepareTravelEntries(std::span<const NetworkModeWorld> replacements,
                                                               std::array<Entry, 5> &prepared);
        static void DiscardTravelEntries(std::array<Entry, 5> &prepared, std::size_t count) noexcept;
        void CommitTravelEntries(std::span<const NetworkModeWorld> replacements, std::array<Entry, 5> &prepared,
                                 std::size_t count) noexcept;

        NetworkModePlan plan_;
        NetworkModeFactories factories_;
        std::array<Entry, 24> entries_{};
        std::size_t entryCount_{};
        std::array<std::optional<NetworkOperationGeneration>, 2> sessions_{};
        std::array<std::optional<NetworkOperationGeneration>, 2> lastSessions_{};
        bool started_{};
        bool stopped_{};
    };
}  // namespace Horo::Network
