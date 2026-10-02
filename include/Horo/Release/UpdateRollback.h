#pragma once

/**
 * @file UpdateRollback.h
 * @brief Policy-gated, verified return to a retained previous product version.
 */

#include "Horo/Release/UpdateActivation.h"

#include <optional>

namespace Horo::Release {
    /** @brief Origin of a rollback request; explicit downgrades require a user acknowledgement. */
    enum class UpdateRollbackReason : std::uint8_t {
        FailedActivation,
        ExplicitUserRequest
    };

    /** @brief Host-owned policy decision made before invoking the updater helper. */
    enum class UpdateRollbackAuthority : std::uint8_t {
        NormalPolicy,
        AdministratorRecovery
    };

    /** @brief Exact current and retained versions plus the host's downgrade policy snapshot. */
    struct UpdateRollbackRequest final {
        UpdateActivationRequest activation;
        UpdateRollbackReason reason{UpdateRollbackReason::FailedActivation};
        UpdateRollbackAuthority authority{UpdateRollbackAuthority::NormalPolicy};
        std::optional<ReleaseProductVersion> minimumAllowedVersion;
        bool explicitWarningAcknowledged{};
    };

    /**
     * @brief Validates downgrade intent and floor, then reuses authenticated activation and its recovery journal.
     * @param request Current version in activation.current and retained prior version in activation.staged.
     * @param files Durable filesystem that owns the installation lock.
     * @param verifier Installed publisher signature verifier.
     * @param host Product-exit gate and bounded startup-health probe.
     * @return Activated or recovered-previous outcome; failures preserve the original active version where provable.
     * @note AdministratorRecovery must be supplied only after independent host authorization; this API does not authenticate a user.
     */
    [[nodiscard]] Result<UpdateActivationOutcome> RollbackVerifiedUpdate(const UpdateRollbackRequest &request,
                                                                         NativeDurableFileSystem &files,
                                                                         const Security::ArtifactVerifier &verifier,
                                                                         IUpdateActivationHost &host);
}  // namespace Horo::Release
