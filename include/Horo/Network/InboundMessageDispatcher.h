#pragma once

/**
 * @file InboundMessageDispatcher.h
 * @brief Owner-thread transport-to-typed-handler routing for admitted sessions.
 */

#include "Horo/Network/MessageCodecRegistry.h"
#include "Horo/Network/MessageDeliveryGate.h"
#include "Horo/Network/NetworkTransport.h"
#include "Horo/Network/PeerSessionLifecycle.h"
#include "Horo/Runtime/FrameScheduler.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace Horo::Network {
    /** @brief Admission evidence derived from the registered session, never from packet payload. */
    struct InboundMessageContext final {
        ConnectionHandle connection;
        NetworkOperationGeneration generation;
        std::shared_ptr<PeerSessionLifecycle> session;
        std::uint64_t ownerTick{};
        CancellationToken cancellation;
        DeliveryPolicy delivery{DeliveryPolicy::ReliableOrdered}; /**< Admitted transport delivery class, never payload-derived. */
    };

    /** @brief Finite owner-owned storage and work budgets. */
    struct InboundDispatchLimits final {
        std::size_t maximumSessions{64};       /**< Concurrent admitted sessions. */
        std::size_t maximumHandlers{128};      /**< Exact typed handler bindings. */
        std::size_t maximumQueuedPackets{256}; /**< Packets staged per NetworkPoll phase. */
        std::size_t maximumPacketsPerPoll{64}; /**< Owner work budget; not a native I/O callback budget. */
        MessageEnvelopeLimits envelope{};      /**< Validated framing and payload bounds. */
    };

    /** @brief Application-owned typed handler; the dispatcher retains only a weak lease. */
    class IInboundMessageHandler {
    public:
        virtual ~IInboundMessageHandler() = default;
        /** @brief Handles one fully admitted owned message on the owner thread. @return Typed application result. */
        [[nodiscard]] virtual Result<void> Handle(const MessageEnvelope &message) = 0;

        /** @brief Receives trusted transport/session evidence when the handler needs sender authority. */
        [[nodiscard]] virtual Result<void> HandleAdmitted(const InboundMessageContext &, const MessageEnvelope &message) {
            return Handle(message);
        }
    };

    /** @brief Host-owned trust/admission observer invoked only after owner-thread transport polling returns. */
    class IInboundSessionHost {
    public:
        virtual ~IInboundSessionHost() = default;
        /** @brief Handles a non-packet transport event; connectivity alone must not grant gameplay admission. */
        virtual void OnTransportEvent(NetworkTransportEvent event) noexcept = 0;
    };

    /** @brief Exact binding of one accepted message class to one owner and phase. */
    struct InboundHandlerBinding final {
        ProtocolId protocol{};                                           /**< Exact accepted protocol. */
        MessageTypeId message{};                                         /**< Exact accepted message type. */
        MessageTrafficClass traffic{MessageTrafficClass::Count};         /**< Required delivery lane purpose. */
        Runtime::RuntimePhase phase{Runtime::RuntimePhase::NetworkPoll}; /**< Required owner phase. */
        std::uint32_t maximumPerTick{};                                  /**< Positive per-owner-clock rate ceiling. */
    };

    /** @brief Bounded owner-poll outcome; untrusted packet rejection is not a host-frame failure. */
    struct InboundDispatchReport final {
        std::size_t processedEvents{};            /**< Number of staged events consumed. */
        std::size_t rejectedPackets{};            /**< Packets denied before gameplay invocation. */
        std::optional<Error> lastPacketRejection; /**< Last stable typed denial for owner diagnostics. */
    };

    /**
     * @brief Host-composed owner-thread packet router above a unique host-owned transport.
     *
     * The host installs only already-Active, trusted sessions. This router never authenticates peers or
     * grants a principal. It borrows the transport and codec snapshot; both must outlive it. PollEvents
     * only stages owned events. Decoding, session admission, delivery replay checks and application calls
     * occur after PollEvents returns on the construction thread. Shutdown/revocation discard queued work.
     */
    class InboundMessageDispatcher final : private INetworkTransportEventConsumer {
    public:
        /** @brief Binds the owner thread and preallocates bounded work storage. @return Router or typed limit failure. */
        [[nodiscard]] static Result<std::unique_ptr<InboundMessageDispatcher>> Create(INetworkTransport &transport,
                                                                                      const MessageCodecRegistry &codecs,
                                                                                      const InboundDispatchLimits &limits = {});
        ~InboundMessageDispatcher() override;
        InboundMessageDispatcher(const InboundMessageDispatcher &) = delete;
        InboundMessageDispatcher &operator=(const InboundMessageDispatcher &) = delete;

        /** @brief Installs one host-admitted active session and its generation-matched delivery gate. @return Typed result. */
        [[nodiscard]] Result<void> RegisterSession(std::shared_ptr<PeerSessionLifecycle> session, ConnectionHandle connection,
                                                   NetworkOperationGeneration generation, MessageDeliveryGate gate, std::uint64_t nowTick,
                                                   const CancellationToken &parentCancellation = {});
        /** @brief Cancels one session before owner destruction; any queued packets become inadmissible. */
        void RevokeSession(ConnectionHandle connection) noexcept;
        /** @brief Registers one exact typed handler with a weak owner lease. @return Typed conflict or capacity result. */
        [[nodiscard]] Result<void> RegisterHandler(const InboundHandlerBinding &binding,
                                                   const std::shared_ptr<IInboundMessageHandler> &handler);
        /** @brief Borrows a host admission observer weakly; absence never admits a transport peer. */
        void SetSessionHost(const std::shared_ptr<IInboundSessionHost> &host) noexcept;
        /** @brief Revokes one exact binding; queued packets cannot invoke it afterward. */
        void RevokeHandler(ProtocolId protocol, MessageTypeId message) noexcept;
        /** @brief Polls transport then decodes and dispatches within a finite owner-thread budget. @return Report or infrastructure
         * failure. */
        [[nodiscard]] Result<InboundDispatchReport> RunNetworkPoll(std::uint64_t nowTick, const CancellationToken &cancellation = {});
        /** @brief Cancels all dispatch and releases leases and queued packets; idempotent. */
        void Shutdown() noexcept;

    private:
        struct Session;
        struct Handler;

        class ConstructionKey final {
            friend class InboundMessageDispatcher;
            ConstructionKey() = default;

        public:
            ConstructionKey(const ConstructionKey &) = default;
        };

    public:
        /** @internal Factory-only constructor; only Create can produce its admission key. */
        InboundMessageDispatcher(ConstructionKey, INetworkTransport &transport, const MessageCodecRegistry &codecs,
                                 const InboundDispatchLimits &limits);

    private:
        void Consume(NetworkTransportEvent event) noexcept override;
        [[nodiscard]] Result<void> Dispatch(NetworkTransportEvent &event, std::uint64_t nowTick);
        [[nodiscard]] Result<void> DispatchPacket(NetworkTransportEvent &event, std::uint64_t nowTick);
        [[nodiscard]] Result<std::shared_ptr<Session>> AdmitSession(ConnectionHandle connection, std::uint64_t nowTick);
        [[nodiscard]] Result<void> InvokeHandler(const std::shared_ptr<Session> &session, Handler &handler,
                                                 const NetworkTransportEvent &event, const MessageEnvelope &message, std::uint64_t nowTick);
        [[nodiscard]] Result<void> RejectAndClose(ConnectionHandle connection, Error error);
        [[nodiscard]] Result<void> CheckOwner() const;
        void DiscardQueuedPrefix(std::size_t count) noexcept;

        INetworkTransport &transport_;
        const MessageCodecRegistry &codecs_;
        InboundDispatchLimits limits_;
        std::thread::id owner_;
        std::vector<NetworkTransportEvent> queue_;
        std::vector<std::shared_ptr<Session>> sessions_;
        std::vector<Handler> handlers_;
        std::weak_ptr<IInboundSessionHost> sessionHost_;
        std::size_t queued_{};
        std::uint64_t lastTick_{};
        bool overflowed_{};
        bool shuttingDown_{};
    };
}  // namespace Horo::Network
