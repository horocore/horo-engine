#include "Horo/Audio/AudioFailureRecovery.h"

#include "Horo/Audio/AudioErrors.h"

#include <array>

namespace Horo::Audio {
    namespace {
        using Area = AudioFailureArea;
        using State = AudioFailureState;
        using Action = AudioRecoveryAction;
        using Active = AudioActiveStatePolicy;

        struct Rule final {
            const ErrorCodeDescriptor *descriptor;
            AudioFailureDecision decision;
        };

        constexpr Rule Reject(const ErrorCodeDescriptor &descriptor, const Area area, const Action action) noexcept {
            return {&descriptor, {area, State::Rejected, action, Active::Preserve, true}};
        }

        constexpr Rule Recover(const ErrorCodeDescriptor &descriptor, const Area area, const Action action,
                               const Active active = Active::Preserve) noexcept {
            return {&descriptor, {area, State::Recovering, action, active, true}};
        }

        constexpr Rule Fail(const ErrorCodeDescriptor &descriptor, const Area area) noexcept {
            return {&descriptor, {area, State::Failed, Action::RequestHostPolicy, Active::RetainUntilDetached, true}};
        }

        const auto Rules = std::to_array<Rule>({
            Reject(AudioErrors::EventQueueInvalid, Area::Queue, Action::CorrectInput),
            Reject(AudioErrors::ResamplerInvalid, Area::Graph, Action::CorrectInput),
            Reject(AudioErrors::ResamplerBudgetExceeded, Area::Graph, Action::RebuildCandidate),
            Reject(AudioErrors::DspContractInvalid, Area::Graph, Action::CorrectInput),
            Reject(AudioErrors::DspContractLimitExceeded, Area::Graph, Action::RebuildCandidate),
            Reject(AudioErrors::DspPreparationStorageInsufficient, Area::Graph, Action::RebuildCandidate),
            Reject(AudioErrors::AssetSchemaInvalid, Area::Asset, Action::CorrectInput),
            Reject(AudioErrors::AssetSchemaVersionUnsupported, Area::Asset, Action::CorrectInput),
            Reject(AudioErrors::AssetSchemaLimitExceeded, Area::Asset, Action::CorrectInput),
            Reject(AudioErrors::MixerAssetSchemaInvalid, Area::Graph, Action::CorrectInput),
            Reject(AudioErrors::MixerAssetSchemaVersionUnsupported, Area::Graph, Action::CorrectInput),
            Reject(AudioErrors::MixerAssetSchemaLimitExceeded, Area::Graph, Action::CorrectInput),
            Reject(AudioErrors::MixerAssetSchemaMigrationFailed, Area::Graph, Action::CorrectInput),
            Reject(AudioErrors::SoundReferenceInvalid, Area::Asset, Action::CorrectInput),
            Reject(AudioErrors::SoundDefinitionInvalid, Area::Asset, Action::CorrectInput),
            Reject(AudioErrors::SoundDefinitionVersionUnsupported, Area::Asset, Action::CorrectInput),
            Reject(AudioErrors::PlaybackRequestInvalid, Area::Voice, Action::CorrectInput),
            Reject(AudioErrors::FormatRegistryInvalid, Area::Codec, Action::CorrectInput),
            Reject(AudioErrors::FormatRegistryCapacityExceeded, Area::Codec, Action::ReleaseCapacity),
            Reject(AudioErrors::FormatRegistryConflict, Area::Codec, Action::CorrectInput),
            Reject(AudioErrors::FormatContainerUnknown, Area::Codec, Action::CorrectInput),
            Reject(AudioErrors::FormatCodecUnknown, Area::Codec, Action::CorrectInput),
            Reject(AudioErrors::FormatCombinationUnsupported, Area::Codec, Action::CorrectInput),
            Reject(AudioErrors::SourceInvalid, Area::Asset, Action::CorrectInput),
            Reject(AudioErrors::SourceUnsupported, Area::Codec, Action::CorrectInput),
            Reject(AudioErrors::SourceLimitExceeded, Area::Asset, Action::ReleaseCapacity),
            Reject(AudioErrors::SourceReadFailed, Area::Asset, Action::RetryAtSafePoint),
            Reject(AudioErrors::SourceDecodeFailed, Area::Codec, Action::CorrectInput),
            Reject(AudioErrors::CookProfileInvalid, Area::Asset, Action::CorrectInput),
            Reject(AudioErrors::CookCombinationUnsupported, Area::Codec, Action::CorrectInput),
            Reject(AudioErrors::CookBudgetExceeded, Area::Asset, Action::ReleaseCapacity),
            Reject(AudioErrors::CookPayloadInvalid, Area::Asset, Action::RebuildCandidate),
            Reject(AudioErrors::CommandBufferInvalid, Area::Queue, Action::CorrectInput),
            Reject(AudioErrors::MemoryInvalid, Area::Memory, Action::CorrectInput),
            Reject(AudioErrors::MemoryBudgetExceeded, Area::Memory, Action::ReleaseCapacity),
            Reject(AudioErrors::MemoryAllocationFailed, Area::Memory, Action::RetryAtSafePoint),
            Reject(AudioErrors::IdentityInvalid, Area::Runtime, Action::CorrectInput),
            Reject(AudioErrors::HandleMalformed, Area::Runtime, Action::ResolveIdentity),
            Reject(AudioErrors::HandleOwnerMismatch, Area::Runtime, Action::ResolveIdentity),
            Reject(AudioErrors::HandleStale, Area::Runtime, Action::ResolveIdentity),
            Reject(AudioErrors::HandleCapacityExhausted, Area::Runtime, Action::ReleaseCapacity),
            Recover(AudioErrors::HandleGenerationExhausted, Area::Runtime, Action::ReplaceRuntime),
            Reject(AudioErrors::VoiceInvalidTransition, Area::Voice, Action::CorrectInput),
            Reject(AudioErrors::VoiceAdmissionClosed, Area::Voice, Action::ResolveIdentity),
            Reject(AudioErrors::CapabilityUnavailable, Area::Provider, Action::RequestHostPolicy),
            Reject(AudioErrors::OperationUnsupported, Area::Runtime, Action::CorrectInput),
            {&AudioErrors::OperationCancelled, {Area::Runtime, State::Cancelled, Action::None, Active::Preserve, true}},
            Reject(AudioErrors::RuntimeInactive, Area::Runtime, Action::ResolveIdentity),
            Recover(AudioErrors::DeviceUnavailable, Area::Device, Action::ReopenDevice, Active::QuiesceDevice),
            Recover(AudioErrors::BackendFailed, Area::Device, Action::RequestHostPolicy, Active::QuiesceDevice),
            Recover(AudioErrors::StreamUnderrun, Area::Stream, Action::RefillStream),
            Recover(AudioErrors::StreamReadFailed, Area::Stream, Action::RefillStream),
            Reject(AudioErrors::StreamCapacityExceeded, Area::Stream, Action::ReleaseCapacity),
            Reject(AudioErrors::QueueSaturated, Area::Queue, Action::RetryAtSafePoint),
            Reject(AudioErrors::GraphBuildFailed, Area::Graph, Action::RebuildCandidate),
            Reject(AudioErrors::GraphStaleCandidate, Area::Graph, Action::RebuildCandidate),
            Reject(AudioErrors::ProviderUnavailable, Area::Provider, Action::RequestHostPolicy),
            Recover(AudioErrors::ProviderFailed, Area::Provider, Action::RequestHostPolicy),
            Reject(AudioErrors::MiddlewareBindingInvalid, Area::Middleware, Action::CorrectInput),
            Reject(AudioErrors::MiddlewareBankUnavailable, Area::Middleware, Action::RebuildCandidate),
            Recover(AudioErrors::MiddlewareFailed, Area::Middleware, Action::RequestHostPolicy, Active::QuiesceDevice),
            Reject(AudioErrors::DeviceFormatUnsupported, Area::Device, Action::CorrectInput),
            Recover(AudioErrors::DeviceLost, Area::Device, Action::ReopenDevice, Active::QuiesceDevice),
            Fail(AudioErrors::CallbackFault, Area::Runtime),
        });

        [[nodiscard]] bool Matches(const Error &error, const ErrorCodeDescriptor &descriptor) noexcept {
            return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
        }
    }  // namespace

    /** @copydoc ClassifyAudioFailure */
    AudioFailureDecision ClassifyAudioFailure(const Error &error, const AudioFailureOrigin origin) noexcept {
        using enum AudioActiveStatePolicy;
        using enum AudioFailureOrigin;

        for (const Rule &rule : Rules) {
            if (!Matches(error, *rule.descriptor))
                continue;
            if (origin == CallbackFault)
                return {rule.decision.area, State::Failed, Action::RequestHostPolicy, RetainUntilDetached, true};
            if (origin == RequestOrCandidate && rule.decision.activeState == QuiesceDevice)
                return {rule.decision.area, State::Rejected, rule.decision.action, Preserve, true};
            if (origin != RequestOrCandidate && origin != ActiveEpoch)
                return {};
            return rule.decision;
        }
        return {};
    }
}  // namespace Horo::Audio
