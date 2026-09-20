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
    extern const ErrorCodeDescriptor VoiceInvalidTransition;
    extern const ErrorCodeDescriptor VoiceAdmissionClosed;
    extern const ErrorCodeDescriptor CapabilityUnavailable;
    extern const ErrorCodeDescriptor OperationUnsupported;
    extern const ErrorCodeDescriptor OperationCancelled;
    extern const ErrorCodeDescriptor RuntimeInactive;
    extern const ErrorCodeDescriptor DeviceUnavailable;
    extern const ErrorCodeDescriptor BackendFailed;
}  // namespace Horo::Audio::AudioErrors
