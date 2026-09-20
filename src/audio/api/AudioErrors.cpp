#include "Horo/Audio/AudioErrors.h"

namespace Horo::Audio::AudioErrors {
    namespace {
        const ErrorDomainId AudioDomain{"horo.audio"};
    }

    const ErrorCodeDescriptor EventQueueInvalid{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.event_queue.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio callback-to-control event queue or outcome identity is invalid.",
        .remediationHint = "Prepare bounded EventStorage and correlate terminal outcomes to exact accepted operations.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ResamplerInvalid{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.resampler.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio resampler request is invalid or outside supported bounds.",
        .remediationHint = "Provide supported rates, pitch, quality, channel and output-frame limits.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ResamplerBudgetExceeded{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.resampler.budget_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio resampler exceeds its admitted processing budget.",
        .remediationHint = "Reserve sufficient work, history and latency capacity before publication.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DspContractInvalid{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.dsp.contract_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio DSP node contract is structurally invalid.",
        .remediationHint = "Declare valid bounded ports, parameters, formats and real-time guarantees before activation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DspContractLimitExceeded{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.dsp.contract_limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio DSP node exceeds an admitted processing limit.",
        .remediationHint = "Reduce ports, state, scratch, latency, tail or block size to the active audio profile.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DspPreparationStorageInsufficient{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.dsp.preparation_storage_insufficient"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio DSP node has not been given its complete prepared storage.",
        .remediationHint = "Reserve the descriptor's state and scratch bytes before activation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AssetSchemaInvalid{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.asset_schema.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The persisted audio asset schema is structurally invalid.",
        .remediationHint = "Correct media bounds, stable identities, annotations, seek metadata or variation settings before cooking.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AssetSchemaVersionUnsupported{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.asset_schema.version_unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The persisted audio asset schema version is unsupported.",
        .remediationHint = "Migrate the asset through an Audio-owned supported schema path before cooking.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AssetSchemaLimitExceeded{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.asset_schema.limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The persisted audio asset schema exceeds an admitted bound.",
        .remediationHint = "Reduce annotation, seek, variation or label counts to the active validated profile.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor FormatRegistryInvalid{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.format_registry.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio format registry metadata is invalid.",
        .remediationHint = "Provide bounded container, codec and representation metadata with valid identities.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor FormatRegistryCapacityExceeded{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.format_registry.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio format registry exceeds its admitted capacity.",
        .remediationHint = "Reduce contributed metadata or explicitly admit larger bounded registry limits.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor FormatRegistryConflict{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.format_registry.conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio format registry contains conflicting identities or duplicate tuples.",
        .remediationHint = "Contribute each container, codec and exact representation tuple once.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor FormatContainerUnknown{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.format.container_unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio container identity is unknown.",
        .remediationHint = "Detect and register a supported container before resolving its codec.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor FormatCodecUnknown{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.format.codec_unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio codec identity is unknown.",
        .remediationHint = "Validate and register the codec independently of container detection.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor FormatCombinationUnsupported{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.format.combination_unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The exact audio container, codec and representation combination is unsupported.",
        .remediationHint = "Select an exact tuple declared by the active audio format registry.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SourceInvalid{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.source.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio source is malformed, truncated or internally contradictory.",
        .remediationHint = "Re-export a valid RIFF/WAVE PCM or Ogg/Vorbis source with complete bounded metadata.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SourceUnsupported{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.source.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio source container, codec or channel layout is unsupported.",
        .remediationHint = "Use an approved WAV/PCM or Ogg/Vorbis source, or install an admitted codec contribution.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SourceLimitExceeded{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.source.limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio source or decoded result exceeds its admitted resource envelope.",
        .remediationHint = "Reduce source duration, channels or analysis density, or explicitly admit larger qualified limits.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SourceReadFailed{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.source.read_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio source reader failed or violated its exact bounded read contract.",
        .remediationHint = "Keep the admitted source generation readable for the complete import invocation and retry.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor SourceDecodeFailed{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.source.decode_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The admitted audio decoder could not produce the declared frame stream.",
        .remediationHint = "Re-export the media or select a decoder contribution compatible with its exact codec tuple.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CommandBufferInvalid{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.command_buffer.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio command buffer reservation is invalid.",
        .remediationHint = "Choose a nonzero epoch, bounded power-of-two capacity and a positive smaller critical reserve.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor MemoryInvalid{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.memory.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio memory reservation is invalid.",
        .remediationHint = "Supply a valid runtime owner and bounded aligned memory dimensions.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor MemoryBudgetExceeded{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.memory.budget_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio memory reservation exceeds its admitted budget.",
        .remediationHint = "Reduce requested capacity or explicitly admit a supported memory profile before publication.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor MemoryAllocationFailed{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.memory.allocation_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Audio memory preparation could not allocate its backing storage.",
        .remediationHint = "Release quiescent resources before retrying preparation outside the callback.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor IdentityInvalid{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.identity.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio identity is invalid.",
        .remediationHint = "Provide a non-zero stable value or persistent asset identity from its owning registry.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor HandleMalformed{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.handle.malformed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio handle is malformed.",
        .remediationHint = "Use a handle issued by the active audio runtime registry.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor HandleOwnerMismatch{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.handle.owner_mismatch"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio handle belongs to another runtime generation.",
        .remediationHint = "Resolve the stable audio identity again after runtime replacement.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor HandleStale{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.handle.stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio handle generation is stale or retired.",
        .remediationHint = "Discard the handle and resolve the stable audio identity against the current runtime.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor HandleCapacityExhausted{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.handle.capacity_exhausted"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The bounded audio handle registry has no available slot.",
        .remediationHint = "Release inactive objects or increase the validated control-runtime capacity.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor HandleGenerationExhausted{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.handle.generation_exhausted"},
        .defaultSeverity = ErrorSeverity::Critical,
        .summary = "Every remaining audio handle slot exhausted its generation range.",
        .remediationHint = "Replace the audio runtime; exhausted slots are never reused.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor CapabilityUnavailable{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.capability.unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required audio capability is unavailable.",
        .remediationHint = "Select a composition that explicitly provides the required capability.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor OperationUnsupported{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.operation.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The selected audio composition does not support this operation.",
        .remediationHint = "Choose a supported operation or an audio provider that declares the capability.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor OperationCancelled{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.operation.cancelled"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "The audio operation was cancelled.",
        .remediationHint = "Retry only if the owning runtime and producer generation remain active.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor RuntimeInactive{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.runtime.inactive"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The audio runtime is not active.",
        .remediationHint = "Submit work only after successful runtime activation and before shutdown begins.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor DeviceUnavailable{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.device.unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The selected audio device is unavailable.",
        .remediationHint = "Re-enumerate through the active backend or select an explicitly supported composition.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BackendFailed{
        .domain = AudioDomain,
        .code = ErrorCode{"audio.backend.failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The selected audio backend failed.",
        .remediationHint = "Inspect the bounded backend diagnostic cause and follow host recovery policy.",
        .retryable = true,
        .userActionable = false,
    };
}  // namespace Horo::Audio::AudioErrors
