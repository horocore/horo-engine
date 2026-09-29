#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Release/UpdateActivation.h"
#include "Horo/Release/UpdateActivationErrors.h"

#include <algorithm>
#include <chrono>

namespace Horo::Release {
    /** @copydoc ProbeVerifiedUpdateEntrypoint */
    Result<void> ProbeVerifiedUpdateEntrypoint(const UpdateActivationVersion &version, const UpdateArchiveLimits &limits,
                                               const Security::ArtifactVerifier &verifier, IExternalProcessRunner &processes,
                                               const std::span<const std::string> arguments, const std::chrono::seconds timeout) {
        if (timeout <= std::chrono::seconds::zero() || timeout > std::chrono::minutes{5})
            return Result<void>::Failure(MakeError(UpdateActivationErrors::InvalidLayout));
        if (auto admitted = VerifyReadyUpdateStage(version.package, version.checkpoint, version.packageFile, version.stageRoot,
                                                   version.inventory, limits, verifier);
            admitted.HasError())
            return admitted;
        const auto entrypoint = std::ranges::find_if(version.inventory, [](const UpdateStagedFile &file) {
            return file.role == UpdateFileRole::Entrypoint;
        });
        if (entrypoint == version.inventory.end() || entrypoint->mode != UpdateFileMode::Executable)
            return Result<void>::Failure(MakeError(UpdateActivationErrors::InvalidLayout));

        ExternalProcessRequest request;
        request.executable = (version.stageRoot / std::filesystem::path(entrypoint->path)).string();
        request.workingDirectory = version.stageRoot;
        request.arguments.assign(arguments.begin(), arguments.end());
        request.environment.base = ProcessEnvironmentBase::Replace;
        request.timeout = timeout;
        request.gracefulTermination = std::chrono::milliseconds{500};
        request.maximumDrainDuration = std::chrono::seconds{1};
        request.maximumLineBytes = 4096U;
        request.maximumOutputBytes = 64U * 1024U;
        auto outcome = processes.Run(request, {});
        if (outcome.HasError())
            return Result<void>::Failure(outcome.ErrorValue());
        if (outcome.Value().reason != ProcessTerminationReason::Exited || outcome.Value().exitCode != 0)
            return Result<void>::Failure(MakeError(UpdateActivationErrors::HealthFailed));
        return Result<void>::Success();
    }
}  // namespace Horo::Release
