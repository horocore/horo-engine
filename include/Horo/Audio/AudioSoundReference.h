#pragma once

/**
 * @file AudioSoundReference.h
 * @brief Backend-neutral authored sound references, definitions, and playback defaults.
 */

#include "Horo/Audio/AudioIdentity.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <variant>

namespace Horo::Audio {
    /** @brief Persisted sound-definition schema version. */
    struct AudioSoundDefinitionSchemaVersion final {
        std::uint16_t major{1};
        std::uint16_t minor{};
        constexpr auto operator<=>(const AudioSoundDefinitionSchemaVersion &) const noexcept = default;
    };

    inline constexpr AudioSoundDefinitionSchemaVersion CurrentAudioSoundDefinitionSchemaVersion{1, 0};

    /** @brief The Audio-owned source family selected by an authored reference. */
    enum class AudioSoundReferenceKind : std::uint8_t {
        Unassigned,
        Clip,
        Variation,
        Stream,
        Music,
        Extension
    };

    /**
     * @brief Stable binding for a package-provided middleware, procedural, or future Audio contribution.
     *
     * The contribution identity is resolved by Audio's immutable registry snapshot. No vendor enum, name,
     * native handle, or package lifetime is retained by this persisted value.
     */
    struct AudioSoundExtensionReference final {
        AudioContributionId contribution;
        AudioSoundId definition;
        AudioSoundDefinitionSchemaVersion contractVersion{CurrentAudioSoundDefinitionSchemaVersion};
        constexpr auto operator<=>(const AudioSoundExtensionReference &) const noexcept = default;
    };

    /** @brief Exactly one typed target carried by an authored sound reference. */
    using AudioSoundReferenceTarget = std::variant<std::monostate, AudioClipId, AudioSoundId, AudioSoundExtensionReference>;

    /** @brief Normalized priority used by deterministic voice admission; larger values win ties. */
    using AudioPriority = std::uint16_t;

    /** @brief Inclusive upper bound for persisted voice-admission priority. */
    inline constexpr AudioPriority MaximumAudioPriority = 255;

    /** @brief Spatial behavior requested by one source or playback request. */
    enum class AudioSpatialMode : std::uint8_t {
        TwoD,
        ThreeD,
        TwoDimensional = TwoD,
        ThreeDimensional = ThreeD,
    };

    /** @brief Admission behavior when a concurrency group reaches its limit. */
    enum class AudioConcurrencyMode : std::uint8_t {
        Allow,
        Reject,
        StealOldest,
        StealQuietest,
        Virtualize,
        RejectNew = Reject,
        StealLowestPriority = StealQuietest,
    };

    /**
     * @brief Persistent concurrency limits carried into voice admission.
     *
     * A missing group is a source-local policy. A zero maximum means that the
     * source does not impose a concurrency ceiling; a non-zero maximum is
     * bounded by the representation and is applied by the Audio control owner.
     */
    struct AudioConcurrencyPolicy final {
        std::optional<AudioConcurrencyGroupId> group;           /**< Optional stable group shared by competing sources. */
        std::uint16_t maxInstances{};                           /**< Zero means this policy imposes no ceiling. */
        AudioConcurrencyMode mode{AudioConcurrencyMode::Allow}; /**< Admission action at the group ceiling. */

        [[nodiscard]] constexpr auto operator<=>(const AudioConcurrencyPolicy &) const noexcept = default;
    };

    /** @brief Policy describing how an admitted request is tied to scene teardown. */
    enum class AudioSceneLifecyclePolicy : std::uint8_t {
        StopOnUnload,
        KeepAliveInHostContext,
        StopOnSceneUnload = StopOnUnload,
        PreserveOnUnload = KeepAliveInHostContext,
    };

    /**
     * @brief Persistent backend-neutral reference to a playable sound source.
     *
     * Clip references point directly to an AudioClip asset. Variation, stream, and music references point to
     * Audio-owned sound-definition assets. Extension references add only a stable Audio contribution identity;
     * the package-specific payload remains behind the Audio contribution boundary.
     */
    struct AudioSoundReference final {
        AudioSoundReferenceKind kind{AudioSoundReferenceKind::Unassigned};
        AudioSoundReferenceTarget target{};

