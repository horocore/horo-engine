#pragma once
/** @file NetworkAdmissionController.h @brief Bounded canonical runtime admission over host-selected control identities. */

#include "Horo/Network/AdmissionProtection.h"
#include "Horo/Network/InboundMessageDispatcher.h"
#include "Horo/Network/MessageCodecRegistry.h"
#include "Horo/Network/PeerSessionLifecycle.h"
#include "Horo/Security/SecureMemory.h"

#include <array>
#include <memory>

namespace Horo::Network {
    /** @brief Closed version-one admission payload kinds; message identities remain product-owned. */
    enum class AdmissionControlKind : std::uint8_t {
        Offer,
        Challenge,
        Proof,
        Acknowledgement,
        Count
    };

    /** @brief Trusted connection direction selected by transport events, never by peer payload. */
    enum class AdmissionEndpoint : std::uint8_t {
        Client,
        Server,
        Count
    };

    /** @brief Peer-local event outcome; rejection is not an infrastructure/frame failure. */
    enum class AdmissionEventDisposition : std::uint8_t {
        Ignored,
        PendingCreated,
        PeerRejected,
        CandidateCancelled,
        Terminalized
    };

    /** @brief Relative finite session limits, converted once to checked absolute owner-clock deadlines. */
    struct NetworkAdmissionTimeouts final {
        std::uint64_t negotiationTicks{};
        std::uint64_t authenticationTicks{};
        std::uint64_t activationTicks{};
        std::uint64_t lifetimeTicks{};
        std::uint64_t inactivityTicks{};
    };

    /**
     * @brief Inert host policy copied before any listener opens.
     * @details Create copies local feature spans and channel policies. Control identities must be
     * distinct mandatory entries of the same canonical product protocol/codec inventory used by
     * the dispatcher. No reserved global IDs or additional transport channel are introduced.
     */
    struct NetworkAdmissionPolicy final {
        HandshakeLocalDescriptor local;
        TransportRequirements requirements; /**< Explicit requirements emitted in this endpoint's offer. */
        std::array<bool, static_cast<std::size_t>(AdmissionEndpoint::Count)> endpoints{};
        NetworkTrustPolicySnapshot trust;
        AdmissionProtectionPolicy protection;
        AdmissionSourceId sourceBucket; /**< Trusted conservative source group; all peers share this host-selected bucket. */
        NetworkAdmissionTimeouts timeouts;
        std::array<MessageIdentityDescriptor, static_cast<std::size_t>(AdmissionControlKind::Count)> controls;
        std::span<const MessageDeliveryChannel> gameplayChannels;
        MessageEnvelopeLimits envelope;
        std::size_t maximumPeers{};                /**< Positive ceiling no larger than MaximumProtectedAdmissions. */
        std::size_t maximumControlFramesPerPoll{}; /**< Finite global parse/provider work bound. */
        std::size_t maximumControlBytesPerPoll{};  /**< Charged before any envelope or payload parsing. */
    };

    /**
     * @brief Public peer-challenge context already validated against this connection's negotiation/nonces.
     * @details The opaque owner transcript digest includes the OTHER endpoint's local fences and must
     * never be reconstructed with this endpoint's handles. No local transport handle is serialized.
     */
    struct AdmissionProofRequest final {
        ProofIssuanceAuthorization authorization;
        HandshakeSelection localSelection;
        std::uint64_t nowTick{};
    };

    /** @brief Host-injected bounded synchronous proof issuance; no credential is selected from project or peer data. */
    class IAdmissionProofSource {
    public:
        virtual ~IAdmissionProofSource() = default;
        /**
         * @brief Validate the exact context and issue a short-lived proof without blocking I/O.
         * @param request Generation/deadline-bound public request, valid only during this call.
         * @return Move-only securely cleared proof or typed private failure; no ordinary payload copy.
         * @details Must perform bounded owner-thread work. A source requiring asynchronous I/O must
         * not be installed through this synchronous capability. Provider errors are sanitized at runtime.
         */
        [[nodiscard]] virtual Result<Security::SecureBytes> Issue(const AdmissionProofRequest &request) = 0;
    };

    /** @brief Host authority normalizing actual secure-channel evidence without exposing native handles. */
    class IAdmissionChannelEvidence {
    public:
        virtual ~IAdmissionChannelEvidence() = default;
        /**
         * @brief Read evidence for the exact live channel generation without blocking or selecting trust.
         * @param connection Owner-local live transport handle.
         * @param generation Exact local admission generation.
         * @param nowTick Published monotonic owner tick.
         * @return Typed non-secret evidence, or private provider failure sanitized by the controller.
         */
        [[nodiscard]] virtual Result<PeerAuthenticationEvidence> Inspect(ConnectionHandle connection, NetworkOperationGeneration generation,
                                                                         std::uint64_t nowTick) = 0;
    };

    /** @brief Retained host authorities; borrowed adapter pointers cannot outlive these owners. */
    struct NetworkAdmissionProviders final {
        std::shared_ptr<ICertificateAuthority> certificates;
        std::shared_ptr<IPeerVerificationAuthority> peers;
        std::shared_ptr<ICredentialAuthority> credentials;
        std::shared_ptr<IPrivateKeyAuthority> privateKeys;
        std::shared_ptr<Security::SecureRandomSource> random;
        std::shared_ptr<IAdmissionProofSource> proofs;
        std::shared_ptr<IAdmissionChannelEvidence> channel;
    };

