#pragma once

/**
 * @file ReleaseProcess.h
 * @brief Bounded child-process adapter for release stage workers.
 */

#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Release/ReleasePipelineExecutor.h"

#include <functional>

namespace Horo::Release {
    /** @brief Shell-free release tool invocation without native capture-policy overrides. */
    struct ReleaseProcessRequest final {
        std::string executable;
        std::vector<std::string> arguments;
        std::filesystem::path workingDirectory;
        ProcessEnvironment environment;
        std::chrono::milliseconds timeout{std::chrono::minutes{15}};
    };

    /** @brief One process line with its immutable release-stage identity. */
    struct ReleaseProcessOutput final {
        ReleaseJobId job;
        ReleaseTargetId target;
        OperationId operation{};
        ReleaseStage stage{ReleaseStage::Validating};
        ReleaseStageAttemptId attempt;
        ProcessOutputLine line;
    };

    /** @brief Caps native process lifetime and output while preserving stage context. */
    class ReleaseProcessRunner final {
    public:
        /** @brief Borrows the host-selected portable process capability. @param processes Host-owned runner. */
        explicit ReleaseProcessRunner(IExternalProcessRunner &processes) noexcept;

        /**
         * @brief Runs a shell-free child on the calling worker under the stage deadline.
         * @param context Active release stage and its cancellation/deadline.
         * @param request Typed process invocation; native capture limits are release-owned.
         * @param onOutput Optional bounded line observer, invoked on the calling worker.
         * @return Zero-exit terminal result or a typed launch, timeout, or process failure.
         */
        [[nodiscard]] Result<ExternalProcessResult> Run(const ReleaseStageContext &context, ReleaseProcessRequest request,
                                                        std::function<void(ReleaseProcessOutput)> onOutput = {}) const;

    private:
        IExternalProcessRunner &processes_;
    };
}  // namespace Horo::Release
