#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
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
        constexpr std::size_t MaximumConcurrentResolvers = 8;

        // GNS owns a process-global default interface. This function-local
        // state serializes the single active adapter with native callbacks.
        struct NativeHostState final {
            std::mutex mutex;
            GnsTransport *active{};
            bool shuttingDown{};
        };

        NativeHostState &NativeHost() {
            static NativeHostState state;
            return state;
        }

        [[nodiscard]] Result<void> Fail(const ErrorCodeDescriptor &code) {
            return Result<void>::Failure(MakeError(code));
        }
    }  // namespace

    void GnsTransport::OnStatus(SteamNetConnectionStatusChangedCallback_t *status) {
        if (!status)
            return;
        auto &host = NativeHost();
        std::lock_guard hostLock(host.mutex);
        auto *transport = host.active;
        if (!transport)
            return;
        std::lock_guard queueLock(transport->callbackMutex_);
        const std::size_t capacity = std::size_t{transport->config_.maximumConnections} * 4 + transport->config_.maximumEventsPerPoll + 1;
        if (transport->callbacks_.size() >= capacity) {
            transport->callbackOverflow_ = true;
            return;
        }
        try {
            transport->callbacks_.push_back(*status);
        } catch (const std::bad_alloc &) {
            // Nothing may unwind across GNS's C callback boundary.
            transport->callbackOverflow_ = true;
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
        } catch (const std::bad_alloc &) {
            return Fail(NetworkErrors::TransportCapabilityUnavailable);
        }
        auto &host = NativeHost();
        std::lock_guard nativeLock(host.mutex);
        if (host.active || host.shuttingDown)
            return Fail(NetworkErrors::TransportCapabilityUnavailable);
        if (ares_library_init(ARES_LIB_INIT_ALL) != ARES_SUCCESS)
            return Fail(NetworkErrors::TransportNativeUnavailable);
        caresInitialized_ = true;
        if (SteamNetworkingErrMsg error{}; !GameNetworkingSockets_Init(nullptr, error)) {
            ares_library_cleanup();
            caresInitialized_ = false;
            return Fail(NetworkErrors::TransportNativeUnavailable);
        }
        native_ = SteamNetworkingSockets();
        if (!native_) {
            GameNetworkingSockets_Kill();
            ares_library_cleanup();
            caresInitialized_ = false;
            return Fail(NetworkErrors::TransportNativeUnavailable);
        }
        config_ = config;
        ownerThread_ = std::this_thread::get_id();
        host.active = this;
        initialized_ = true;
        ++capabilityRevision_;
        return Result<void>::Success();
    }

    Result<ConnectionHandle> GnsTransport::ReserveConnection() {
        for (auto &slot : connections_) {
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
        connections_.emplace_back();
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
        const auto it = std::ranges::find_if(connections_, [native](const ConnectionSlot &slot) {
            return slot.native == native && slot.phase != ConnectionPhase::Terminal;
        });
        return it == connections_.end() ? nullptr : std::to_address(it);
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
        } else {
            const auto activeResolvers = std::ranges::count_if(connections_, [](const ConnectionSlot &candidate) {
                return candidate.pendingEndpoint.has_value() || candidate.resolution != nullptr;
            });
            if (static_cast<std::size_t>(activeResolvers) >= MaximumConcurrentResolvers)
                return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportLimitExceeded));
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
            try {
                slot.pendingEndpoint = request.endpoint;
            } catch (const std::bad_alloc &) {
                slot.phase = ConnectionPhase::Terminal;
                return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable));
            }
        }
        return handle;
    }

    const ErrorCodeDescriptor *GnsTransport::StartResolution(ConnectionSlot &slot) const {
        try {
            auto resolution = std::make_unique<GnsDetail::Resolution>();
            const std::string hostname(slot.pendingEndpoint->Hostname());
            if (!GnsDetail::StartResolution(*resolution, hostname, slot.pendingEndpoint->Port(), resolverServer_))
                return &NetworkErrors::NameResolutionFailed;
            slot.resolution = std::move(resolution);
            slot.pendingEndpoint.reset();
        } catch (const std::bad_alloc &) {
            return &NetworkErrors::TransportCapabilityUnavailable;
        }
        return nullptr;
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
        if (const std::size_t capacity = std::size_t{config_.maximumConnections} * 4 + config_.maximumEventsPerPoll + 1;
            events_.size() >= capacity) {
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
        using enum ConnectionPhase;
        if (slot.phase == Terminal)
            return;
        if (slot.phase == Connected && stats_.activeConnections != 0)
            --stats_.activeConnections;
        const auto native = std::exchange(slot.native, k_HSteamNetConnection_Invalid);
        slot.phase = Terminal;
        slot.pendingEndpoint.reset();
        slot.resolution.reset();
        if (native != k_HSteamNetConnection_Invalid)
            native_->CloseConnection(native, 0, nullptr, false);
        Enqueue({.kind = kind, .connection = slot.handle, .failure = failure});
    }

    void GnsTransport::OnStatusOwned(const SteamNetConnectionStatusChangedCallback_t &status) {
        if (shutdown_)
            return;
        if (status.m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting && status.m_info.m_hListenSocket == nativeListener_ &&
            nativeListener_ != k_HSteamListenSocket_Invalid) {
            if (const auto inboundCount = std::ranges::count_if(connections_,
                                                                [](const ConnectionSlot &slot) {
                return slot.inbound && slot.phase != ConnectionPhase::Terminal;
            });
                static_cast<std::size_t>(inboundCount) >= listenerMaximumConnections_) {
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

    void GnsTransport::DrainNativeCallbacks() {
        std::deque<SteamNetConnectionStatusChangedCallback_t> pending;
        {
            std::lock_guard queueLock(callbackMutex_);
            pending.swap(callbacks_);
            overflow_ |= std::exchange(callbackOverflow_, false);
        }
        for (const auto &status : pending)
            OnStatusOwned(status);
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

    Result<std::size_t> GnsTransport::PollEvents(INetworkTransportEventConsumer &consumer) {
        std::vector<NetworkTransportEvent> ready;
        bool fatal{};
        {
            std::lock_guard lock(mutex_);
            if (shutdown_)
                return Result<std::size_t>::Failure(MakeError(NetworkErrors::TransportShuttingDown));
            if (!initialized_)
                return Result<std::size_t>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable));
            if (std::this_thread::get_id() != ownerThread_)
                return Result<std::size_t>::Failure(MakeError(NetworkErrors::NetworkIoWrongThread));
            AdvancePending(std::chrono::steady_clock::now());
            native_->RunCallbacks();
            DrainNativeCallbacks();
            PollMessages();
            fatal = overflow_;
            if (!fatal) {
                const auto count = std::min(events_.size(), std::size_t{config_.maximumEventsPerPoll});
                try {
                    ready.reserve(count);
                    for (std::size_t i = 0; i < count; ++i) {
                        ready.push_back(std::move(events_.front()));
                        events_.pop_front();
                    }
                } catch (const std::bad_alloc &) {
                    fatal = true;
                }
            }
        }
        if (fatal) {
            Shutdown();
            return Result<std::size_t>::Failure(MakeError(NetworkErrors::FatalFailure));
        }
        for (auto &event : ready)
            consumer.Consume(std::move(event));
        return Result<std::size_t>::Success(ready.size());
    }

    void GnsTransport::AdvancePending(const std::chrono::steady_clock::time_point now) {
        for (auto &slot : connections_) {
            if (slot.phase == ConnectionPhase::Pending)
                AdvancePendingSlot(slot, now);
        }
    }

    void GnsTransport::AdvancePendingSlot(ConnectionSlot &slot, const std::chrono::steady_clock::time_point now) {
        using enum NetworkTransportEventKind;
        if (slot.cancellation.IsCancellationRequested()) {
            End(slot, Failed, &NetworkErrors::TransportOperationCancelled);
            return;
        }
        if (now >= slot.deadline) {
            End(slot, Failed, &NetworkErrors::SessionTimedOut);
            return;
        }
        if (slot.pendingEndpoint) {
            if (const auto *failure = StartResolution(slot)) {
                End(slot, Failed, failure);
                return;
            }
        }
        if (!slot.resolution)
            return;
        if (!GnsDetail::PollResolution(*slot.resolution)) {
            End(slot, Failed, &NetworkErrors::NameResolutionFailed);
            return;
        }
        if (!slot.resolution->complete)
            return;
        const auto address = slot.resolution->address;
        slot.resolution.reset();
        if (!address)
            End(slot, Failed, &NetworkErrors::NameResolutionFailed);
        else if (StartNative(slot, *address).HasError())
            End(slot, Failed, &NetworkErrors::TransportNativeUnavailable);
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
            slot.pendingEndpoint.reset();
            slot.resolution.reset();
        }
        if (nativeListener_ != k_HSteamListenSocket_Invalid)
            native_->CloseListenSocket(nativeListener_);
        nativeListener_ = k_HSteamListenSocket_Invalid;
        stats_.activeConnections = 0;
        auto &host = NativeHost();
        {
            std::lock_guard nativeLock(host.mutex);
            host.active = nullptr;
            host.shuttingDown = true;
        }
        {
            std::lock_guard queueLock(callbackMutex_);
            callbacks_.clear();
            callbackOverflow_ = false;
        }
        GameNetworkingSockets_Kill();
        if (caresInitialized_) {
            ares_library_cleanup();
            caresInitialized_ = false;
        }
        native_ = nullptr;
        initialized_ = false;
        {
            std::lock_guard nativeLock(host.mutex);
            host.shuttingDown = false;
        }
    }

    Result<std::unique_ptr<INetworkTransport>> CreateGnsTransport() {
        try {
            return Result<std::unique_ptr<INetworkTransport>>::Success(std::make_unique<GnsTransport>());
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<INetworkTransport>>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable));
        }
    }
}  // namespace Horo::Network
