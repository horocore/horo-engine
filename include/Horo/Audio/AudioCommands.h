#pragma once

/** @file AudioCommands.h
 * @brief Owned, bounded audio command values and preparation-time validation contracts.
 */

#include "Horo/Audio/AudioClock.h"
#include "Horo/Audio/AudioMemory.h"
#include "Horo/Audio/AudioParameterAutomation.h"

#include <variant>

namespace Horo::Audio {
    inline constexpr std::uint32_t MaximumScheduledAudioCommands = 16;

    /** @brief Exact callback generation and scene context resolved by the control owner, never by callback ECS lookup. */
    struct AudioCommandScope {
        AudioRuntimeId owner;
        std::uint64_t epoch{};
        AudioSceneContextHandle scene;
        auto operator<=>(const AudioCommandScope &) const noexcept = default;
    };

    /** @brief Bind an already admitted voice slot to a prepared clip; no callback asset lookup or allocation. */
    struct AudioCreateVoiceCommand {
        AudioVoiceHandle voice;
        AudioClipHandle clip;
    };

    /** @brief Start an admitted voice at the next buffer boundary. */
    struct AudioStartVoiceCommand {
        AudioVoiceHandle voice;
    };

    /** @brief Stop an admitted voice; transport must preserve critical capacity and explicit retry ownership. */
    struct AudioStopVoiceCommand {
        AudioVoiceHandle voice;
    };

    /** @brief Last-value-wins parameter snapshot; not an individually completed playback operation. */
    struct AudioSetParameterCommand {
        AudioVoiceHandle voice;
        AudioParameterId parameter;
        float value{}; /**< Finite model-owned units; normalization canonicalizes zero/subnormal, not range or gain. */
    };

    /** @brief Sample-clock automation intent; never coalesced across overlap/cancellation boundaries. */
    struct AudioAutomateParameterCommand {
        AudioParameterAutomationRequest request;
    };

    /** @brief Cancel one exact callback-generation automation request through ordinary FIFO consumption. */
    struct AudioCancelAutomationCommand {
        std::uint64_t requestId{};
        std::uint64_t clockGeneration{};
        std::uint64_t discontinuityRevision{};
    };

    /** @brief Publish prevalidated graph storage retained through callback acknowledgement by its control owner. */
    struct AudioSwapGraphCommand {
        AudioMemoryHandle storage;
    };

    /** @brief Request logical release of prepared storage; physical reclamation stays with the quiescent control owner. */
    struct AudioReleaseResourceCommand {
        AudioMemoryHandle storage;
    };

    /** @brief Ordered scene-context barrier; the scope identifies the closing context generation. */
    struct AudioSceneUnloadCommand {};

    /** @brief Ordered runtime reset barrier; only the control owner stages it and establishes the next epoch. */
    struct AudioResetCommand {};

    /** @brief Callback application boundary for a prepared scheduled batch. */
    enum class AudioCommandTargetKind : std::uint8_t {
        NextBufferBoundary,
        ExactSampleFrame
    };

    /** @brief Resolved callback target; exact samples name the current clock and discontinuity generations. */
    struct AudioCommandTarget final {
        AudioCommandTargetKind kind{AudioCommandTargetKind::NextBufferBoundary};
        std::uint64_t sampleFrame{};
        std::uint64_t clockGeneration{};
        std::uint64_t discontinuityRevision{};
    };

    /**
     * @brief One callback-visible reference to a complete retained batch prepared by control.
     * The referenced fixed batch is one FIFO record and therefore cannot be partially published.
     */
    struct AudioScheduledBatchCommand final {
        AudioMemoryHandle storage;
        AudioCommandTarget target;
        std::uint32_t commandCount{};
        bool containsCriticalCommand{};
    };

    /** @brief Allocation-free tagged payload; clock-mapped scheduled batches are a separate timing contract. */
    using AudioCommandPayload =
        std::variant<AudioCreateVoiceCommand, AudioStartVoiceCommand, AudioStopVoiceCommand, AudioSetParameterCommand,
                     AudioAutomateParameterCommand, AudioCancelAutomationCommand, AudioSwapGraphCommand, AudioReleaseResourceCommand,
                     AudioSceneUnloadCommand, AudioResetCommand, AudioScheduledBatchCommand>;

    /** @brief Owned next-buffer-boundary intent; copying retains IDs, not resource lifetime or producer references. */
    struct AudioCommand {
        AudioCommandScope scope;
        AudioCommandPayload payload;
    };

    /** @brief Fixed-size preparation/admission classification, never an allocating general Error. */
    enum class AudioCommandStatus : std::uint8_t {
        Ok,
        InvalidScope,
        InvalidPayload
    };
    /** @brief Reservation class; critical classification does not authorize priority reordering. */
    enum class AudioCommandClass : std::uint8_t {
        Ordinary,
        Critical
    };

    /**
     * @brief Validate structural identity and canonicalize parameter values without allocation or ambient lookup.
     * @param command Caller-owned intent; owner/epoch must be nonzero, and scene is required except for reset.
     * Reset uses an entirely empty scene handle and is still bound to one exact runtime epoch.
     * @param normalized Output assigned only on success; aliasing command is allowed.
     * @return Fixed-size scope/payload failure or Ok. Rejection leaves the output unchanged.
     * @pre Control must separately validate actual live handles, scene admission, resource leases and epoch authority.
     * Structural validation is not proof of liveness. Resource owners must outlive consumption/acknowledgement.
     */
    [[nodiscard]] AudioCommandStatus NormalizeAudioCommand(const AudioCommand &command, AudioCommand &normalized) noexcept;
    /** @brief Classify stop/release/unload/reset as critical; other intents consume ordinary capacity.
     * @param command Structurally validated intent. @return Reservation class, with FIFO ordering unchanged.
     */
    [[nodiscard]] AudioCommandClass ClassifyAudioCommand(const AudioCommand &command) noexcept;
    /** @brief Permit replacing only two adjacent parameter snapshots with identical scope, voice and parameter.
     * @param earlier Validated earlier intent not yet published to the callback.
     * @param later Validated next intent; all other intent pairs, including barriers, prohibit coalescing.
     * @return True if later can replace earlier without changing any intervening operation or lifecycle order.
     */
    [[nodiscard]] bool CanCoalesceAudioCommands(const AudioCommand &earlier, const AudioCommand &later) noexcept;
}  // namespace Horo::Audio
