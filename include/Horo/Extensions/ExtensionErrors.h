#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Extensions::ExtensionErrors {
    extern const ErrorCodeDescriptor InvalidManifest;
    extern const ErrorCodeDescriptor LoadFailed;
    extern const ErrorCodeDescriptor MissingEntryPoint;
    extern const ErrorCodeDescriptor ContributionRejected;
    extern const ErrorCodeDescriptor InvocationFailed;
    extern const ErrorCodeDescriptor ModuleResolutionFailed;
    /** @brief A lifecycle transition used the wrong owner, ordering, or state evidence. */
    extern const ErrorCodeDescriptor LifecycleTransitionInvalid;
    /** @brief A lifecycle transition targeted a state revision that is no longer current. */
    extern const ErrorCodeDescriptor LifecycleRevisionStale;
    /** @brief A lifecycle revision, generation, message, or audit-history bound was exhausted. */
    extern const ErrorCodeDescriptor LifecycleCapacityExceeded;
    /** @brief Capability admission policy or manifest-derived request is malformed. */
    extern const ErrorCodeDescriptor CapabilityAdmissionInvalid;
    /** @brief A required permission is unknown, unapproved, or belongs to another activation. */
    extern const ErrorCodeDescriptor PermissionDenied;
    /** @brief A capability is absent from the host composition or exact admitted set. */
    extern const ErrorCodeDescriptor CapabilityUnavailable;
    /** @brief A retained capability handle outlived its activation admission. */
    extern const ErrorCodeDescriptor CapabilityRevoked;
    /** @brief Application capability registry input is malformed. */
    extern const ErrorCodeDescriptor CapabilityRegistryInvalid;
    /** @brief A provider already owns the exact capability contract version. */
    extern const ErrorCodeDescriptor CapabilityRegistryDuplicate;
    /** @brief No compatible provider contract version exists. */
    extern const ErrorCodeDescriptor CapabilityVersionIncompatible;
    /** @brief The bounded application capability registry is full. */
    extern const ErrorCodeDescriptor CapabilityRegistryCapacityExceeded;
    /** @brief The application capability registry is shutting down. */
    extern const ErrorCodeDescriptor CapabilityRegistryShutdown;
    /** @brief A backend-service descriptor or requested identity is malformed. */
    extern const ErrorCodeDescriptor BackendServiceInvalid;
    /** @brief A backend service identity already has a published provider. */
    extern const ErrorCodeDescriptor BackendServiceDuplicate;
    /** @brief No live backend service exists for the requested identity. */
    extern const ErrorCodeDescriptor BackendServiceUnavailable;
    /** @brief The resolved provider does not implement the requested stable contract. */
    extern const ErrorCodeDescriptor BackendServiceContractMismatch;
    /** @brief The caller's C++ contract type does not match the registered adapter. */
    extern const ErrorCodeDescriptor BackendServiceTypeMismatch;
    /** @brief A backend service was called outside its declared thread rule. */
    extern const ErrorCodeDescriptor BackendServiceThreadViolation;
    /** @brief A backend provider operation failed while preserving its typed cause. */
    extern const ErrorCodeDescriptor BackendServiceInvocationFailed;
    /** @brief A backend service operation was cooperatively cancelled. */
    extern const ErrorCodeDescriptor BackendServiceCancelled;
    /** @brief The bounded backend-service registry is full. */
    extern const ErrorCodeDescriptor BackendServiceCapacityExceeded;
    /** @brief Backend-service registration and invocation admission are closed. */
    extern const ErrorCodeDescriptor BackendServiceShutdown;
    /** @brief A project-validator descriptor, snapshot, finding, or limit is malformed. */
    extern const ErrorCodeDescriptor ProjectValidatorRegistryInvalid;
    /** @brief A project-validator identity already has a published provider. */
    extern const ErrorCodeDescriptor ProjectValidatorRegistryDuplicate;
    /** @brief The bounded project-validator provider registry is full. */
    extern const ErrorCodeDescriptor ProjectValidatorRegistryCapacityExceeded;
    /** @brief Project-validator registration and new validation admission are closed. */
    extern const ErrorCodeDescriptor ProjectValidatorRegistryShutdown;
    /** @brief One attributed project-validator callback or finding pass failed. */
    extern const ErrorCodeDescriptor ProjectValidatorInvocationFailed;
    /** @brief Project validation was cooperatively cancelled without publishing partial results. */
    extern const ErrorCodeDescriptor ProjectValidationCancelled;
    /** @brief A pipeline-step descriptor, artifact declaration, or input is malformed. */
    extern const ErrorCodeDescriptor PipelineStepRegistryInvalid;
    /** @brief A step identity or generated artifact producer is already published. */
    extern const ErrorCodeDescriptor PipelineStepRegistryDuplicate;
    /** @brief The bounded pipeline-step registry is full. */
    extern const ErrorCodeDescriptor PipelineStepRegistryCapacityExceeded;
    /** @brief Pipeline-step registration and new run admission are closed. */
    extern const ErrorCodeDescriptor PipelineStepRegistryShutdown;
    /** @brief The registered dependency graph is incomplete or violates phase ordering. */
    extern const ErrorCodeDescriptor PipelineGraphInvalid;
    /** @brief The registered pipeline-step dependency graph contains a cycle. */
    extern const ErrorCodeDescriptor PipelineGraphCycle;
    /** @brief A step tried to publish malformed, undeclared, duplicate, or oversized output. */
    extern const ErrorCodeDescriptor PipelineOutputInvalid;
    /** @brief One attributed pipeline-step callback failed. */
    extern const ErrorCodeDescriptor PipelineStepInvocationFailed;
    /** @brief A pipeline run was cancelled and discarded all staged outputs. */
    extern const ErrorCodeDescriptor PipelineRunCancelled;
    /** @brief A toolchain provider descriptor, authority, intent, or resolved process request is malformed. */
    extern const ErrorCodeDescriptor ToolchainProviderRegistryInvalid;
    /** @brief A toolchain provider contribution identity is already published. */
    extern const ErrorCodeDescriptor ToolchainProviderRegistryDuplicate;
    /** @brief The bounded toolchain provider registry is full. */
    extern const ErrorCodeDescriptor ToolchainProviderRegistryCapacityExceeded;
    /** @brief Toolchain provider registration and new invocation admission are closed. */
    extern const ErrorCodeDescriptor ToolchainProviderRegistryShutdown;
    /** @brief The exact requested toolchain provider generation is absent or revoked. */
    extern const ErrorCodeDescriptor ToolchainProviderUnavailable;
    /** @brief Host policy rejected a provider's logical invocation intent. */
    extern const ErrorCodeDescriptor ToolchainPolicyRejected;
    /** @brief An approved tool failed at the platform process boundary. */
    extern const ErrorCodeDescriptor ToolchainInvocationFailed;
    /** @brief An asset-cooker descriptor, request, target, or bound is malformed. */
    extern const ErrorCodeDescriptor AssetCookerRegistryInvalid;
    /** @brief An asset-cooker contribution identity is already published. */
    extern const ErrorCodeDescriptor AssetCookerRegistryDuplicate;
    /** @brief The bounded asset-cooker provider registry is full. */
    extern const ErrorCodeDescriptor AssetCookerRegistryCapacityExceeded;
    /** @brief Asset-cooker registration and new cook admission are closed. */
    extern const ErrorCodeDescriptor AssetCookerRegistryShutdown;
    /** @brief No registered cooker handles the requested asset type and target. */
    extern const ErrorCodeDescriptor AssetCookerUnavailable;
    /** @brief Multiple cookers claim the request and no exact project-policy choice was supplied. */
    extern const ErrorCodeDescriptor AssetCookerAmbiguous;
    /** @brief A cooker produced malformed, duplicate, incomplete, or oversized staged output. */
    extern const ErrorCodeDescriptor AssetCookerOutputInvalid;
    /** @brief One attributed asset-cooker callback failed. */
    extern const ErrorCodeDescriptor AssetCookerInvocationFailed;
    /** @brief An asset cook was cooperatively cancelled and discarded staged output. */
    extern const ErrorCodeDescriptor AssetCookCancelled;
}  // namespace Horo::Extensions::ExtensionErrors
