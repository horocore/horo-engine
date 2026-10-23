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
        CleanupResult CleanupNative(const bool killNative, const std::uint64_t root) noexcept {
            using enum SteamNetworkingSocketsLib::HoroBridgeStatus;
            if (killNative) {
                try {
                    GameNetworkingSockets_Kill();
                } catch (...) {
                    return {true, root, Unavailable};
                }
            }
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
            auto &host = NativeHost();
            bool nativePending{};
            std::uint64_t root{};
            {
                std::lock_guard lock(host.mutex);
                if (host.active)
                    return SteamNetworkingSocketsLib::HoroBridgeStatus::Ok;  // Existing adapter-conflict path retains its error.
                if (host.cleanupRunning)
                    return SteamNetworkingSocketsLib::HoroBridgeStatus::Unavailable;
                if (!host.shuttingDown)
                    return SteamNetworkingSocketsLib::HoroBridgeStatus::Ok;
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
        std::unique_lock lock(mutex_);
        if (shutdown_)
            return Fail(NetworkErrors::TransportShuttingDown);
        if (initialized_ || !ValidGnsTransportConfig(config))
            return Fail(NetworkErrors::TransportLimitExceeded);
        if (security_ &&
            (config.maximumConnections != config_.maximumConnections || config.maximumEventsPerPoll != config_.maximumEventsPerPoll ||
             config.maximumMessageBytes != config_.maximumMessageBytes))
            return Fail(NetworkErrors::TransportBackendConflict);
        try {
            connections_.reserve(config.maximumConnections);
        } catch (const std::bad_alloc &) {
            return Fail(NetworkErrors::TransportCapabilityUnavailable);
        }
        if (auto started = StartNativeOwner(config); started.HasError())
            return started;
        if (security_) {
            if (auto provisioned = ProvisionNative(lock); provisioned.HasError())
                return provisioned;
        }
        initialized_ = true;
        if (security_)
            security_->Attach(*this);
        if (!security_)
            ++capabilityRevision_;
        return Result<void>::Success();
    }

    Result<void> GnsTransport::StartNativeOwner(const NetworkTransportConfig &config) {
        const auto cleanup = RetryCleanup();
        if (cleanup != SteamNetworkingSocketsLib::HoroBridgeStatus::Ok)
            return Result<void>::Failure(GnsSecurityFailure(cleanup));
        auto &host = NativeHost();
        std::unique_lock nativeLock(host.mutex);
        if (host.active || host.shuttingDown)
            return Fail(NetworkErrors::TransportCapabilityUnavailable);
        if (ares_library_init(ARES_LIB_INIT_ALL) != ARES_SUCCESS)
            return Fail(NetworkErrors::TransportNativeUnavailable);
        caresInitialized_ = true;
        if (SteamNetworkingErrMsg error{}; !GnsDetail::NativeCall([&] {
            return GameNetworkingSockets_Init(nullptr, error);
        }).value_or(false)) {
            ares_library_cleanup();
            caresInitialized_ = false;
            return Fail(NetworkErrors::TransportNativeUnavailable);
        }
        native_ = GnsDetail::NativeCall([] {
            return SteamNetworkingSockets();
        }).value_or(nullptr);
        if (!native_) {
            // Initialization published a native owner. Transfer its retirement
            // before releasing the process lock, including failed Kill retries.
            host.shuttingDown = true;
            host.cleanupRunning = true;
            host.nativeCleanupPending = true;
            nativeLock.unlock();
            PublishCleanup(CleanupNative(true, 0));
            ares_library_cleanup();
            caresInitialized_ = false;
            return Fail(NetworkErrors::TransportNativeUnavailable);
        }
        config_ = config;
        ownerThread_ = std::this_thread::get_id();
        host.active = this;
        return Result<void>::Success();
    }

    Result<void> GnsTransport::ProvisionNative(std::unique_lock<std::mutex> &lock) {
        Result<std::vector<std::byte>> request =
            Result<std::vector<std::byte>>::Failure(MakeError(NetworkErrors::AuthenticationTrustUnavailable));
        try {
            request = security_->Prepare(*native_, rootGeneration_);
        } catch (...) {
            request = Result<std::vector<std::byte>>::Failure(MakeError(NetworkErrors::TransportCapabilityUnavailable));
        }
        if (request.HasError()) {
            auto error = request.ErrorValue();
            lock.unlock();
            Shutdown();
            return Result<void>::Failure(std::move(error));
        }
        // No native or Horo lock may be held across a host issuer callback.
        lock.unlock();
        auto certificate = security_->Issue(request.Value());
        lock.lock();
        if (shutdown_ || !native_)
            return Fail(NetworkErrors::TransportShuttingDown);
        if (certificate.HasError()) {
            auto error = certificate.ErrorValue();
            lock.unlock();
            Shutdown();
            return Result<void>::Failure(std::move(error));
        }
        auto ownedCertificate = std::move(certificate).Value();
        SteamNetworkingErrMsg error{};
        const auto accepted = GnsDetail::NativeCall([&] {
            return native_->SetCertificate(ownedCertificate.View().data(), static_cast<int>(ownedCertificate.View().size()), error);
        });
        if (!accepted || !*accepted) {
            lock.unlock();
            Shutdown();
            return Fail(accepted ? NetworkErrors::AuthenticationRejected : NetworkErrors::TransportNativeUnavailable);
        }
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
        if (!native_ && !rootGeneration_)
            return;
        auto &host = NativeHost();
        {
            std::lock_guard nativeLock(host.mutex);
            host.active = nullptr;
            host.shuttingDown = true;
            host.cleanupRunning = true;
            host.nativeCleanupPending = native_ != nullptr;
            host.cleanupRoot = rootGeneration_;
        }
        CloseNativeConnections();
        const auto retired = CleanupNative(native_ != nullptr, rootGeneration_);
        PublishCleanup(retired);
        rootGeneration_ = 0;  // Retry ownership transferred to NativeHost before release.
        if (caresInitialized_) {
            ares_library_cleanup();
            caresInitialized_ = false;
        }
        native_ = nullptr;
        initialized_ = false;
    }

    void GnsTransport::CloseNativeConnections() noexcept {
        // Kill remains the fallback owner if an individual close unexpectedly
        // throws. Never allocate an Error while retiring native state.
        try {
            for (auto &slot : connections_) {
                if (slot.native != k_HSteamNetConnection_Invalid)
                    native_->CloseConnection(slot.native, 0, nullptr, false);
            }
            if (nativeListener_ != k_HSteamListenSocket_Invalid)
                native_->CloseListenSocket(nativeListener_);
        } catch (...) {
        }
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
    }

}  // namespace Horo::Network
