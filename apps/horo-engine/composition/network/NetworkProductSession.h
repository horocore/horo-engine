#pragma once
/** @file NetworkProductSession.h @brief Private application session lifetime, clock and transport-event composition. */
#include "Horo/Foundation/Platform.h"
#include "Horo/Network/InboundMessageDispatcher.h"
#include "Horo/Network/NetworkAdmissionController.h"
#include "Horo/Network/NetworkModeComposition.h"

#include <memory>
#include <optional>
#include <thread>

namespace Horo::Application::Internal {
    class ModeParticipant;
    class NetworkProductHost;
    class ClientSessionOwner;

    /**
     * @brief Explicit simulation-thread clock domain shared by admission, ingress and expiry.
     * @details Construct and use on one thread. The retained source must be monotonic and use the
     * same epoch/quantum as the canonical lifecycle's absolute deadlines. A backwards sample
     * permanently invalidates the domain. No frame counter or implicit wall clock supplies ticks.
     */
    class NetworkProductSessionClock final {
    public:
        /**
         * @brief Retain a stable monotonic source with one-based integer ticks since an explicit epoch.
         * @param source Owned shared source, retained until the last clock-domain owner retires.
         * @param tickDuration Strictly positive duration represented by one session tick.
         * @param epoch Nonnegative source-relative epoch, no later than any sampled time.
         */
        NetworkProductSessionClock(std::shared_ptr<Clock> source, Duration tickDuration, Duration epoch);
        /**
         * @brief Read the exact tick published before this NetworkPoll without sampling again.
         * @return Published tick, or a typed wrong-thread, rollback or unpublished-domain error.
         */
        [[nodiscard]] Result<std::uint64_t> PollTick() const;
        /**
         * @brief Validate immutable configuration without sampling or invoking host services.
         * @return Success for a valid owner-thread source, quantum and epoch; typed failure otherwise.
         */
        [[nodiscard]] Result<void> Validate() const;

    private:
        friend class ClientSessionOwner;
        /** @brief Sample and latch monotonic rollback; return the one-based session tick or typed error. */
        [[nodiscard]] Result<std::uint64_t> Sample();
        /** @brief Publish one successful sample for all ingress work in the next transport poll. */
        [[nodiscard]] Result<std::uint64_t> BeginPoll();
        std::shared_ptr<Clock> source_;
        Duration tickDuration_;
        Duration epoch_;
        std::optional<Duration> lastSample_;
        std::uint64_t pollTick_{};
        std::optional<Error> failure_;
        std::thread::id owner_{std::this_thread::get_id()};
    };

    /** @brief Original terminal of an actual outbound transport attempt; never a peer, principal or session generation. */
    struct ClientConnectionAttemptTerminal final {
        Network::ConnectionHandle connection{};
        const ErrorCodeDescriptor *failure{}; /**< Stable transport error descriptor; no borrowed native text or allocated payload. */
    };

    /**
     * @brief One application-owned client lifecycle lease and transport terminal observer.
     * @details All methods run on the simulation thread. The owner retains the exact admitted
     * canonical lifecycle; it never copies authentication/session state. One mode and one borrowed
     * transport bind for this owner's lifetime. Detach precedes transport destruction and permanently
     * retires the binding; restart requires a fresh owner. Standalone hosts use a null clock and
     * cannot admit a network peer. Shutdown precedes service teardown so borrowed lifecycle
     * diagnostics remain alive while the original terminal is recorded.
     */
    class ClientSessionOwner final : public Network::IInboundSessionHost, public Network::INetworkAdmissionObserver {
    public:
        /**
         * @brief Retain the domain selected by the composition root before transport factories run.
         * @param clock Shared explicit domain; null is reserved for standalone composition.
         */
        explicit ClientSessionOwner(std::shared_ptr<NetworkProductSessionClock> clock);
        /**
         * @brief Require a configured clock and an unused owner before network host composition.
         * @return Success for an unused valid owner; typed configuration or stale-owner error otherwise.
         */
        [[nodiscard]] Result<void> Validate() const;
        /**
         * @brief Bind the transport used by the same inbound dispatcher, including partial startup.
         * @param transport Borrowed transport; the service must detach before shutdown/destruction.
         * @return Success for the exact live binding; typed stale-owner error on conflicting/reused binding.
         */
        [[nodiscard]] Result<void> AttachTransport(Network::INetworkTransport &transport);
        /**
         * @brief Retain the exact successful Connect result before the first owner PollEvents.
         * @param connection Actual accepted transport handle, including its generation.
         * @return Success once, or typed overlap/lifecycle error. No peer/session is created.
         * @details Connect queues events; only PollEvents may deliver them. Service activation hands off
         * synchronously before polling. A failed handoff rolls back through normal service shutdown.
         */
        [[nodiscard]] Result<void> BeginConnectionAttempt(Network::ConnectionHandle connection);
        /** @brief Borrow the first original attempt terminal through Shutdown, ending at owner destruction; null before termination. */
        [[nodiscard]] const ClientConnectionAttemptTerminal *ConnectionAttemptTerminal() const noexcept;
        /**
         * @brief Terminalize and detach the exact binding before transport destruction.
         * @param transport Exact borrowed transport; other or already detached bindings are ignored.
         */
        void DetachTransport(const Network::INetworkTransport &transport) noexcept;
        /**
         * @brief Borrow the one clock tick published before the current transport poll.
         * @return Published tick or typed shutdown, clock or wrong-thread error.
         */
        [[nodiscard]] Result<std::uint64_t> PollTick() const;
        /**
         * @brief Reconcile exact-generation transport terminals; connectivity never grants authentication.
         * @param event Borrowed dispatcher event, consumed synchronously after native polling.
         * @details Unknown connectivity is closed. Stale handles cannot revoke a replacement peer.
         * Existing canonical remote-close reasons survive later transport failure and shutdown.
         */
        void OnTransportEvent(const Network::NetworkTransportEvent &event) noexcept override;

