#pragma once

/**
 * @file UpdateActivation.h
 * @brief Locked, recoverable transition between verified update versions.
 */

#include "Horo/Foundation/Platform.h"
#include "Horo/Release/UpdateStageReady.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Horo {
    class IExternalProcessRunner;
}

namespace Horo::Release {
    /** @brief Complete immutable evidence for one installed version. */
    struct UpdateActivationVersion final {
        UpdatePackageRecord package;
        UpdateTransferCheckpoint checkpoint;
        std::filesystem::path packageFile;
        std::filesystem::path stageRoot;
        std::vector<UpdateStagedFile> inventory;
    };

    /** @brief Two versions belonging to one installation and its protected root. */
    struct UpdateActivationRequest final {
        std::filesystem::path installationRoot;
        UpdateActivationVersion current;
        UpdateActivationVersion staged;
        UpdateArchiveLimits archiveLimits;
        std::chrono::seconds healthTimeout{30};
    };

    /** @brief Host-owned process coordination while the activation lock is held. */
    class IUpdateActivationHost {
    public:
        virtual ~IUpdateActivationHost() = default;

        /** @brief Blocks new product launches and waits for all users of the installation to exit. */
        [[nodiscard]] virtual Result<void> EnsureProductsStopped(const std::filesystem::path &installationRoot) = 0;
        /** @brief Runs and completes a bounded startup probe, leaving no child process behind. */
        [[nodiscard]] virtual Result<void> ProbeStartupHealth(const std::filesystem::path &versionRoot, std::chrono::seconds timeout) = 0;
    };

    /** @brief Activation committed or a prior interrupted transition safely restored. */
    enum class UpdateActivationOutcome : std::uint8_t {
        Activated,
        RecoveredPrevious
    };

    /**
     * @brief Encodes a canonical active pointer for initial installation provisioning.
     * @param package Signed package whose ID and digest identify the active version.
     * @return Bounded canonical record or invalid identity failure.
     */
    [[nodiscard]] Result<std::string> EncodeActiveUpdateRecord(const UpdatePackageRecord &package);

    /**
     * @brief Reauthenticates a staged version and runs its signed entrypoint as a bounded, shell-free health probe.
     * @param version Immutable package, stage, and inventory evidence held quiescent by the host for the entire call.
     * @param limits Archive limits used when the stage was admitted.
     * @param verifier Trusted publisher signature verifier.
     * @param processes Native process runner owned by the application composition root.
     * @param arguments Product-specific health arguments selected by the trusted host, not parsed from the package.
     * @param timeout Positive maximum child runtime before the process runner starts bounded termination and pipe draining.
     * @return Success only for a clean zero exit; otherwise authentication, launch, or health failure.
     * @note The host must independently block new product launches and stop the running product before invoking this probe.
     */
    [[nodiscard]] Result<void> ProbeVerifiedUpdateEntrypoint(const UpdateActivationVersion &version, const UpdateArchiveLimits &limits,
                                                             const Security::ArtifactVerifier &verifier, IExternalProcessRunner &processes,
                                                             std::span<const std::string> arguments, std::chrono::seconds timeout);

    /**
     * @brief Verifies both versions under an installation lock and switches the active pointer atomically.
     * @param request Existing and staged version evidence; both trees remain immutable through this call.
     * @param files Native durable filesystem that owns the process-wide and OS installation lock.
     * @param verifier Trusted publisher signature verifier for both packages.
     * @param host Process-exit gate and bounded startup-health probe; retained by the caller.
     * @return Activated or recovered prior version; failure leaves a durable recovery journal if rollback is uncertain.
     * @note A healthy activation atomically pins the verified previous version as last-known-good. Recovery restores the prior pin.
     * @note This blocking operation belongs in a separate updater helper after the product hands off and exits.
     */
    [[nodiscard]] Result<UpdateActivationOutcome> ActivateVerifiedUpdate(const UpdateActivationRequest &request,
                                                                         NativeDurableFileSystem &files,
                                                                         const Security::ArtifactVerifier &verifier,
                                                                         IUpdateActivationHost &host);
}  // namespace Horo::Release
