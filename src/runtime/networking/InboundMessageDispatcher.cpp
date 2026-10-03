#include "Horo/Network/InboundMessageDispatcher.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace Horo::Network {
    struct InboundMessageDispatcher::Session final {
        Session(std::shared_ptr<PeerSessionLifecycle> owner, const ConnectionHandle handle,
                const NetworkOperationGeneration sessionGeneration, MessageDeliveryGate delivery,
                const CancellationToken &parentCancellation)
            : lifecycle(std::move(owner)), connection(handle), generation(sessionGeneration), gate(std::move(delivery)),
              cancellation(parentCancellation) {}

        std::shared_ptr<PeerSessionLifecycle> lifecycle;
        ConnectionHandle connection;
        NetworkOperationGeneration generation;
        MessageDeliveryGate gate;
        CancellationSource cancellation;
    };

    struct InboundMessageDispatcher::Handler final {
        struct RateState final {
            ConnectionHandle connection{};
            std::uint64_t countedTick{};
            std::uint32_t count{};
        };

        Handler(const InboundHandlerBinding &handlerBinding, const std::shared_ptr<IInboundMessageHandler> &handlerOwner,
                const std::size_t maximumSessions)
            : binding(handlerBinding), owner(handlerOwner), rates(maximumSessions) {}

        InboundHandlerBinding binding;
        std::weak_ptr<IInboundMessageHandler> owner;
        std::vector<RateState> rates;
    };

    /** @copydoc InboundMessageDispatcher::InboundMessageDispatcher */
    InboundMessageDispatcher::InboundMessageDispatcher(ConstructionKey, INetworkTransport &transport, const MessageCodecRegistry &codecs,
                                                       const InboundDispatchLimits &limits)
        : transport_(transport), codecs_(codecs), limits_(limits), owner_(std::this_thread::get_id()), queue_(limits.maximumQueuedPackets) {
        sessions_.reserve(limits.maximumSessions);
        handlers_.reserve(limits.maximumHandlers);
    }

    /** @copydoc InboundMessageDispatcher::~InboundMessageDispatcher */
    InboundMessageDispatcher::~InboundMessageDispatcher() {
        Shutdown();
    }

    /** @copydoc InboundMessageDispatcher::Create */
    Result<std::unique_ptr<InboundMessageDispatcher>> InboundMessageDispatcher::Create(INetworkTransport &transport,
                                                                                       const MessageCodecRegistry &codecs,
                                                                                       const InboundDispatchLimits &limits) {
        if (limits.maximumSessions == 0 || limits.maximumSessions > 4096 || limits.maximumHandlers == 0 || limits.maximumHandlers > 4096 ||
            limits.maximumQueuedPackets == 0 || limits.maximumQueuedPackets > 4096 || limits.maximumPacketsPerPoll == 0 ||
            limits.maximumPacketsPerPoll > limits.maximumQueuedPackets || limits.envelope.maximumFrameBytes == 0)
            return Result<std::unique_ptr<InboundMessageDispatcher>>::Failure(MakeError(NetworkErrors::NetworkIoServiceInvalid));
        try {
            auto created = std::make_unique<InboundMessageDispatcher>(ConstructionKey{}, transport, codecs, limits);
            return Result<std::unique_ptr<InboundMessageDispatcher>>::Success(std::move(created));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<InboundMessageDispatcher>>::Failure(MakeError(NetworkErrors::NetworkIoServiceCapacityExceeded));
        }
    }

    /** @copydoc InboundMessageDispatcher::CheckOwner */
    Result<void> InboundMessageDispatcher::CheckOwner() const {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkIoWrongThread));
        if (shuttingDown_)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionShuttingDown));
        return Result<void>::Success();
    }

    /** @copydoc InboundMessageDispatcher::RegisterSession */
    Result<void> InboundMessageDispatcher::RegisterSession(std::shared_ptr<PeerSessionLifecycle> session, const ConnectionHandle connection,
                                                           const NetworkOperationGeneration generation, MessageDeliveryGate gate,
                                                           const std::uint64_t nowTick, const CancellationToken &parentCancellation) {
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (session == nullptr || !connection.IsValid() || !generation.IsValid())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleInvalid));
        if (parentCancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(NetworkErrors::SessionCancelled));
        if (const auto active = session->AdmitGameplay(connection, generation, nowTick); active.HasError())
            return active;
        if (session->Negotiation() == nullptr || !gate.Matches(connection, generation, session->Negotiation()->transport))
            return Result<void>::Failure(MakeError(NetworkErrors::MessageDeliveryInvalid));
        if (std::ranges::any_of(sessions_, [connection](const auto &entry) {
            return entry->connection == connection;
        }))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleOperationStale));
        if (sessions_.size() == limits_.maximumSessions)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkIoServiceCapacityExceeded));
        try {
            sessions_.push_back(std::make_shared<Session>(std::move(session), connection, generation, std::move(gate), parentCancellation));
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkIoServiceCapacityExceeded));
        }
        return Result<void>::Success();
    }

    /** @copydoc InboundMessageDispatcher::RevokeSession */
    void InboundMessageDispatcher::RevokeSession(const ConnectionHandle connection) noexcept {
        if (std::this_thread::get_id() != owner_)
            return;
        for (const auto &entry : sessions_)
            if (entry->connection == connection)
                entry->cancellation.RequestCancellation();
        for (const auto &entry : sessions_)
            if (entry->connection == connection)
                entry->gate.Shutdown();
        std::erase_if(sessions_, [connection](const auto &entry) {
            return entry->connection == connection;
        });
        for (auto &handler : handlers_)
            for (auto &rate : handler.rates)
                if (rate.connection == connection)
                    rate = {};
        // Queued packets retain bytes, not authority. A later drain still rejects this generation.
    }

    /** @copydoc InboundMessageDispatcher::RegisterHandler */
    Result<void> InboundMessageDispatcher::RegisterHandler(const InboundHandlerBinding &binding,
                                                           const std::shared_ptr<IInboundMessageHandler> &handler) {
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (handler == nullptr || !binding.protocol.IsValid() || !binding.message.IsValid() ||
            binding.traffic >= MessageTrafficClass::Count || binding.phase != Runtime::RuntimePhase::NetworkPoll ||
            binding.maximumPerTick == 0)
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        if (!codecs_.FindCodec(binding.protocol, binding.message).has_value())
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        if (std::ranges::any_of(handlers_, [&](const Handler &existing) {
            return existing.binding.protocol == binding.protocol && existing.binding.message == binding.message;
        }))
            return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorConflict));
        if (handlers_.size() == limits_.maximumHandlers)
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        try {
            handlers_.emplace_back(binding, handler, limits_.maximumSessions);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        }
        return Result<void>::Success();
    }

    /** @copydoc InboundMessageDispatcher::SetSessionHost */
    void InboundMessageDispatcher::SetSessionHost(const std::shared_ptr<IInboundSessionHost> &host) noexcept {
        if (std::this_thread::get_id() == owner_ && !shuttingDown_)
            sessionHost_ = host;
    }

    /** @copydoc InboundMessageDispatcher::RevokeHandler */
    void InboundMessageDispatcher::RevokeHandler(const ProtocolId protocol, const MessageTypeId message) noexcept {
        if (std::this_thread::get_id() != owner_)
            return;
        std::erase_if(handlers_, [protocol, message](const Handler &entry) {
            return entry.binding.protocol == protocol && entry.binding.message == message;
        });
    }

    /** @copydoc InboundMessageDispatcher::Consume */
    void InboundMessageDispatcher::Consume(NetworkTransportEvent event) noexcept {
        if (shuttingDown_)
            return;
        if (queued_ == queue_.size()) {
            overflowed_ = true;
            return;
        }
        queue_[queued_++] = std::move(event);
    }

    /** @copydoc InboundMessageDispatcher::Dispatch */
    Result<void> InboundMessageDispatcher::Dispatch(NetworkTransportEvent &event, const std::uint64_t nowTick) {
        using enum NetworkTransportEventKind;
        if (event.kind > Failed)
            return RejectAndClose(event.connection, MakeError(NetworkErrors::TransportMalformedPacket));
        if (event.kind == PacketReceived)
            return DispatchPacket(event, nowTick);
        if (event.kind == Closed || event.kind == Failed)
            RevokeSession(event.connection);
        if (auto host = sessionHost_.lock())
            host->OnTransportEvent(std::move(event));
        else if (event.kind == Accepted || event.kind == Connected)
            return transport_.Close(event.connection);  // No host trust authority; fail closed.
        return Result<void>::Success();
    }

    /** @brief Revokes one generation and closes its transport before returning a typed denial. */
    Result<void> InboundMessageDispatcher::RejectAndClose(const ConnectionHandle connection, Error error) {
        RevokeSession(connection);
        static_cast<void>(transport_.Close(connection));
        return Result<void>::Failure(std::move(error));
    }

    /** @brief Pins an active host-admitted session through one packet invocation. */
    Result<std::shared_ptr<InboundMessageDispatcher::Session>> InboundMessageDispatcher::AdmitSession(const ConnectionHandle connection,
                                                                                                      const std::uint64_t nowTick) {
        const auto session = std::ranges::find_if(sessions_, [&](const auto &entry) {
            return entry->connection == connection;
        });
        if (session == sessions_.end()) {
            static_cast<void>(transport_.Close(connection));
            return Result<std::shared_ptr<Session>>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        }
        const auto pinnedSession = *session;
        if (pinnedSession->cancellation.Token().IsCancellationRequested()) {
            RevokeSession(connection);
            static_cast<void>(transport_.Close(connection));
            return Result<std::shared_ptr<Session>>::Failure(MakeError(NetworkErrors::SessionCancelled));
        }
        if (const auto admitted = pinnedSession->lifecycle->AdmitGameplay(connection, pinnedSession->generation, nowTick);
            admitted.HasError()) {
            RevokeSession(connection);
            static_cast<void>(transport_.Close(connection));
            return Result<std::shared_ptr<Session>>::Failure(admitted.ErrorValue());
        }
        return Result<std::shared_ptr<Session>>::Success(pinnedSession);
    }

    /** @copydoc InboundMessageDispatcher::DispatchPacket */
    Result<void> InboundMessageDispatcher::DispatchPacket(NetworkTransportEvent &event, const std::uint64_t nowTick) {
        auto session = AdmitSession(event.connection, nowTick);
        if (session.HasError())
            return Result<void>::Failure(std::move(session).ErrorValue());
        auto decoded = DecodeMessageEnvelope(event.payload, codecs_, limits_.envelope);
        if (decoded.HasError())
            return RejectAndClose(event.connection, std::move(decoded).ErrorValue());
        const auto &message = decoded.Value();
        if (const auto *negotiation = session.Value()->lifecycle->Negotiation();
            negotiation == nullptr || message.protocol != negotiation->protocol)
            return RejectAndClose(event.connection, MakeError(NetworkErrors::GameplayDispatchRejected));
        const auto handler = std::ranges::find_if(handlers_, [&](const Handler &entry) {
            return entry.binding.protocol == message.protocol && entry.binding.message == message.message;
        });
        if (handler == handlers_.end())
            return RejectAndClose(event.connection, MakeError(NetworkErrors::GameplayDispatchRejected));
        return InvokeHandler(session.Value(), *handler, event, message, nowTick);
    }

    /** @copydoc InboundMessageDispatcher::InvokeHandler */
    Result<void> InboundMessageDispatcher::InvokeHandler(const std::shared_ptr<Session> &session, Handler &handler,
                                                         const NetworkTransportEvent &event, const MessageEnvelope &message,
                                                         const std::uint64_t nowTick) {
        const auto bound = handler.owner.lock();
        if (bound == nullptr)
            return RejectAndClose(event.connection, MakeError(NetworkErrors::GameplayDispatchRejected));
        auto rate = std::ranges::find_if(handler.rates, [&](const Handler::RateState &entry) {
            return entry.connection == event.connection;
        });
        if (rate == handler.rates.end()) {
            rate = std::ranges::find_if(handler.rates, [](const Handler::RateState &entry) {
                return !entry.connection.IsValid();
            });
            if (rate == handler.rates.end())
                return RejectAndClose(event.connection, MakeError(NetworkErrors::RpcCapacityExceeded));
            rate->connection = event.connection;
        }
        if (rate->countedTick != nowTick) {
            rate->countedTick = nowTick;
            rate->count = 0;
        }
        if (rate->count >= handler.binding.maximumPerTick)
            return RejectAndClose(event.connection, MakeError(NetworkErrors::TransportReliableBackpressure));
        const MessageDeliveryInput
            input{event.connection,    session->generation, event.channel, event.delivery, handler.binding.traffic, message.sequence, 0,
                  event.payload.size()};
        try {
            // Apply invokes synchronously; pin both owners while the decoded message and rate slot are borrowed.
            return session->gate.Apply(input, nowTick,
                                       [rate, &message, bound, session, connection = event.connection, generation = session->generation,
                                        nowTick, delivery = event.delivery] {
                ++rate->count;
                if (auto activity = session->lifecycle->RecordActivity(connection, generation, nowTick); activity.HasError())
                    return activity;
                return bound->HandleAdmitted({connection, generation, session->lifecycle, nowTick, session->cancellation.Token(), delivery},
                                             message);
            });
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkIoServiceCapacityExceeded));
        } catch (const std::invalid_argument &) {
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        } catch (const std::out_of_range &) {
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        } catch (...) {
            // Untrusted handlers may throw non-standard values; none may cross the admitted owner boundary.
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        }
    }

    /** @copydoc InboundMessageDispatcher::RunNetworkPoll */
    Result<InboundDispatchReport> InboundMessageDispatcher::RunNetworkPoll(const std::uint64_t nowTick,
                                                                           const CancellationToken &cancellation) {
        if (const auto owner = CheckOwner(); owner.HasError())
            return Result<InboundDispatchReport>::Failure(owner.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<InboundDispatchReport>::Failure(MakeError(NetworkErrors::SessionCancelled));
        if (nowTick == 0 || nowTick < lastTick_)
            return Result<InboundDispatchReport>::Failure(MakeError(NetworkErrors::MessageDeliveryInvalid));
        lastTick_ = nowTick;
        if (const auto polled = transport_.PollEvents(*this); polled.HasError()) {
            Shutdown();  // A partial native poll cannot leave staged events or authority live.
            transport_.Shutdown();
            return Result<InboundDispatchReport>::Failure(polled.ErrorValue());
        }
        if (overflowed_) {
            Shutdown();  // Unknown number of dropped native events: fail closed across all sessions.
            transport_.Shutdown();
            return Result<InboundDispatchReport>::Failure(MakeError(NetworkErrors::NetworkIoCompletionQueueFull));
        }
        const std::size_t count = std::min(queued_, limits_.maximumPacketsPerPoll);
        InboundDispatchReport report;
        for (std::size_t index = 0; index < count; ++index) {
            const bool packet = queue_[index].kind == NetworkTransportEventKind::PacketReceived;
            auto delivered = Dispatch(queue_[index], nowTick);
            if (shuttingDown_)
                return Result<InboundDispatchReport>::Failure(MakeError(NetworkErrors::SessionShuttingDown));
            if (delivered.HasError()) {
                if (packet) {
                    ++report.rejectedPackets;
                    report.lastPacketRejection.emplace(std::move(delivered).ErrorValue());
                    continue;
                }
                DiscardQueuedPrefix(index + 1);
                return Result<InboundDispatchReport>::Failure(delivered.ErrorValue());
            }
        }
        DiscardQueuedPrefix(count);
        report.processedEvents = count;
        return Result<InboundDispatchReport>::Success(std::move(report));
    }

    /** @brief Preserves queued suffix order and releases every consumed payload. */
    void InboundMessageDispatcher::DiscardQueuedPrefix(const std::size_t count) noexcept {
        const auto oldQueued = queued_;
        for (std::size_t index = count; index < oldQueued; ++index)
            queue_[index - count] = std::move(queue_[index]);
        queued_ -= count;
        for (std::size_t stale = queued_; stale < oldQueued; ++stale)
            queue_[stale] = {};
    }

    /** @copydoc InboundMessageDispatcher::Shutdown */
    void InboundMessageDispatcher::Shutdown() noexcept {
        if (shuttingDown_)
            return;
        shuttingDown_ = true;
        for (const auto &session : sessions_)
            session->cancellation.RequestCancellation();
        for (const auto &session : sessions_)
            session->gate.Shutdown();
        sessions_.clear();
        handlers_.clear();
        sessionHost_.reset();
        for (std::size_t index = 0; index < queued_; ++index)
            queue_[index] = {};
        queued_ = 0;
    }
}  // namespace Horo::Network
