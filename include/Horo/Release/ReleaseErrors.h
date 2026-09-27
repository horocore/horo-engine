#pragma once

/**
 * @file ReleaseErrors.h
 * @brief Stable backend-neutral release-domain failure identities.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Release::ReleaseErrors {
    /** @brief Semantic version text is invalid, ambiguous, non-canonical, or too large. */
    extern const ErrorCodeDescriptor VersionInvalid;
    /** @brief Release tag does not contain one lowercase `v` plus canonical SemVer. */
    extern const ErrorCodeDescriptor TagInvalid;
    /** @brief Authority input is incomplete, malformed, or outside its bounded contract. */
    extern const ErrorCodeDescriptor AuthorityInvalid;
    /** @brief Duplicate sources or exact version identities conflict. */
    extern const ErrorCodeDescriptor AuthorityConflict;
    /** @brief Engine, game, and persistent-contract claim kinds were mixed. */
    extern const ErrorCodeDescriptor ProductKindMismatch;
    /** @brief Engine core or prerelease conflicts with durable compatibility metadata. */
    extern const ErrorCodeDescriptor PersistentContractMismatch;
    /** @brief A distribution product, version, build, package, or installation identity is malformed. */
    extern const ErrorCodeDescriptor DistributionIdentityInvalid;
    /** @brief A product, platform, artifact class, and package format combination is unsupported. */
    extern const ErrorCodeDescriptor DistributionCombinationUnsupported;
    /** @brief A release profile catalog or preset contains malformed or incomplete policy. */
    extern const ErrorCodeDescriptor ProfileInvalid;
    /** @brief Release profile inheritance or overrides produce a deterministic conflict. */
    extern const ErrorCodeDescriptor ProfileConflict;
    /** @brief A release profile requires a capability unavailable to the resolving host. */
    extern const ErrorCodeDescriptor ProfileCapabilityUnsupported;
    /** @brief A release profile catalog exceeds an explicit resource ceiling. */
    extern const ErrorCodeDescriptor ProfileLimitExceeded;
    /** @brief A release job, stage, candidate or terminal transition is forbidden. */
    extern const ErrorCodeDescriptor PipelineTransitionInvalid;
    /** @brief Frozen release inputs changed before a stage consumed them. */
    extern const ErrorCodeDescriptor PipelineInputChanged;
    /** @brief A stage returned an incomplete or contradictory typed output. */
    extern const ErrorCodeDescriptor PipelineOutputInvalid;
    /** @brief A candidate stage or final path already belongs to another attempt. */
    extern const ErrorCodeDescriptor PipelineOutputCollision;
    /** @brief A private stage could not be durably assembled or promoted. */
    extern const ErrorCodeDescriptor PipelineStagingIoFailed;
    /** @brief A stage worker threw across the release execution boundary. */
    extern const ErrorCodeDescriptor PipelineStageException;
    /** @brief A stage exceeded its service-owned deadline. */
    extern const ErrorCodeDescriptor PipelineStageTimeout;
    /** @brief A bounded release child process failed to exit successfully. */
    extern const ErrorCodeDescriptor PipelineProcessFailed;
    /** @brief A release child process stopped after an accepted cancellation. */
    extern const ErrorCodeDescriptor PipelineProcessCancelled;
    /** @brief The release service or its operation queue has no admission capacity. */
    extern const ErrorCodeDescriptor PipelineAdmissionRejected;
}  // namespace Horo::Release::ReleaseErrors
