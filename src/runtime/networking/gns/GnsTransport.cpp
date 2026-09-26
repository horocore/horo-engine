#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Network {
    namespace {
        constexpr std::uint32_t MaximumConfiguredConnections = 4096;
        constexpr std::uint32_t MaximumConfiguredEvents = 1024;
        constexpr std::uint32_t MaximumConfiguredMessageBytes = 1200;
        constexpr unsigned MaximumConcurrentResolvers = 8;

        // The C GNS library owns a process-global default interface and dispatches
        // callbacks from RunCallbacks. The host may initialize one adapter at a time.
        std::mutex g_nativeMutex;
        GnsTransport *g_activeTransport{};

        [[nodiscard]] Result<void> Fail(const ErrorCodeDescriptor &code) {
            return Result<void>::Failure(MakeError(code));
        }
    }  // namespace

    void GnsTransport::OnStatus(SteamNetConnectionStatusChangedCallback_t *status) {
        // RunCallbacks is serialized by the active adapter's mutex. Shutdown
        // cannot clear this pointer until that call returns.
        if (g_activeTransport && status) {
            try {
                g_activeTransport->OnStatusOwned(*status);
            } catch (...) {
                // No C++ exception may unwind through GNS's C callback.
                g_activeTransport->overflow_ = true;
            }
        }
    }

    Result<void> GnsTransport::Initialize(const NetworkTransportConfig &config) {
        std::lock_guard lock(mutex_);
        if (shutdown_)
            return Fail(NetworkErrors::TransportShuttingDown);
        if (initialized_ || config.maximumConnections == 0 || config.maximumConnections > MaximumConfiguredConnections ||
            config.maximumEventsPerPoll == 0 || config.maximumEventsPerPoll > MaximumConfiguredEvents || config.maximumMessageBytes == 0 ||
            config.maximumMessageBytes > MaximumConfiguredMessageBytes)
            return Fail(NetworkErrors::TransportLimitExceeded);
        try {
            connections_.reserve(config.maximumConnections);
            resolverTasks_ = std::make_shared<std::atomic<unsigned>>(0);
        } catch (const std::bad_alloc &) {
            return Fail(NetworkErrors::TransportCapabilityUnavailable);
        }
        std::lock_guard nativeLock(g_nativeMutex);
        if (g_activeTransport)
            return Fail(NetworkErrors::TransportCapabilityUnavailable);
        SteamNetworkingErrMsg error{};
        if (!GameNetworkingSockets_Init(nullptr, error))
            return Fail(NetworkErrors::TransportNativeUnavailable);
        native_ = SteamNetworkingSockets();
        if (!native_) {
            GameNetworkingSockets_Kill();
            return Fail(NetworkErrors::TransportNativeUnavailable);
        }
        config_ = config;
        ownerThread_ = std::this_thread::get_id();
        g_activeTransport = this;
        initialized_ = true;
        ++capabilityRevision_;
        return Result<void>::Success();
    }

    Result<ConnectionHandle> GnsTransport::ReserveConnection() {
        for (std::uint32_t i = 0; i < connections_.size(); ++i) {
            auto &slot = connections_[i];
            if (slot.phase != ConnectionPhase::Terminal || slot.handle.Generation() == std::numeric_limits<std::uint32_t>::max())
                continue;
            auto next = slot.handle.NextGeneration();
            if (next.HasError())
                continue;
            slot = ConnectionSlot{};
            slot.handle = std::move(next).Value();
            slot.phase = ConnectionPhase::Pending;
            return Result<ConnectionHandle>::Success(slot.handle);
        }
        if (connections_.size() >= config_.maximumConnections)
            return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportLimitExceeded));
        auto created = ConnectionHandle::Create(static_cast<std::uint32_t>(connections_.size()), 1);
        if (created.HasError())
            return created;
        connections_.push_back(ConnectionSlot{});
        auto &slot = connections_.back();
        slot.handle = created.Value();
        slot.phase = ConnectionPhase::Pending;
        return created;
    }

    ConnectionSlot *GnsTransport::Find(const ConnectionHandle handle) noexcept {
        if (!handle.IsValid() || handle.Slot() >= connections_.size())
            return nullptr;
        auto &slot = connections_[handle.Slot()];
        return slot.handle == handle ? &slot : nullptr;
    }

    ConnectionSlot *GnsTransport::Find(const HSteamNetConnection native) noexcept {
        if (native == k_HSteamNetConnection_Invalid)
            return nullptr;
        const auto it = std::find_if(connections_.begin(), connections_.end(), [native](const ConnectionSlot &slot) {
            return slot.native == native && slot.phase != ConnectionPhase::Terminal;
        });
        return it == connections_.end() ? nullptr : &*it;
    }

    Result<ListenerHandle> GnsTransport::Listen(const NetworkListenRequest &request) {
        std::lock_guard lock(mutex_);
        if (shutdown_)
            return Result<ListenerHandle>::Failure(MakeError(NetworkErrors::TransportShuttingDown));
        if (!initialized_)
            return Result<ListenerHandle>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable));
        if (nativeListener_ != k_HSteamListenSocket_Invalid || request.maximumConnections == 0 ||
            request.maximumConnections > config_.maximumConnections)
            return Result<ListenerHandle>::Failure(MakeError(NetworkErrors::TransportLimitExceeded));
        auto address = GnsDetail::NativeAddress(request.bindAddress);
        if (address.HasError())
            return Result<ListenerHandle>::Failure(std::move(address).ErrorValue());
        SteamNetworkingConfigValue_t option{};
        option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void *>(&OnStatus));
        const auto native = native_->CreateListenSocketIP(address.Value(), 1, &option);
        if (native == k_HSteamListenSocket_Invalid)
            return Result<ListenerHandle>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
        auto handle = listener_.IsValid() ? listener_.NextGeneration() : ListenerHandle::Create(0, 1);
        if (handle.HasError()) {
            native_->CloseListenSocket(native);
            return handle;
        }
        listener_ = handle.Value();
        nativeListener_ = native;
        listenerMaximumConnections_ = request.maximumConnections;
        Enqueue({.kind = NetworkTransportEventKind::ListenerReady, .listener = listener_});
        return handle;
    }

    Result<ConnectionHandle> GnsTransport::Connect(const NetworkConnectRequest &request) {
        std::lock_guard lock(mutex_);
        if (shutdown_)
            return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportShuttingDown));
        if (!initialized_)
            return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable));
        if (request.cancellation.IsCancellationRequested())
            return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportOperationCancelled));
        if (request.timeout <= std::chrono::milliseconds::zero() || request.timeout > std::chrono::minutes{5})
            return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportLimitExceeded));
        if (!request.endpoint.IsValid())
            return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::NetworkAddressInvalid));
        std::optional<SteamNetworkingIPAddr> address;
        if (!request.endpoint.RequiresResolution()) {
            auto converted = GnsDetail::NativeAddress(request.endpoint);
            if (converted.HasError())
                return Result<ConnectionHandle>::Failure(std::move(converted).ErrorValue());
            address = converted.Value();
        }
        auto handle = ReserveConnection();
        if (handle.HasError())
            return handle;
        auto &slot = connections_[handle.Value().Slot()];
        slot.cancellation = request.cancellation;
        slot.deadline = std::chrono::steady_clock::now() + request.timeout;
        if (address) {
            auto started = StartNative(slot, *address);
            if (started.HasError()) {
                slot.phase = ConnectionPhase::Terminal;
                return Result<ConnectionHandle>::Failure(std::move(started).ErrorValue());
            }
        } else {
            auto started = StartResolution(slot, request.endpoint);
            if (started.HasError()) {
                slot.phase = ConnectionPhase::Terminal;
                return Result<ConnectionHandle>::Failure(std::move(started).ErrorValue());
            }
        }
        return handle;
    }

    Result<void> GnsTransport::StartResolution(ConnectionSlot &slot, const NetworkAddress &endpoint) {
        if (resolverTasks_->fetch_add(1) >= MaximumConcurrentResolvers) {
            resolverTasks_->fetch_sub(1);
            return Fail(NetworkErrors::TransportLimitExceeded);
        }
        try {
            slot.resolution = std::make_shared<GnsDetail::Resolution>();
            const std::string hostname(endpoint.Hostname());
            const auto state = slot.resolution;
            const auto port = endpoint.Port();
            std::thread([state, hostname, port, tasks = resolverTasks_] {
                GnsDetail::ResolveHostname(state, hostname, port);
                tasks->fetch_sub(1);
            }).detach();
        } catch (const std::bad_alloc &) {
            resolverTasks_->fetch_sub(1);
            slot.resolution.reset();
            return Fail(NetworkErrors::TransportCapabilityUnavailable);
        } catch (const std::system_error &) {
            resolverTasks_->fetch_sub(1);
            slot.resolution.reset();
            return Fail(NetworkErrors::TransportCapabilityUnavailable);
        }
        return Result<void>::Success();
    }

    Result<void> GnsTransport::StartNative(ConnectionSlot &slot, const SteamNetworkingIPAddr &address) {
        SteamNetworkingConfigValue_t option{};
        option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void *>(&OnStatus));
        const auto native = native_->ConnectByIPAddress(address, 1, &option);
        if (native == k_HSteamNetConnection_Invalid)
            return Fail(NetworkErrors::TransportNativeUnavailable);
        slot.native = native;
        return Result<void>::Success();
    }

    void GnsTransport::Enqueue(NetworkTransportEvent event) {
        // Native callback count is bounded by configured live connections;
        // packet reads stop before this queue reaches its finite capacity.
        const std::size_t capacity = std::size_t{config_.maximumConnections} * 4 + config_.maximumEventsPerPoll + 1;
        if (events_.size() >= capacity) {
            overflow_ = true;
            return;
        }
        try {
            events_.push_back(std::move(event));
        } catch (const std::bad_alloc &) {
            overflow_ = true;
        }
    }

    void GnsTransport::End(ConnectionSlot &slot, const NetworkTransportEventKind kind, const ErrorCodeDescriptor *failure) {
        if (slot.phase == ConnectionPhase::Terminal)
            return;
        if (slot.phase == ConnectionPhase::Connected && stats_.activeConnections != 0)
            --stats_.activeConnections;
        const auto native = std::exchange(slot.native, k_HSteamNetConnection_Invalid);
        slot.phase = ConnectionPhase::Terminal;
        slot.resolution.reset();
        if (native != k_HSteamNetConnection_Invalid)
            native_->CloseConnection(native, 0, nullptr, false);
        Enqueue({.kind = kind, .connection = slot.handle, .failure = failure});
    }

    void GnsTransport::OnStatusOwned(const SteamNetConnectionStatusChangedCallback_t &status) {
        std::lock_guard lock(mutex_);
        if (shutdown_)
            return;
        if (status.m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting && status.m_info.m_hListenSocket == nativeListener_ &&
            nativeListener_ != k_HSteamListenSocket_Invalid) {
            const auto inboundCount = std::count_if(connections_.begin(), connections_.end(), [](const ConnectionSlot &slot) {
                return slot.inbound && slot.phase != ConnectionPhase::Terminal;
            });
            if (static_cast<std::size_t>(inboundCount) >= listenerMaximumConnections_) {
                native_->CloseConnection(status.m_hConn, 0, nullptr, false);
                return;
            }
            auto admitted = ReserveConnection();
            if (admitted.HasError()) {
                native_->CloseConnection(status.m_hConn, 0, nullptr, false);
                return;
            }
            auto &slot = connections_[admitted.Value().Slot()];
            slot.native = status.m_hConn;
            slot.inbound = true;
            slot.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            if (native_->AcceptConnection(status.m_hConn) != k_EResultOK) {
                End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportConnectionFailed);
                return;
            }
            Enqueue({.kind = NetworkTransportEventKind::Accepted, .listener = listener_, .connection = slot.handle});
            return;
        }
        auto *slot = Find(status.m_hConn);
        if (!slot)
            return;  // Retired native callback cannot name a replacement generation.
        if (status.m_info.m_eState == k_ESteamNetworkingConnectionState_Connected && slot->phase == ConnectionPhase::Pending) {
            slot->phase = ConnectionPhase::Connected;
            ++stats_.activeConnections;
            Enqueue({.kind = NetworkTransportEventKind::Connected, .connection = slot->handle});
        } else if (status.m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer) {
            End(*slot, NetworkTransportEventKind::Closed);
        } else if (status.m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
            End(*slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportConnectionFailed);
        }
    }

    Result<void> GnsTransport::Send(const ConnectionHandle connection, const ChannelId channel, const std::span<const std::byte> payload,
                                    const DeliveryPolicy delivery) {
        std::lock_guard lock(mutex_);
        if (shutdown_)
            return Fail(NetworkErrors::TransportShuttingDown);
        auto *slot = Find(connection);
        if (!slot)
            return Fail(NetworkErrors::TransportHandleInvalid);
        if (slot->phase != ConnectionPhase::Connected)
            return Fail(NetworkErrors::TransportConnectionFailed);
        if (channel.Value() != 0 || payload.size() > config_.maximumMessageBytes)
            return Fail(NetworkErrors::TransportLimitExceeded);
        if (delivery != DeliveryPolicy::ReliableOrdered && delivery != DeliveryPolicy::UnreliableUnordered)
            return Fail(NetworkErrors::TransportDeliveryUnsupported);
        const int flags = delivery == DeliveryPolicy::ReliableOrdered ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
        const std::byte empty{};
        const void *bytes = payload.empty() ? static_cast<const void *>(&empty) : payload.data();
        const auto result =
            native_->SendMessageToConnection(slot->native, bytes, static_cast<std::uint32_t>(payload.size()), flags, nullptr);
        if (result == k_EResultLimitExceeded)
            return Fail(delivery == DeliveryPolicy::ReliableOrdered ? NetworkErrors::TransportReliableBackpressure
                                                                    : NetworkErrors::TransportLimitExceeded);
        if (result != k_EResultOK)
            return Fail(NetworkErrors::TransportConnectionFailed);
        ++stats_.sentMessages;
        stats_.sentBytes += payload.size();
        return Result<void>::Success();
    }

    Result<void> GnsTransport::Close(const ConnectionHandle connection) {
        std::lock_guard lock(mutex_);
        if (shutdown_)
            return Fail(NetworkErrors::TransportShuttingDown);
        auto *slot = Find(connection);
        if (!slot)
            return Fail(NetworkErrors::TransportHandleInvalid);
        End(*slot, NetworkTransportEventKind::Closed);
        return Result<void>::Success();
    }

    Result<void> GnsTransport::CloseListener(const ListenerHandle listener) {
        std::lock_guard lock(mutex_);
        if (shutdown_)
            return Fail(NetworkErrors::TransportShuttingDown);
        if (!listener.IsValid() || listener != listener_)
            return Fail(NetworkErrors::TransportHandleInvalid);
        if (nativeListener_ != k_HSteamListenSocket_Invalid) {
            native_->CloseListenSocket(nativeListener_);
            nativeListener_ = k_HSteamListenSocket_Invalid;
        }
        return Result<void>::Success();
    }

    void GnsTransport::PollMessages() {
        const std::size_t capacity = std::size_t{config_.maximumConnections} * 4 + config_.maximumEventsPerPoll + 1;
        for (auto &slot : connections_) {
            if (slot.phase != ConnectionPhase::Connected)
                continue;
            while (events_.size() < capacity && events_.size() < config_.maximumEventsPerPoll) {
                SteamNetworkingMessage_t *message{};
                const int count = native_->ReceiveMessagesOnConnection(slot.native, &message, 1);
                if (count < 0) {
                    End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportConnectionFailed);
                    break;
                }
                if (count == 0)
                    break;
                if (!message || message->m_cbSize < 0 || static_cast<std::uint32_t>(message->m_cbSize) > config_.maximumMessageBytes ||
                    (message->m_cbSize != 0 && !message->m_pData)) {
                    if (message)
                        message->Release();
                    End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportMalformedPacket);
                    break;
                }
                NetworkTransportEvent event{.kind = NetworkTransportEventKind::PacketReceived,
                                            .connection = slot.handle,
                                            .delivery = (message->m_nFlags & k_nSteamNetworkingSend_Reliable)
                                                            ? DeliveryPolicy::ReliableOrdered
                                                            : DeliveryPolicy::UnreliableUnordered};
                const auto *first = static_cast<const std::byte *>(message->m_pData);
                try {
                    if (message->m_cbSize != 0)
                        event.payload.assign(first, first + message->m_cbSize);
                } catch (const std::bad_alloc &) {
                    message->Release();
                    End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportCapabilityUnavailable);
                    break;
                }
                ++stats_.receivedMessages;
                stats_.receivedBytes += event.payload.size();
                message->Release();
                Enqueue(std::move(event));
            }
        }
    }

    Result<std::size_t> GnsTransport::PollEvents(INetworkTransportEventConsumer &consumer) {
        std::unique_lock lock(mutex_);
        if (shutdown_)
            return Result<std::size_t>::Failure(MakeError(NetworkErrors::TransportShuttingDown));
        if (!initialized_)
            return Result<std::size_t>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable));
        if (std::this_thread::get_id() != ownerThread_)
            return Result<std::size_t>::Failure(MakeError(NetworkErrors::NetworkIoWrongThread));
        AdvancePending(std::chrono::steady_clock::now());
        native_->RunCallbacks();
        PollMessages();
        if (overflow_) {
            Shutdown();
            return Result<std::size_t>::Failure(MakeError(NetworkErrors::FatalFailure));
        }
        const auto count = std::min(events_.size(), std::size_t{config_.maximumEventsPerPoll});
        std::vector<NetworkTransportEvent> ready;
        ready.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            ready.push_back(std::move(events_.front()));
            events_.pop_front();
        }
        lock.unlock();
        for (auto &event : ready)
            consumer.Consume(std::move(event));
        return Result<std::size_t>::Success(count);
    }

    void GnsTransport::AdvancePending(const std::chrono::steady_clock::time_point now) {
        for (auto &slot : connections_) {
            if (slot.phase == ConnectionPhase::Pending) {
                if (slot.cancellation.IsCancellationRequested())
                    End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportOperationCancelled);
                else if (now >= slot.deadline)
                    End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::SessionTimedOut);
                else if (slot.resolution) {
                    std::optional<SteamNetworkingIPAddr> address;
                    bool complete{};
                    {
                        std::lock_guard resolutionLock(slot.resolution->mutex);
                        complete = slot.resolution->complete;
                        address = slot.resolution->address;
                    }
                    if (complete) {
                        slot.resolution.reset();
                        if (!address)
                            End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::NameResolutionFailed);
                        else if (StartNative(slot, *address).HasError())
                            End(slot, NetworkTransportEventKind::Failed, &NetworkErrors::TransportNativeUnavailable);
                    }
                }
            }
        }
    }

    void GnsTransport::Shutdown() noexcept {
        std::lock_guard lock(mutex_);
        if (shutdown_)
            return;
        shutdown_ = true;
        ++capabilityRevision_;
        events_.clear();
        if (!initialized_)
            return;
        for (auto &slot : connections_) {
            if (slot.native != k_HSteamNetConnection_Invalid)
                native_->CloseConnection(slot.native, 0, nullptr, false);
            slot.native = k_HSteamNetConnection_Invalid;
            slot.phase = ConnectionPhase::Terminal;
            slot.resolution.reset();
        }
        if (nativeListener_ != k_HSteamListenSocket_Invalid)
            native_->CloseListenSocket(nativeListener_);
        nativeListener_ = k_HSteamListenSocket_Invalid;
        stats_.activeConnections = 0;
        resolverTasks_.reset();
        std::lock_guard nativeLock(g_nativeMutex);
        g_activeTransport = nullptr;
        GameNetworkingSockets_Kill();
        native_ = nullptr;
        initialized_ = false;
    }

    Result<std::unique_ptr<INetworkTransport>> CreateGnsTransport() {
        try {
            return Result<std::unique_ptr<INetworkTransport>>::Success(std::make_unique<GnsTransport>());
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<INetworkTransport>>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable));
        }
    }
}  // namespace Horo::Network