    /** @brief Application projection of canonical admission, never a source of authentication authority. */
    class INetworkAdmissionObserver {
    public:
        virtual ~INetworkAdmissionObserver() = default;
        /**
         * @brief Retain the same pre-active canonical owner for terminal observability without granting any role.
         * @param endpoint Trusted local connection direction.
         * @param lifecycle Exact newly negotiating runtime-owned lifecycle.
         * @param generation Exact local admission generation.
         * @param nowTick Published shared owner-clock tick.
         * @return Lease projection success or typed overlap/stale denial; denial closes only this candidate.
         */
        [[nodiscard]] virtual Result<void> OnSessionPrepared(AdmissionEndpoint endpoint,
                                                             const std::shared_ptr<PeerSessionLifecycle> &lifecycle,
                                                             NetworkOperationGeneration generation, std::uint64_t nowTick) = 0;
        /**
         * @brief Retain the exact lifecycle already authenticated and registered by the runtime controller.
         * @param endpoint Trusted local connection direction.
         * @param lifecycle Same shared canonical owner installed in the dispatcher.
         * @param generation Exact local session generation.
         * @param nowTick Shared monotonic admission/poll tick.
         * @return Application role admission or typed denial; denial revokes the runtime registration.
         */
        [[nodiscard]] virtual Result<void> OnSessionActivated(AdmissionEndpoint endpoint,
                                                              const std::shared_ptr<PeerSessionLifecycle> &lifecycle,
                                                              NetworkOperationGeneration generation, std::uint64_t nowTick) = 0;
    };

    /**
     * @brief Sole bounded runtime owner of pending negotiation, authentication and session activation.
     * @details Construct before native listeners. All operations run on the construction thread and
     * share the product's explicit monotonic tick domain. Transport/dispatcher borrows outlive this
     * controller; Shutdown precedes dispatcher shutdown and transport detach. Dispatcher retains only
     * a weak controller lease. Fixed peer slots and finite frame/byte work limits bound pre-active work.
     */
    class NetworkAdmissionController final {
        struct Impl;

        class ConstructionKey final {
            friend class NetworkAdmissionController;

            ConstructionKey() noexcept {}
        };

    public:
        /** @brief Factory-only standard shared construction after validation.
         * @param key Nonaggregate private factory authority. @param impl Fully validated owned state. */
        explicit NetworkAdmissionController(ConstructionKey key, std::unique_ptr<Impl> impl) noexcept;
        /**
         * @brief Validate/copy host policy and retain providers before opening any native listener.
         * @param transport Borrowed exact transport owned by the composition root.
         * @param dispatcher Borrowed router sharing the canonical codec snapshot.
         * @param codecs Retained immutable product codec inventory, identical to the router's snapshot.
         * @param policy Inert trusted configuration; borrowed spans are copied synchronously.
         * @param providers Owned authority lifetimes with no default credentials or trust fallback.
         * @param observer Weak application role projection, optional for server-only composition.
         * @return Runtime controller or typed invalid/capacity/provider configuration error.
         */
        [[nodiscard]] static Result<std::shared_ptr<NetworkAdmissionController>> Create(
            INetworkTransport &transport, InboundMessageDispatcher &dispatcher, std::shared_ptr<const MessageCodecRegistry> codecs,
            const NetworkAdmissionPolicy &policy, NetworkAdmissionProviders providers,
            std::weak_ptr<INetworkAdmissionObserver> observer = {});
        ~NetworkAdmissionController();
        NetworkAdmissionController(const NetworkAdmissionController &) = delete;
        NetworkAdmissionController &operator=(const NetworkAdmissionController &) = delete;
        /** @brief Start a bounded owner-clock poll, expire pending/active peers and publish parse budgets. @return Typed lifecycle result.
         */
        [[nodiscard]] Result<void> BeginPoll(std::uint64_t nowTick);
        /** @brief Consume a real non-packet transport event after native polling. @return Typed safe result. */
        [[nodiscard]] Result<AdmissionEventDisposition> OnTransportEvent(const NetworkTransportEvent &event, std::uint64_t nowTick);
        /** @brief Consume only pre-active reserved control frames; early gameplay is rejected. @return Typed safe admission result. */
        [[nodiscard]] Result<void> ConsumeControl(const NetworkTransportEvent &event, std::uint64_t nowTick);
        /** @brief Cancel every owned generation with the canonical local-cancellation reason before teardown. */
        void Cancel(std::uint64_t nowTick) noexcept;
        /** @brief Record a canonical transport-wide failure while preserving every prior terminal/close reason.
         * @param failure TransportUnavailable, NameResolutionFailed or TransportSaturated. @param nowTick Last valid owner tick. */
        void FailTransport(NetworkFailureKind failure, std::uint64_t nowTick) noexcept;
        /** @brief Terminalize all generations exactly once before provider/transport retirement. */
        void Shutdown(std::uint64_t nowTick) noexcept;

    private:
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Network