        /** @brief Creates a direct clip reference. @param clip Stable clip asset identity. @return Reference or typed identity error. */
        [[nodiscard]] static Result<AudioSoundReference> ForClip(AudioClipId clip);
        /** @brief Creates a variation-definition reference. @param definition Stable sound-definition identity. @return Reference or typed
         * identity error. */
        [[nodiscard]] static Result<AudioSoundReference> ForVariation(AudioSoundId definition);
        /** @brief Creates a stream-definition reference. @param definition Stable sound-definition identity. @return Reference or typed
         * identity error. */
        [[nodiscard]] static Result<AudioSoundReference> ForStream(AudioSoundId definition);
        /** @brief Creates a music-definition reference. @param definition Stable sound-definition identity. @return Reference or typed
         * identity error. */
        [[nodiscard]] static Result<AudioSoundReference> ForMusic(AudioSoundId definition);
        /**
         * @brief Creates a package contribution reference.
         * @param contribution Stable Audio contribution identity.
         * @param definition Stable contribution-owned sound-definition identity.
         * @param contractVersion Version of the contribution's Audio sound contract.
         * @return Reference or typed identity error.
         */
        [[nodiscard]] static Result<AudioSoundReference> ForExtension(
            AudioContributionId contribution, AudioSoundId definition,
            AudioSoundDefinitionSchemaVersion contractVersion = CurrentAudioSoundDefinitionSchemaVersion);

        /** @brief Reports whether this value selects an assigned target. @return True when not unassigned. */
        [[nodiscard]] constexpr bool IsAssigned() const noexcept {
            return kind != AudioSoundReferenceKind::Unassigned;
        }

        constexpr auto operator<=>(const AudioSoundReference &) const noexcept = default;
    };

    /** @brief Backend-neutral defaults applied when a sound definition creates a voice. */
    struct AudioSoundPlaybackDefaults final {
        float gain{1.0F};                                       /**< Finite non-negative linear gain. */
        float pitch{1.0F};                                      /**< Finite positive playback multiplier, bounded to 8x. */
        std::optional<AudioBusId> bus;                          /**< Optional stable mixer destination. */
        bool loop{};                                            /**< Whether playback repeats at the source boundary. */
        AudioSpatialMode spatialMode{AudioSpatialMode::ThreeD}; /**< 2D direct-bus or 3D provider-resolved routing. */
        bool enableDoppler{};                                   /**< 3D pitch hint; 2D requests do not resolve a spatial provider. */
        bool playOnStart{true};                                 /**< Whether a scene source emits on activation. */
        AudioPriority priority{128};                            /**< Deterministic admission priority. */
        AudioConcurrencyPolicy concurrency;                     /**< Shared-group admission and virtualization policy. */
        constexpr auto operator<=>(const AudioSoundPlaybackDefaults &) const noexcept = default;
    };

    /** @brief Backend-neutral playback values shared by persistent sources and transient requests. */
    using AudioPlaybackSettings = AudioSoundPlaybackDefaults;

    /** @brief Compatibility name for the shared source/request playback values. */
    using AudioPlaybackParameters = AudioSoundPlaybackDefaults;

    /**
     * @brief Versioned authored sound definition with no runtime voice, decoder, provider, or native ownership.
     */
    struct AudioSoundDefinition final {
        AudioSoundDefinitionSchemaVersion version{CurrentAudioSoundDefinitionSchemaVersion};
        AudioSoundReference source;
        AudioSoundPlaybackDefaults playback;
        bool operator==(const AudioSoundDefinition &) const = default;
    };

    /** @brief Compatibility name for callers that refer to the persisted schema explicitly. */
    using AudioSoundDefinitionSchema = AudioSoundDefinition;

    /**
     * @brief Validates one backend-neutral authored sound reference without registry lookup or mutation.
     * @param reference Candidate persisted reference.
     * @return Success or SoundReferenceInvalid.
     */
    [[nodiscard]] Result<void> ValidateAudioSoundReference(const AudioSoundReference &reference);

    /**
     * @brief Validates finite playback defaults without selecting a backend or resolving a bus.
     * @param playback Candidate defaults.
     * @return Success or SoundDefinitionInvalid.
     */
    [[nodiscard]] Result<void> ValidateAudioSoundPlaybackDefaults(const AudioSoundPlaybackDefaults &playback);

    /**
     * @brief Validates a concurrency policy without consulting a live voice registry.
     * @param policy Candidate authored policy.
     * @return Success or SoundDefinitionInvalid.
     */
    [[nodiscard]] Result<void> ValidateAudioConcurrencyPolicy(const AudioConcurrencyPolicy &policy);

    /**
     * @brief Validates a scene teardown policy without opening a scene context.
     * @param policy Candidate lifecycle policy.
     * @return Success or SoundDefinitionInvalid.
     */
    [[nodiscard]] Result<void> ValidateAudioSceneLifecyclePolicy(AudioSceneLifecyclePolicy policy);

    /**
     * @brief Validates a current-version authored sound definition.
     * @param definition Candidate persisted definition.
     * @return Success or a stable version, reference, or default-value error.
     */
    [[nodiscard]] Result<void> ValidateAudioSoundDefinition(const AudioSoundDefinition &definition);
}  // namespace Horo::Audio
