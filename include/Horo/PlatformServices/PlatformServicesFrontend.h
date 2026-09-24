#pragma once

/**
 * @file PlatformServicesFrontend.h
 * @brief Backend-neutral Platform Services capability routing and admission validation.
 */

#include "Horo/PlatformServices/PlatformServicesBackend.h"

#include <array>
#include <memory>
#include <optional>

namespace Horo::PlatformServices {
    /** @brief Frozen product policy that may deny otherwise available service operations. */
    struct PlatformServicesOperationPolicy final {
        std::array<bool, static_cast<std::size_t>(PlatformServiceKind::Count)> deniedServices{};
    };

    /** @brief Stable errors owned by frontend composition and pre-admission validation. */
    namespace FrontendErrors {
        /** @brief Frontend composition inputs are missing, malformed, or generation-incompatible. */
        extern const ErrorCodeDescriptor InvalidComposition;
        /** @brief The frontend has closed admission before backend shutdown. */
        extern const ErrorCodeDescriptor Unavailable;
        /** @brief A request contains an invalid stable identity, bound, or payload. */
        extern const ErrorCodeDescriptor InvalidRequest;
        /** @brief Frozen product policy denies the requested service. */
        extern const ErrorCodeDescriptor OperationDenied;
        /** @brief Compatibility alias for the canonical explicit-Null rejection descriptor. */
        extern const ErrorCodeDescriptor &NullProvider;
        /** @brief A backend returned successful but malformed request identity evidence. */
        extern const ErrorCodeDescriptor InvalidDispatchResult;
    }  // namespace FrontendErrors

    /**
     * @brief Sole backend-neutral request surface for one active provider and session generation.
     * @details The frontend owns a strong backend lease and copied immutable capability/session snapshots. Rejected requests never
     *          invoke the backend. Close stops admission before invoking backend shutdown and is idempotent. Routing and Close are
     *          serialized by the composition owner's lane; concurrent entry is not supported by this slice.
     */
    class PlatformServicesFrontend final {
    public:
        /**
         * @brief Creates one generation-fenced frontend over an already activated backend.
         * @param backend Strong lease to the active backend; retained for the complete frontend lifetime.
         * @param capabilities Exact validated capability snapshot returned by backend activation.
         * @param session Immutable session snapshot for the same provider generation.
         * @param policy Frozen product operation policy.
         * @return Frontend or typed composition/inspection failure.
         */
        [[nodiscard]] static Result<PlatformServicesFrontend> Create(std::shared_ptr<IPlatformServicesBackend> backend,
                                                                     PlatformServiceCapabilitySnapshot capabilities,
                                                                     PlatformSessionSnapshot session,
                                                                     PlatformServicesOperationPolicy policy = {});

        PlatformServicesFrontend(const PlatformServicesFrontend &) = delete;
        PlatformServicesFrontend &operator=(const PlatformServicesFrontend &) = delete;
        ~PlatformServicesFrontend();
        PlatformServicesFrontend(PlatformServicesFrontend &&other) noexcept;
        PlatformServicesFrontend &operator=(PlatformServicesFrontend &&) noexcept = delete;

        /** @brief Validates and routes one achievement unlock. @param request Owned typed intent. @return Backend request handle or
         * pre-admission failure. */
        [[nodiscard]] Result<PlatformRequestHandle<void>> UnlockAchievement(AchievementUnlockRequest request) const;
        /** @brief Validates and routes one leaderboard score submission. @param request Owned typed intent. @return Backend request
         * handle or pre-admission failure. */
        [[nodiscard]] Result<PlatformRequestHandle<void>> SubmitScore(LeaderboardScoreRequest request) const;
        /** @brief Validates and routes one persistent-stat write. @param request Owned typed intent. @return Backend request handle or
         * pre-admission failure. */
        [[nodiscard]] Result<PlatformRequestHandle<void>> WriteStat(StatWriteRequest request) const;
        /** @brief Validates and routes one cloud object read. @param request Owned typed address. @return Backend request handle or
         * pre-admission failure. */
        [[nodiscard]] Result<PlatformRequestHandle<CloudReadResult>> ReadCloudObject(CloudReadRequest request) const;
        /** @brief Validates and routes one bounded cloud object write. @param request Owned typed payload. @return Backend request handle
         * or pre-admission failure. */
        [[nodiscard]] Result<PlatformRequestHandle<void>> WriteCloudObject(CloudWriteRequest request) const;
        /** @brief Validates and routes one bounded presence update. @param request Owned typed presence. @return Backend request handle
         * or pre-admission failure. */
        [[nodiscard]] Result<PlatformRequestHandle<void>> SetPresence(PresenceUpdateRequest request) const;
        /** @brief Validates and routes one presence clear. @param subject Current opaque subject capability. @return Backend request
         * handle or pre-admission failure. */
        [[nodiscard]] Result<PlatformRequestHandle<void>> ClearPresence(PlatformSubjectHandle subject) const;
        /** @brief Validates and routes one bounded friends query. @param query Owned typed page query. @return Backend request handle or
         * pre-admission failure. */
        [[nodiscard]] Result<PlatformRequestHandle<FriendsPage>> QueryFriends(FriendsQuery query) const;
        /** @brief Routes one current-session query without requiring an already active subject. @return Backend request handle or
         * pre-admission failure. */
        [[nodiscard]] Result<PlatformRequestHandle<PlatformSessionSnapshot>> QueryCurrentSession() const;

        /**
         * @brief Returns public finite limits for one available policy-admitted service without exposing its private binding.
         * @param service Known Horo service kind.
         * @return Copied limits or a typed lifecycle, policy, or availability failure.
         */
        [[nodiscard]] Result<PlatformServiceLimits> ServiceLimits(PlatformServiceKind service) const;

        /**
         * @brief Idempotently closes admission before invoking backend shutdown exactly once.
         * @return The first shutdown result; repeated calls preserve an earlier failure without reinvocation.
         */
        [[nodiscard]] Result<void> Close();
        /** @brief Reports whether new request admission remains open. @return True before Close begins. */
        [[nodiscard]] bool IsOpen() const noexcept;

    private:
        PlatformServicesFrontend(std::shared_ptr<IPlatformServicesBackend> backend, PlatformServiceCapabilitySnapshot capabilities,
                                 PlatformSessionSnapshot session, PlatformServicesOperationPolicy policy) noexcept;

        [[nodiscard]] Result<const PlatformServiceCapability *> ValidateService(PlatformServiceKind service) const;
        [[nodiscard]] Result<const PlatformServiceCapability *> ValidateSubjectService(PlatformServiceKind service,
                                                                                       const PlatformSubjectHandle &subject) const;
        /** @brief Shares stable-identity and subject admission for leaderboard/stat requests. */
        [[nodiscard]] Result<void> ValidateLeaderboardOrStat(bool identityValid, const PlatformSubjectHandle &subject) const;

        std::shared_ptr<IPlatformServicesBackend> backend_;
        PlatformServiceCapabilitySnapshot capabilities_;
        PlatformSessionSnapshot session_;
        PlatformServicesOperationPolicy policy_;
        bool open_{true};
        bool shutdownInvoked_{};
        std::optional<Error> closeError_;
    };
}  // namespace Horo::PlatformServices