        /** @brief Borrow the stable canonical world owner; valid across permitted composition moves. */
        [[nodiscard]] std::weak_ptr<Network::NetworkWorldBindingController> WorldBinding() const noexcept {
            return binding_;
        }

        /** @brief Retain one exact pre-active runtime owner for terminal observation without granting client gameplay authority. */
        [[nodiscard]] Result<void> OnSessionPrepared(Network::AdmissionEndpoint endpoint,
                                                     const std::shared_ptr<Network::PeerSessionLifecycle> &lifecycle,
                                                     Network::NetworkOperationGeneration generation, std::uint64_t nowTick) override;
        /** @brief Project a runtime-controller-issued active client using the exact published poll tick; server peers retain server
         * authority only. */
        [[nodiscard]] Result<void> OnSessionActivated(Network::AdmissionEndpoint endpoint,
                                                      const std::shared_ptr<Network::PeerSessionLifecycle> &lifecycle,
                                                      Network::NetworkOperationGeneration generation, std::uint64_t nowTick) override;

    private:
        friend class ModeParticipant;
        friend class SessionBoundaryService;
        friend class NetworkProductHost;
        /** @brief Bind the canonical mode, whose lifetime contains this session owner. */
        void Bind(Network::NetworkModeComposition &mode) noexcept;
        /** @brief Retain and admit one exact active lifecycle using the shared clock; replacement needs a new connection handle. */
        [[nodiscard]] Result<void> Admit(Network::NetworkModeWorldKind world, std::shared_ptr<Network::PeerSessionLifecycle> peer,
                                         Network::ConnectionHandle connection, Network::NetworkOperationGeneration generation,
                                         std::optional<std::uint64_t> publishedTick = std::nullopt);
        /** @brief Expire the exact lifecycle before transport ingress can dispatch packets. */
        [[nodiscard]] Result<void> BeginPoll();
        /** @brief Revoke gameplay authority after transport/session terminals and before world callbacks. */
        [[nodiscard]] Result<void> Reconcile();
        /** @brief Cancel only the exact client generation and revoke gameplay authority. */
        [[nodiscard]] Result<void> Disconnect(Network::NetworkModeWorldKind world, Network::NetworkOperationGeneration generation);
        /** @brief Borrow the original retained canonical terminal until replacement or destruction. */
        [[nodiscard]] const Network::PeerSessionTerminalSnapshot *Terminal() const noexcept;
        /** @brief Revoke and terminalize before service teardown; repeated shutdown preserves the original terminal. */
        void Shutdown() noexcept;
        /** @brief Clear admitted gameplay authority in the canonical mode without replacing lifecycle state. */
        [[nodiscard]] Result<void> Revoke();
        /** @brief Record a canonical transport failure while preserving any previously pinned close reason. */
        void TransportEnded(Network::NetworkFailureKind failure = Network::NetworkFailureKind::TransportUnavailable) noexcept;
        /** @brief Consume one exact pending attempt without allocating or changing a peer lifecycle. */
        void EndConnectionAttempt(const ErrorCodeDescriptor &failure) noexcept;
        /** @brief Sample the shared domain and fail closed on owner-thread clock rollback. */
        [[nodiscard]] Result<std::uint64_t> Sample(bool poll);
        std::shared_ptr<NetworkProductSessionClock> clock_;
        std::shared_ptr<Network::PeerSessionLifecycle> peer_;
        Network::NetworkModeComposition *mode_{};
        std::weak_ptr<Network::NetworkWorldBindingController> binding_;
        Network::INetworkTransport *transport_{};  // Borrow ends explicitly before transport shutdown/destruction.
        Network::ConnectionHandle connection_{};
        Network::ConnectionHandle pendingConnection_{};
        std::optional<ClientConnectionAttemptTerminal> connectionTerminal_;
        Network::NetworkOperationGeneration generation_{};
        std::uint64_t lastTick_{};
        bool admitted_{};
        bool stopped_{};
        bool ingressFailed_{};
        bool transportRetired_{};
    };
}  // namespace Horo::Application::Internal
