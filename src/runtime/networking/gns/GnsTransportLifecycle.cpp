#include "GnsNativeHost.h"
#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

#include <new>

namespace Horo::Network {
    /** @copydoc NativeHost */
    NativeHostState &NativeHost() {
        static NativeHostState state;
        return state;
    }

    namespace {
        struct CleanupResult final {
            bool nativePending{};
            std::uint64_t root{};
            SteamNetworkingSocketsLib::HoroBridgeStatus status{SteamNetworkingSocketsLib::HoroBridgeStatus::Ok};
        };

        // No allocating error projection in cleanup. Failed native retirement stays
        // owned by process state, even after the unique adapter is destroyed.
        template <typename Kill = decltype(&GameNetworkingSockets_Kill)>
        CleanupResult CleanupNative(const bool killNative, const std::uint64_t root,
                                    const Kill kill = &GameNetworkingSockets_Kill) noexcept {
            using enum SteamNetworkingSocketsLib::HoroBridgeStatus;
            if (killNative && !GnsDetail::NativeVoid([kill] {
                kill();
            }))
                return {true, root, Unavailable};
            if (!root)
                return {};
            const auto status = SteamNetworkingSocketsLib::HoroRetireSelectedRoot(root);
            return {false, status == Ok ? 0 : root, status};
        }

        void PublishCleanup(const CleanupResult &result) noexcept {
            auto &host = NativeHost();
            std::lock_guard lock(host.mutex);
            host.nativeCleanupPending = result.nativePending;
            host.cleanupRoot = result.root;
            host.cleanupRunning = false;
            host.shuttingDown = result.nativePending || result.root != 0;
        }

        SteamNetworkingSocketsLib::HoroBridgeStatus RetryCleanup() noexcept {
            using enum SteamNetworkingSocketsLib::HoroBridgeStatus;
            auto &host = NativeHost();
            bool nativePending{};
            std::uint64_t root{};
            {
                std::lock_guard lock(host.mutex);
                if (host.active)
                    return Ok;  // Existing adapter-conflict path retains its error.
                if (host.cleanupRunning)
                    return Unavailable;
                if (!host.shuttingDown)
                    return Ok;
                host.cleanupRunning = true;
                nativePending = host.nativeCleanupPending;
                root = host.cleanupRoot;
            }
            const auto result = CleanupNative(nativePending, root);
            PublishCleanup(result);
            return result.status;
        }

        Result<void> Fail(const ErrorCodeDescriptor &code) {
            return Result<void>::Failure(MakeError(code));
        }
    }  // namespace

    Result<void> GnsTransport::Initialize(const NetworkTransportConfig &config) {
        {
            std::lock_guard lock(mutex_);
            if (shutdown_)
                return Fail(NetworkErrors::TransportShuttingDown);
            if (initialized_ || !ValidGnsTransportConfig(config))
                return Fail(NetworkErrors::TransportLimitExceeded);
            if (security_ &&
                (config.maximumConnections != config_.maximumConnections || config.maximumEventsPerPoll != config_.maximumEventsPerPoll ||
                 config.maximumMessageBytes != config_.maximumMessageBytes || config.maximumChannels != config_.maximumChannels))
                return Fail(NetworkErrors::TransportBackendConflict);
            try {
                connections_.reserve(config.maximumConnections);
            } catch (const std::bad_alloc &) {
                return Fail(NetworkErrors::TransportCapabilityUnavailable);
            }
            if (auto started = StartNativeOwner(config); started.HasError())
                return started;
            if (!security_) {
                initialized_ = true;
                ++capabilityRevision_;
                return Result<void>::Success();
            }
        }
        // The provisioning callback runs between lexical lock scopes. Reentrant
        // Shutdown is legitimate and is checked before either native use/publication.
        if (auto provisioned = ProvisionNative(); provisioned.HasError()) {
            Shutdown();
            return provisioned;
        }
        std::lock_guard lock(mutex_);
        if (shutdown_ || !nativeOwner_.sockets)
            return Fail(NetworkErrors::TransportShuttingDown);
        initialized_ = true;
        security_->Attach(*this);
        return Result<void>::Success();
    }

    Result<void> GnsTransport::StartNativeOwner(const NetworkTransportConfig &config) {
        if (const auto cleanup = RetryCleanup(); cleanup != SteamNetworkingSocketsLib::HoroBridgeStatus::Ok)
            return Result<void>::Failure(GnsSecurityFailure(cleanup));
        auto &host = NativeHost();
        {
            std::lock_guard nativeLock(host.mutex);
            if (host.active || host.shuttingDown)
                return Fail(NetworkErrors::TransportCapabilityUnavailable);
            if (ares_library_init(ARES_LIB_INIT_ALL) != ARES_SUCCESS)
                return Fail(NetworkErrors::TransportNativeUnavailable);
            nativeOwner_.resolverInitialized = true;
            if (SteamNetworkingErrMsg error{}; !GnsDetail::NativeCall([&error] {
                return GameNetworkingSockets_Init(nullptr, error);
            }).value_or(false)) {
                ares_library_cleanup();
                nativeOwner_.resolverInitialized = false;
                return Fail(NetworkErrors::TransportNativeUnavailable);
            }
            nativeOwner_.sockets = GnsDetail::NativeCall([] {
                return SteamNetworkingSockets();
            }).value_or(nullptr);
            if (nativeOwner_.sockets) {
                config_ = config;
                ownerThread_ = std::this_thread::get_id();
                host.active = this;
                return Result<void>::Success();
            }
            // Transfer retirement before releasing the process lock. Cleanup
            // outside this scope can reacquire it without manual unlock paths.
            host.shuttingDown = true;
            host.cleanupRunning = true;
            host.nativeCleanupPending = true;
        }
        PublishCleanup(CleanupNative(true, 0));
        ares_library_cleanup();
        nativeOwner_.resolverInitialized = false;
        return Fail(NetworkErrors::TransportNativeUnavailable);
    }

