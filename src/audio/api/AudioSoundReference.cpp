#include "Horo/Audio/AudioSoundReference.h"

#include "Horo/Audio/AudioErrors.h"

#include <cmath>

namespace Horo::Audio {
    namespace {
        [[nodiscard]] Result<AudioSoundReference> InvalidReference() {
            return Result<AudioSoundReference>::Failure(MakeError(AudioErrors::SoundReferenceInvalid));
        }

        [[nodiscard]] Result<void> InvalidDefinition() {
            return Result<void>::Failure(MakeError(AudioErrors::SoundDefinitionInvalid));
        }

        [[nodiscard]] Result<void> InvalidReferenceValue() {
            return Result<void>::Failure(MakeError(AudioErrors::SoundReferenceInvalid));
        }

        [[nodiscard]] bool IsKnownKind(const AudioSoundReferenceKind kind) noexcept {
            using enum AudioSoundReferenceKind;
            switch (kind) {
                case Unassigned:
                case Clip:
                case Variation:
                case Stream:
                case Music:
                case Extension:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsCurrentVersion(const AudioSoundDefinitionSchemaVersion version) noexcept {
            return version == CurrentAudioSoundDefinitionSchemaVersion;
        }

        [[nodiscard]] bool IsKnownSpatialMode(const AudioSpatialMode mode) noexcept {
            switch (mode) {
                case AudioSpatialMode::TwoD:
                case AudioSpatialMode::ThreeD:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnownConcurrencyMode(const AudioConcurrencyMode mode) noexcept {
            using enum AudioConcurrencyMode;
            switch (mode) {
                case Allow:
                case Reject:
                case StealOldest:
                case StealQuietest:
                case Virtualize:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsKnownSceneLifecyclePolicy(const AudioSceneLifecyclePolicy policy) noexcept {
            switch (policy) {
                case AudioSceneLifecyclePolicy::StopOnUnload:
                case AudioSceneLifecyclePolicy::KeepAliveInHostContext:
                    return true;
            }
            return false;
        }

        /** @brief Checks the finite gain and bounded pitch portion of playback defaults. */
        [[nodiscard]] bool HasValidGainAndPitch(const AudioSoundPlaybackDefaults &playback) noexcept {
            if (!std::isfinite(playback.gain) || playback.gain < 0.0F)
                return false;
            if (!std::isfinite(playback.pitch) || playback.pitch <= 0.0F || playback.pitch > 8.0F)
                return false;
            return true;
        }

        /** @brief Checks routing, spatial, and deterministic admission values without consulting runtime state. */
        [[nodiscard]] bool HasValidPlaybackAdmissionValues(const AudioSoundPlaybackDefaults &playback) noexcept {
            if (playback.bus.has_value() && !playback.bus->IsValid())
                return false;
            if (!IsKnownSpatialMode(playback.spatialMode))
                return false;
            return playback.priority <= MaximumAudioPriority;
        }

        [[nodiscard]] Result<AudioSoundReference> ForDefinition(const AudioSoundReferenceKind kind, const AudioSoundId definition) {
            if (!definition.IsValid())
                return InvalidReference();
            return Result<AudioSoundReference>::Success({kind, definition});
        }
    }  // namespace

    /** @copydoc AudioSoundReference::ForClip */
    Result<AudioSoundReference> AudioSoundReference::ForClip(const AudioClipId clip) {
        if (!clip.IsValid())
            return InvalidReference();
        return Result<AudioSoundReference>::Success({AudioSoundReferenceKind::Clip, clip});
    }

    /** @copydoc AudioSoundReference::ForVariation */
    Result<AudioSoundReference> AudioSoundReference::ForVariation(const AudioSoundId definition) {
        return ForDefinition(AudioSoundReferenceKind::Variation, definition);
    }

    /** @copydoc AudioSoundReference::ForStream */
    Result<AudioSoundReference> AudioSoundReference::ForStream(const AudioSoundId definition) {
        return ForDefinition(AudioSoundReferenceKind::Stream, definition);
    }

    /** @copydoc AudioSoundReference::ForMusic */
    Result<AudioSoundReference> AudioSoundReference::ForMusic(const AudioSoundId definition) {
        return ForDefinition(AudioSoundReferenceKind::Music, definition);
    }

    /** @copydoc AudioSoundReference::ForExtension */
    Result<AudioSoundReference> AudioSoundReference::ForExtension(const AudioContributionId contribution, const AudioSoundId definition,
                                                                  const AudioSoundDefinitionSchemaVersion contractVersion) {
        if (!contribution.IsValid() || !definition.IsValid() || contractVersion.major == 0)
            return InvalidReference();
        return Result<AudioSoundReference>::Success(
            {AudioSoundReferenceKind::Extension, AudioSoundExtensionReference{contribution, definition, contractVersion}});
    }

    /** @copydoc ValidateAudioSoundReference */
    Result<void> ValidateAudioSoundReference(const AudioSoundReference &reference) {
        using enum AudioSoundReferenceKind;
        if (!IsKnownKind(reference.kind))
            return InvalidReferenceValue();

        switch (reference.kind) {
            case Unassigned:
                return std::holds_alternative<std::monostate>(reference.target) ? Result<void>::Success() : InvalidReferenceValue();
            case Clip:
                return std::holds_alternative<AudioClipId>(reference.target) && std::get<AudioClipId>(reference.target).IsValid()
                           ? Result<void>::Success()
                           : InvalidReferenceValue();
            case Variation:
            case Stream:
            case Music:
                return std::holds_alternative<AudioSoundId>(reference.target) && std::get<AudioSoundId>(reference.target).IsValid()
                           ? Result<void>::Success()
                           : InvalidReferenceValue();
            case Extension: {
                if (!std::holds_alternative<AudioSoundExtensionReference>(reference.target))
                    return InvalidReferenceValue();
                const AudioSoundExtensionReference &extension = std::get<AudioSoundExtensionReference>(reference.target);
                return extension.contribution.IsValid() && extension.definition.IsValid() && extension.contractVersion.major != 0
                           ? Result<void>::Success()
                           : InvalidReferenceValue();
            }
        }
        return InvalidReferenceValue();
    }

    /** @copydoc ValidateAudioSoundPlaybackDefaults */
    Result<void> ValidateAudioSoundPlaybackDefaults(const AudioSoundPlaybackDefaults &playback) {
        if (!HasValidGainAndPitch(playback) || !HasValidPlaybackAdmissionValues(playback))
            return InvalidDefinition();
        return ValidateAudioConcurrencyPolicy(playback.concurrency);
    }

    /** @copydoc ValidateAudioConcurrencyPolicy */
    Result<void> ValidateAudioConcurrencyPolicy(const AudioConcurrencyPolicy &policy) {
        if ((policy.group.has_value() && !policy.group->IsValid()) || !IsKnownConcurrencyMode(policy.mode))
            return InvalidDefinition();
        return Result<void>::Success();
    }

    /** @copydoc ValidateAudioSceneLifecyclePolicy */
    Result<void> ValidateAudioSceneLifecyclePolicy(const AudioSceneLifecyclePolicy policy) {
        return IsKnownSceneLifecyclePolicy(policy) ? Result<void>::Success() : InvalidDefinition();
    }

    /** @copydoc ValidateAudioSoundDefinition */
    Result<void> ValidateAudioSoundDefinition(const AudioSoundDefinition &definition) {
        if (!IsCurrentVersion(definition.version))
            return Result<void>::Failure(MakeError(AudioErrors::SoundDefinitionVersionUnsupported));
        if (!definition.source.IsAssigned())
            return InvalidDefinition();
        if (const Result<void> reference = ValidateAudioSoundReference(definition.source); reference.HasError())
            return reference;
        return ValidateAudioSoundPlaybackDefaults(definition.playback);
    }
}  // namespace Horo::Audio
