#pragma once

/** @file AudioVoiceControls.h
 * @brief Owned typed voice control intent for the bounded audio command path.
 */

#include "Horo/Audio/AudioIdentity.h"

namespace Horo::Audio {
    /** @brief Source-frame loop interval; disabled loops require zero endpoints. */
    struct AudioVoiceLoop final {
        bool enabled{};
        std::uint32_t begin{}; /**< Inclusive source frame. */
        std::uint32_t end{};   /**< Exclusive source frame. */
    };

    /** @brief Exact rendered source position, independent of resampler look-ahead consumption. */
    struct AudioVoiceCursor final {
        std::uint64_t frame{}; /**< Resident or streamed source position, never decoder look-ahead. */
        double fraction{};     /**< In [0,1); frozen during pause and discontinuity fade-out. */
    };

    /** @brief Operations applied only at an admitted sample/buffer boundary on the exclusive processing owner. */
    enum class AudioVoiceControl : std::uint8_t {
        Start,
        Stop,
        Pause,
        Resume,
        Seek,
        SetLoop,
        SetPlaybackSpeed,
        Cancel,
        Restart,
        StartVirtual, /**< Start an admitted logical voice without a physical reservation. */
        Virtualize,   /**< Release physical execution at the dispatched boundary; control owns retirement/accounting. */
        Realize       /**< Resume physical execution only after control has reserved capacity and prepared resources. */
    };

    /** @brief Fixed-size typed operation; unused fields must remain at their defaults. */
    struct AudioVoiceControlRequest final {
        AudioVoiceHandle voice;
        AudioVoiceControl control{AudioVoiceControl::Start};
        std::uint32_t seekFrame{};
        AudioVoiceLoop loop;
        double playbackSpeed{1.0};         /**< Independent pitch-preserving speed: only unity is supported. */
        std::uint64_t operationSequence{}; /**< Optional nonzero repeated-play identity; zero for direct controls. */
    };

    /**
     * @brief Validate bounded structural intent without consulting a live voice or allocating.
     * @param request Owned operation; seek and loop endpoints are validated against PCM by the playback owner.
     * @return True for known controls, valid handle shape and canonical unused fields.
     * Non-unity/non-finite speed remains structurally valid so playback can return its explicit unsupported error.
     */
    [[nodiscard]] bool ValidateAudioVoiceControlRequest(const AudioVoiceControlRequest &request) noexcept;
}  // namespace Horo::Audio
