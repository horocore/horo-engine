#include "Horo/PlatformServices/PlatformServicesBackend.h"

#include <algorithm>
#include <utility>

namespace Horo::PlatformServices {
    namespace {
        constexpr std::uint64_t NullProviderIdentity = 0x4e554c4cULL;

        /** @brief Returns the stable explicit-Null rejection for any typed service request. */
        template <typename T> [[nodiscard]] Result<T> NullProviderFailure() {
            return Result<T>::Failure(MakeError(BackendErrors::NullProvider));
        }
    }  // namespace

    /** @copydoc NullPlatformServicesBackend::NullPlatformServicesBackend */
    NullPlatformServicesBackend::NullPlatformServicesBackend(const PlatformProviderGeneration generation) noexcept
        : generation_(generation) {}

    /** @copydoc IPlatformServicesBackend::InspectCapabilities */
    Result<PlatformServiceCapabilitySnapshot> NullPlatformServicesBackend::InspectCapabilities() const {
        if (!generation_.IsValid())
            return Result<PlatformServiceCapabilitySnapshot>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));

        PlatformServiceCapabilitySnapshot snapshot{.interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                        PlatformServicesBackendInterfaceMinor},
                                                   .provider = {NullProviderIdentity},
                                                   .providerGeneration = generation_};
        for (std::size_t index = 0; index < snapshot.services.size(); ++index) {
            snapshot.services[index] = {.service = static_cast<PlatformServiceKind>(index),
                                        .availability = PlatformServiceAvailability::Unavailable,
                                        .limits = {},
                                        .binding = std::nullopt,
                                        .unavailableReason = PlatformServiceUnavailableReason::NullProviderSelected};
        }
        return Result<PlatformServiceCapabilitySnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc IPlatformServicesBackend::Activate */
    Result<void> NullPlatformServicesBackend::Activate(const PlatformServicesBackendConfig &config) {
        if (!generation_.IsValid())
            return Result<void>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));
        if (std::ranges::any_of(config.requiredServices, [](const bool required) {
            return required;
        }))
            return Result<void>::Failure(MakeError(BackendErrors::RequiredServiceUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc IPlatformServicesBackend::RequestCancel */
    Result<void> NullPlatformServicesBackend::RequestCancel(PlatformRequestId, PlatformRequestGeneration) {
        return Result<void>::Success();
    }

    /** @copydoc IPlatformServicesBackend::Shutdown */
    Result<void> NullPlatformServicesBackend::Shutdown() {
        return Result<void>::Success();
    }

    /** @copydoc IAchievementService::UnlockAchievement */
    Result<PlatformRequestHandle<void>> NullPlatformServicesBackend::UnlockAchievement(AchievementUnlockRequest) {
        return NullProviderFailure<PlatformRequestHandle<void>>();
    }

    /** @copydoc ILeaderboardStatService::SubmitScore */
    Result<PlatformRequestHandle<void>> NullPlatformServicesBackend::SubmitScore(LeaderboardScoreRequest) {
        return NullProviderFailure<PlatformRequestHandle<void>>();
    }

    /** @copydoc ILeaderboardStatService::WriteStat */
    Result<PlatformRequestHandle<void>> NullPlatformServicesBackend::WriteStat(StatWriteRequest) {
        return NullProviderFailure<PlatformRequestHandle<void>>();
    }

    /** @copydoc ICloudService::ReadCloudObject */
    Result<PlatformRequestHandle<CloudReadResult>> NullPlatformServicesBackend::ReadCloudObject(CloudReadRequest) {
        return NullProviderFailure<PlatformRequestHandle<CloudReadResult>>();
    }

    /** @copydoc ICloudService::WriteCloudObject */
    Result<PlatformRequestHandle<void>> NullPlatformServicesBackend::WriteCloudObject(CloudWriteRequest) {
        return NullProviderFailure<PlatformRequestHandle<void>>();
    }

    /** @copydoc IPresenceService::SetPresence */
    Result<PlatformRequestHandle<void>> NullPlatformServicesBackend::SetPresence(PresenceUpdateRequest) {
        return NullProviderFailure<PlatformRequestHandle<void>>();
    }

    /** @copydoc IPresenceService::ClearPresence */
    Result<PlatformRequestHandle<void>> NullPlatformServicesBackend::ClearPresence(PlatformSubjectHandle) {
        return NullProviderFailure<PlatformRequestHandle<void>>();
    }

    /** @copydoc IFriendsService::QueryFriends */
    Result<PlatformRequestHandle<FriendsPage>> NullPlatformServicesBackend::QueryFriends(FriendsQuery) {
        return NullProviderFailure<PlatformRequestHandle<FriendsPage>>();
    }

    /** @copydoc ISessionService::QueryCurrentSession */
    Result<PlatformRequestHandle<PlatformSessionSnapshot>> NullPlatformServicesBackend::QueryCurrentSession() {
        return NullProviderFailure<PlatformRequestHandle<PlatformSessionSnapshot>>();
    }
}  // namespace Horo::PlatformServices
