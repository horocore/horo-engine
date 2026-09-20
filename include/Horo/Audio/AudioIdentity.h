#pragma once

/**
 * @file AudioIdentity.h
 * @brief Stable audio identities and generation-safe process-local handles.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstdint>

namespace Horo::Audio {
    /** @brief Maximum number of live or reusable slots admitted by one audio handle registry. */
    inline constexpr std::uint32_t MaximumAudioHandleSlots = 1'048'576;

    /** @brief Strong persistent identity backed by the canonical Assets identity. */
    template <typename Tag> class AudioAssetIdentity final {
    public:
        AudioAssetIdentity() = default;

        /**
         * @brief Creates an audio identity from a persistent asset identity.
         * @param asset Canonical asset identity owned by the Assets subsystem.
         * @return Typed audio identity or an identity validation error.
         */
        [[nodiscard]] static Result<AudioAssetIdentity> Create(const Assets::AssetId &asset) {
            if (!asset.IsValid())
                return Result<AudioAssetIdentity>::Failure(MakeError(AudioErrors::IdentityInvalid));
            return Result<AudioAssetIdentity>::Success(AudioAssetIdentity{asset});
        }

        /** @brief Returns the persistent asset identity. @return Borrowed canonical Assets identity. */
        [[nodiscard]] const Assets::AssetId &Asset() const noexcept {
            return asset_;
        }

        /** @brief Reports whether this value contains a non-zero persistent identity. @return True for a usable identity. */
        [[nodiscard]] bool IsValid() const noexcept {
            return asset_.IsValid();
        }

        [[nodiscard]] auto operator<=>(const AudioAssetIdentity &) const noexcept = default;

    private:
        explicit AudioAssetIdentity(const Assets::AssetId &asset) : asset_(asset) {}

        Assets::AssetId asset_;
    };

    /** @brief Strong stable numeric identity resolved before real-time processing. */
    template <typename Tag> class AudioStableIdentity final {
    public:
        AudioStableIdentity() = default;

        /**
         * @brief Creates a stable identity from its canonical encoded value.
         * @param value Non-zero persistent value assigned by the owning model or registry.
         * @return Typed identity or an identity validation error.
         */
        [[nodiscard]] static Result<AudioStableIdentity> Create(const std::uint64_t value) {
            if (value == 0)
                return Result<AudioStableIdentity>::Failure(MakeError(AudioErrors::IdentityInvalid));
            return Result<AudioStableIdentity>::Success(AudioStableIdentity{value});
        }

        /** @brief Returns the canonical encoded value. @return Non-zero value for a valid identity. */
        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return value_;
        }

        /** @brief Reports whether this value contains a non-zero identity. @return True for a usable identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != 0;
        }

        [[nodiscard]] auto operator<=>(const AudioStableIdentity &) const noexcept = default;

    private:
        explicit constexpr AudioStableIdentity(const std::uint64_t value) : value_(value) {}

        std::uint64_t value_{};
    };

    struct AudioClipIdentityTag;
    struct AudioSoundIdentityTag;
    struct AudioBusIdentityTag;
    struct AudioParameterIdentityTag;
    struct AudioEventIdentityTag;
    struct AudioContributionIdentityTag;
    struct AudioRuntimeIdentityTag;

    /** @brief Persistent identity of one authored or cooked audio clip asset. */
    using AudioClipId = AudioAssetIdentity<AudioClipIdentityTag>;
    /** @brief Persistent identity of one playable audio sound definition. */
    using AudioSoundId = AudioAssetIdentity<AudioSoundIdentityTag>;
    /** @brief Stable identity of one mixer bus; display names and positions are not identity. */
    using AudioBusId = AudioStableIdentity<AudioBusIdentityTag>;
    /** @brief Stable parameter identity resolved before callback submission. */
    using AudioParameterId = AudioStableIdentity<AudioParameterIdentityTag>;
    /** @brief Stable event identity resolved before callback submission. */
    using AudioEventId = AudioStableIdentity<AudioEventIdentityTag>;
    /** @brief Stable Audio contribution identity used by package-provided sound definitions. */
    using AudioContributionId = AudioStableIdentity<AudioContributionIdentityTag>;
    /** @brief Process-local owner identity of one audio runtime generation. */
    using AudioRuntimeId = AudioStableIdentity<AudioRuntimeIdentityTag>;

    /** @brief Alias used by middleware contracts for stable parameter identity. */
    using StableParameterId = AudioParameterId;
    /** @brief Alias used by middleware contracts for stable event identity. */
    using StableEventId = AudioEventId;

    /** @brief Registry-owned process-local handle that cannot be serialized. */
    template <typename Tag> struct AudioHandle final {
        AudioRuntimeId owner;       /**< Exact audio runtime generation that issued the handle. */
        std::uint32_t slot{};       /**< Non-zero slot in the owning typed registry. */
        std::uint32_t generation{}; /**< Non-zero slot generation; retired values never wrap. */

        /** @brief Reports whether owner, slot, and generation are all present. @return True for a well-formed handle. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && slot != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const AudioHandle &) const noexcept = default;
    };

    struct AudioClipHandleTag;
    struct AudioVoiceHandleTag;
    struct AudioBusHandleTag;
    struct AudioEventInstanceTag;
    struct AudioDeviceIdentityTag;
    struct AudioSceneContextHandleTag;

    /** @brief Generation-safe handle to one prepared runtime clip. */
    using AudioClipHandle = AudioHandle<AudioClipHandleTag>;
    /** @brief Generation-safe handle to one admitted playback voice. */
    using AudioVoiceHandle = AudioHandle<AudioVoiceHandleTag>;
    /** @brief Generation-safe handle to one active mixer bus generation. */
    using AudioBusHandle = AudioHandle<AudioBusHandleTag>;
    /** @brief Generation-safe identity of one active middleware event instance. */
    using AudioEventInstanceId = AudioHandle<AudioEventInstanceTag>;
    /** @brief Generation-scoped Horo device identity that exposes no native device value. */
    using AudioDeviceId = AudioHandle<AudioDeviceIdentityTag>;
    /** @brief Generation-safe client handle to one active scene audio context. */
    using AudioSceneContextHandle = AudioHandle<AudioSceneContextHandleTag>;
}  // namespace Horo::Audio
