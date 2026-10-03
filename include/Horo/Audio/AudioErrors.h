#pragma once

/**
 * @file AudioErrors.h
 * @brief Stable backend-neutral audio failure identities.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Audio::AudioErrors {
    /** @brief An audio event queue, completion token or callback outcome is malformed. */
    extern const ErrorCodeDescriptor EventQueueInvalid;
    extern const ErrorCodeDescriptor ResamplerInvalid;
    extern const ErrorCodeDescriptor ResamplerBudgetExceeded;
    extern const ErrorCodeDescriptor DspContractInvalid;
    extern const ErrorCodeDescriptor DspContractLimitExceeded;
    extern const ErrorCodeDescriptor DspPreparationStorageInsufficient;
    extern const ErrorCodeDescriptor AssetSchemaInvalid;
    extern const ErrorCodeDescriptor AssetSchemaVersionUnsupported;
    extern const ErrorCodeDescriptor AssetSchemaLimitExceeded;
    extern const ErrorCodeDescriptor MixerAssetSchemaInvalid;
    extern const ErrorCodeDescriptor MixerAssetSchemaVersionUnsupported;
    extern const ErrorCodeDescriptor MixerAssetSchemaLimitExceeded;
    extern const ErrorCodeDescriptor MixerAssetSchemaMigrationFailed;
    extern const ErrorCodeDescriptor SoundReferenceInvalid;
    extern const ErrorCodeDescriptor SoundDefinitionInvalid;
    extern const ErrorCodeDescriptor SoundDefinitionVersionUnsupported;
    extern const ErrorCodeDescriptor PlaybackRequestInvalid;
    extern const ErrorCodeDescriptor FormatRegistryInvalid;
    extern const ErrorCodeDescriptor FormatRegistryCapacityExceeded;
    extern const ErrorCodeDescriptor FormatRegistryConflict;
    extern const ErrorCodeDescriptor FormatContainerUnknown;
    extern const ErrorCodeDescriptor FormatCodecUnknown;
    extern const ErrorCodeDescriptor FormatCombinationUnsupported;
    extern const ErrorCodeDescriptor SourceInvalid;
    extern const ErrorCodeDescriptor SourceUnsupported;
    extern const ErrorCodeDescriptor SourceLimitExceeded;
    extern const ErrorCodeDescriptor SourceReadFailed;
    extern const ErrorCodeDescriptor SourceDecodeFailed;
    extern const ErrorCodeDescriptor CookProfileInvalid;
    extern const ErrorCodeDescriptor CookCombinationUnsupported;
    extern const ErrorCodeDescriptor CookBudgetExceeded;
    extern const ErrorCodeDescriptor CookPayloadInvalid;
    extern const ErrorCodeDescriptor CommandBufferInvalid;
    extern const ErrorCodeDescriptor MemoryInvalid;
    extern const ErrorCodeDescriptor MemoryBudgetExceeded;
    extern const ErrorCodeDescriptor MemoryAllocationFailed;
    extern const ErrorCodeDescriptor IdentityInvalid;
    extern const ErrorCodeDescriptor HandleMalformed;
    extern const ErrorCodeDescriptor HandleOwnerMismatch;
    extern const ErrorCodeDescriptor HandleStale;
    extern const ErrorCodeDescriptor HandleCapacityExhausted;
    extern const ErrorCodeDescriptor HandleGenerationExhausted;
    /** @brief An authored concurrency group or bucket projection is malformed. */
    extern const ErrorCodeDescriptor ConcurrencyInvalid;
    /** @brief Cooldown history belongs to a retired timeline or lies in the future. */
    extern const ErrorCodeDescriptor ConcurrencyTimelineStale;
    extern const ErrorCodeDescriptor VoiceInvalidTransition;
    extern const ErrorCodeDescriptor VoiceAdmissionClosed;
    extern const ErrorCodeDescriptor CapabilityUnavailable;
    extern const ErrorCodeDescriptor OperationUnsupported;
    extern const ErrorCodeDescriptor OperationCancelled;
    extern const ErrorCodeDescriptor RuntimeInactive;
    extern const ErrorCodeDescriptor DeviceUnavailable;
    extern const ErrorCodeDescriptor BackendFailed;
    extern const ErrorCodeDescriptor StreamUnderrun;            /**< Bounded stream fill missed its deadline. */
    extern const ErrorCodeDescriptor StreamReadFailed;          /**< Stream source I/O failed outside the callback. */
    extern const ErrorCodeDescriptor StreamCapacityExceeded;    /**< Stream preparation exceeded an admitted bound. */
    extern const ErrorCodeDescriptor QueueSaturated;            /**< Ordinary queue admission failed without using critical reserve. */
    extern const ErrorCodeDescriptor GraphBuildFailed;          /**< A candidate graph could not be prepared. */
    extern const ErrorCodeDescriptor GraphStaleCandidate;       /**< Candidate graph revision became stale. */
    extern const ErrorCodeDescriptor ProviderUnavailable;       /**< Selected provider is absent at preflight. */
    extern const ErrorCodeDescriptor ProviderFailed;            /**< Active provider reported failure. */
    extern const ErrorCodeDescriptor MiddlewareBindingInvalid;  /**< Candidate middleware binding is invalid. */
    extern const ErrorCodeDescriptor MiddlewareBankUnavailable; /**< Required candidate bank is missing. */
    extern const ErrorCodeDescriptor MiddlewareFailed;          /**< Selected middleware output backend failed. */
    extern const ErrorCodeDescriptor DeviceFormatUnsupported;   /**< Candidate output tuple is not admitted. */
    extern const ErrorCodeDescriptor DeviceLost;                /**< Active output device was lost. */
    extern const ErrorCodeDescriptor CallbackFault;             /**< Callback invariant or deadline fault was drained. */
}  // namespace Horo::Audio::AudioErrors
