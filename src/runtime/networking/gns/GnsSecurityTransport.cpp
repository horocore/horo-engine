#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

namespace Horo::Network {
    /** @copydoc GnsTransport::ReadSecurePeer */
    Result<SteamNetworkingSocketsLib::HoroNativeChannel> GnsTransport::ReadSecurePeer(ConnectionHandle connection,
                                                                                      NetworkOperationGeneration generation) {
        std::lock_guard lock(mutex_);
        using Native = SteamNetworkingSocketsLib::HoroNativeChannel;
        if (!security_ || shutdown_ || !initialized_ || std::this_thread::get_id() != ownerThread_)
            return Result<Native>::Failure(MakeError(NetworkErrors::AuthenticationTrustUnavailable));
        auto *slot = Find(connection);
        if (!slot || slot->phase != ConnectionPhase::Connected || !generation.IsValid() ||
            (slot->admissionGeneration.IsValid() && slot->admissionGeneration != generation))
            return Result<Native>::Failure(MakeError(NetworkErrors::TransportHandleInvalid));
        Native evidence{};
        const auto status = SteamNetworkingSocketsLib::HoroInspectNativeChannel(slot->native, &evidence);
        if (status != SteamNetworkingSocketsLib::HoroBridgeStatus::Ok)
            return Result<Native>::Failure(GnsSecurityFailure(status));
        if (evidence.rootGeneration != rootGeneration_)
            return Result<Native>::Failure(MakeError(NetworkErrors::TerminalGenerationStale));
        slot->admissionGeneration = generation;
        return Result<Native>::Success(evidence);
    }

    /** @copydoc GnsTransport::BindSecurePeer */
    Result<AuthenticationDigest> GnsTransport::BindSecurePeer(ConnectionHandle connection, NetworkOperationGeneration generation,
                                                              const SteamNetworkingSocketsLib::HoroNativeChannel &expected,
                                                              std::span<const std::byte> context) {
        std::lock_guard lock(mutex_);
        auto *slot = Find(connection);
        if (!security_ || shutdown_ || !initialized_ || std::this_thread::get_id() != ownerThread_ || !slot ||
            slot->phase != ConnectionPhase::Connected || slot->admissionGeneration != generation ||
            expected.rootGeneration != rootGeneration_)
            return Result<AuthenticationDigest>::Failure(MakeError(NetworkErrors::TerminalGenerationStale));
        AuthenticationDigest digest{};
        const auto status =
            SteamNetworkingSocketsLib::HoroAttestNativeKeyOwner(slot->native, &expected,
                                                                reinterpret_cast<const std::uint8_t *>(context.data()), context.size(),
                                                                reinterpret_cast<std::uint8_t *>(digest.data()));
        if (status != SteamNetworkingSocketsLib::HoroBridgeStatus::Ok)
            return Result<AuthenticationDigest>::Failure(GnsSecurityFailure(status));
        return Result<AuthenticationDigest>::Success(digest);
    }
}  // namespace Horo::Network
