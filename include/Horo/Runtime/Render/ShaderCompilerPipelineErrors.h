#pragma once

/**
 * @file ShaderCompilerPipelineErrors.h
 * @brief Stable typed failures for offline shader compilation orchestration.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::ShaderCompilerPipelineErrors {
    extern const ErrorCodeDescriptor InvalidLimits;         /**< @brief Caller bounds are zero or exceed hard safety limits. */
    extern const ErrorCodeDescriptor InvalidRequest;        /**< @brief Input source or target data is malformed or incomplete. */
    extern const ErrorCodeDescriptor NonCanonicalInput;     /**< @brief Dependencies, defines, or targets are not unique and sorted. */
    extern const ErrorCodeDescriptor UnsupportedTarget;     /**< @brief A target route conflicts with its backend baseline. */
    extern const ErrorCodeDescriptor UnpinnedToolchain;     /**< @brief A required exact tool release/build identity is absent. */
    extern const ErrorCodeDescriptor CancellationRequested; /**< @brief The owning operation cancelled before the batch completed. */
    extern const ErrorCodeDescriptor AdapterFailure;        /**< @brief The selected private toolchain adapter failed one target. */
    extern const ErrorCodeDescriptor
        InvalidAdapterOutput; /**< @brief Adapter output is mismatched, empty, or outside the declared bounds. */
    extern const ErrorCodeDescriptor ArtifactIdentityUnavailable;   /**< @brief Artifact-key storage could not be allocated. */
    extern const ErrorCodeDescriptor AllocationFailed;              /**< @brief Pipeline result storage could not be allocated. */
    extern const ErrorCodeDescriptor ToolchainConfigurationInvalid; /**< @brief Host toolchain configuration is malformed. */
    extern const ErrorCodeDescriptor ToolNotApproved;               /**< @brief Tool identity is absent from the reviewed lock catalog. */
    extern const ErrorCodeDescriptor ToolMissing;                   /**< @brief A required host executable is absent or unreadable. */
    extern const ErrorCodeDescriptor ToolDigestMismatch;            /**< @brief Executable bytes do not match the reviewed lock. */
    extern const ErrorCodeDescriptor ScratchIoFailed;               /**< @brief Isolated compiler scratch I/O failed. */
    extern const ErrorCodeDescriptor ToolProcessFailed;             /**< @brief A compiler process did not exit successfully. */
    extern const ErrorCodeDescriptor ToolOutputInvalid;             /**< @brief A compiler output is absent, malformed, or over budget. */
}  // namespace Horo::Render::ShaderCompilerPipelineErrors