    Result<std::vector<std::byte>> GnsTransport::PrepareCertificateRequest() {
        std::lock_guard lock(mutex_);
        if (shutdown_ || !nativeOwner_.sockets)
            return Result<std::vector<std::byte>>::Failure(MakeError(NetworkErrors::TransportShuttingDown));
        // Native allocation/result construction remains inside the existing
        // genuine foreign-code barrier; typed Error projection is outside it.
        auto request = GnsDetail::NativeCall([this] {
            return security_->Prepare(*nativeOwner_.sockets, nativeOwner_.rootGeneration);
        });
        if (!request.has_value())
            return Result<std::vector<std::byte>>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable));
        return std::move(request.value());
    }

    Result<void> GnsTransport::ProvisionNative() {
        auto request = PrepareCertificateRequest();
        if (request.HasError())
            return Result<void>::Failure(request.ErrorValue());
        // Neither native nor Horo locks are held across the host issuer.
        auto certificate = security_->Issue(request.Value());
        std::lock_guard lock(mutex_);
        if (shutdown_ || !nativeOwner_.sockets)
            return Fail(NetworkErrors::TransportShuttingDown);
        if (certificate.HasError())
            return Result<void>::Failure(certificate.ErrorValue());
        auto ownedCertificate = std::move(certificate).Value();
        SteamNetworkingErrMsg error{};
        if (const auto accepted = GnsDetail::NativeCall([this, &ownedCertificate, &error] {
            return nativeOwner_.sockets->SetCertificate(ownedCertificate.View().data(), static_cast<int>(ownedCertificate.View().size()),
                                                        error);
        });
            !accepted.has_value() || !accepted.value())
            return Fail(accepted.has_value() ? NetworkErrors::AuthenticationRejected : NetworkErrors::TransportNativeUnavailable);
        ownedCertificate.Clear();
        return Result<void>::Success();
    }

    void GnsTransport::Shutdown() noexcept {
        // Providers take lease -> transport. Revoke before taking transport so an
        // in-flight provider can finish without a lock inversion or dangling borrow.
        if (security_)
            security_->Detach();
        std::lock_guard lock(mutex_);
        if (shutdown_)
            return;
        shutdown_ = true;
        ++capabilityRevision_;
        events_.clear();
        if (!nativeOwner_.sockets && !nativeOwner_.rootGeneration)
            return;
        auto &host = NativeHost();
        {
            std::lock_guard nativeLock(host.mutex);
            host.active = nullptr;
            host.shuttingDown = true;
            host.cleanupRunning = true;
            host.nativeCleanupPending = nativeOwner_.sockets != nullptr;
            host.cleanupRoot = nativeOwner_.rootGeneration;
        }
        const bool connectionsClosed = CloseNativeConnections();
        // A foreign close exception is handled by the same native Kill fallback,
        // whose failed retirement remains owned for retry; no error is swallowed.
        const auto retired =
            CleanupNative(nativeOwner_.sockets != nullptr || !connectionsClosed, nativeOwner_.rootGeneration, nativeOwner_.kill);
        PublishCleanup(retired);
        nativeOwner_.rootGeneration = 0;  // Retry ownership transferred to NativeHost before release.
        if (nativeOwner_.resolverInitialized) {
            ares_library_cleanup();
            nativeOwner_.resolverInitialized = false;
        }
        nativeOwner_.sockets = nullptr;
        initialized_ = false;
    }

    bool GnsTransport::CloseNativeConnections() noexcept {
        // Kill remains the fallback owner if an individual close unexpectedly
        // throws. Never allocate an Error while retiring native state.
        const bool closed = GnsDetail::NativeVoid([this] {
            for (const auto &slot : connections_) {
                if (slot.native != k_HSteamNetConnection_Invalid)
                    nativeOwner_.sockets->CloseConnection(slot.native, 0, nullptr, false);
            }
            if (nativeListener_ != k_HSteamListenSocket_Invalid)
                nativeOwner_.sockets->CloseListenSocket(nativeListener_);
        });
        for (auto &slot : connections_) {
            slot.native = k_HSteamNetConnection_Invalid;
            slot.phase = ConnectionPhase::Terminal;
            slot.pendingEndpoint.reset();
            slot.resolution.reset();
        }
        nativeListener_ = k_HSteamListenSocket_Invalid;
        stats_.activeConnections = 0;
        {
            std::lock_guard queueLock(callbackMutex_);
            callbacks_.clear();
            callbackOverflow_ = false;
        }
        return closed;
    }

}  // namespace Horo::Network
