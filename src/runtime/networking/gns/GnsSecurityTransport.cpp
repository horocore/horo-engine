#include "GnsTransportInternal.h"
#include "Horo/Network/NetworkErrors.h"

#include <bit>

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
        if (const auto status = SteamNetworkingSocketsLib::HoroInspectNativeChannel(slot->native, &evidence);
            status != SteamNetworkingSocketsLib::HoroBridgeStatus::Ok)
            return Result<Native>::Failure(GnsSecurityFailure(status));
        if (evidence.rootGeneration != nativeOwner_.rootGeneration)
            return Result<Native>::Failure(MakeError(NetworkErrors::TerminalGenerationStale));
        slot->admissionGeneration = generation;
        return Result<Native>::Success(evidence);
    }

    /** @copydoc GnsTransport::BindSecurePeer */
    Result<AuthenticationDigest> GnsTransport::BindSecurePeer(ConnectionHandle connection, NetworkOperationGeneration generation,
                                                              const SteamNetworkingSocketsLib::HoroNativeChannel &expected,
                                                              const std::array<std::byte, 192> &context) {
        std::lock_guard lock(mutex_);
        const auto *slot = Find(connection);
        if (!security_ || shutdown_ || !initialized_ || std::this_thread::get_id() != ownerThread_ || !slot ||
            slot->phase != ConnectionPhase::Connected || slot->admissionGeneration != generation ||
            expected.rootGeneration != nativeOwner_.rootGeneration)
            return Result<AuthenticationDigest>::Failure(MakeError(NetworkErrors::TerminalGenerationStale));
        const auto nativeContext = std::bit_cast<std::array<std::uint8_t, 192>>(context);
        std::array<std::uint8_t, 32> digest{};
        if (const auto status = SteamNetworkingSocketsLib::HoroAttestNativeKeyOwner(slot->native, &expected, nativeContext.data(),
                                                                                    nativeContext.size(), digest.data());
            status != SteamNetworkingSocketsLib::HoroBridgeStatus::Ok)
            return Result<AuthenticationDigest>::Failure(GnsSecurityFailure(status));
        return Result<AuthenticationDigest>::Success(std::bit_cast<AuthenticationDigest>(digest));
    }
}  // namespace Horo::Network
