#pragma once

/** @file AudioVoiceRenderState.h @brief Target-private retained publication, callback state and fixed render storage. */
#include "Horo/Audio/AudioVoiceRenderRuntime.h"

#include <array>
#include <atomic>

namespace Horo::Audio {
    /** @brief Complete retained slot; metadata is immutable between publication and completed-block acknowledgement.
     * A pitch bank is processing storage: callback exchanges it with resident playback, then control retires it.
     */
    struct VoiceRenderSlot final {
        AudioMemoryHandle handle;
        MixerPlanIdentity graph;
        AudioStereoSpatialTarget target;
        AudioSpatialIdentity source;
        AudioSpatialIdentity listener;
        std::uint64_t sourceRevision{};
        std::uint64_t listenerRevision{};
        std::uint64_t sequence{};
        std::uint32_t busIndex{};
        std::uint32_t smoothingFrames{};
        std::optional<AudioResampler> pitchBank;
    };

    /** @brief One control owner and one callback lane; only the named mailboxes cross lanes.
     * Control owns pending/retained slots. Callback owns playback, raw scratch, active pointer and gain.
     * SC completion follows the mixer's last sample read, never merely command consumption.
     */
    struct AudioVoiceRenderRuntime::State final {
        AudioVoiceRenderDescriptor descriptor;
        std::shared_ptr<AudioVoiceStateMachine> registry;
        std::unique_ptr<AudioVoicePlayback> resident;
        std::shared_ptr<AudioStreamingService> service;
        std::optional<AudioStreamRenderPort> stream;
        std::optional<CoreStereoSpatialRenderer> spatial;
        AudioVoiceHandle voice;
        AudioResamplerDescriptor conversion;
        std::optional<AudioResamplerPlan> residentPlan;
        std::uint64_t coefficientBytes{};

        /** @brief Control-owned slots and SC mailboxes shared with the callback lane. */
        struct PublicationState final {
            std::array<VoiceRenderSlot, 2> slots;
            std::array<std::uint64_t, 2> generations{};
            std::int32_t retainedSlot{-1};
            std::int32_t pendingSlot{-1};
            std::atomic<VoiceRenderSlot *> candidate{};
            std::atomic<std::uint64_t> completed{};
            std::atomic<bool> applied{};
            std::atomic<std::uint64_t> resolvedPublication{};
            std::atomic<bool> closed{};
        };

        PublicationState publications;

        /** @brief Mutable processing state accessed only by the callback, or detached control. */
        struct CallbackState final {
            VoiceRenderSlot *active{};
            std::uint64_t callbackSequence{};
            float gain{1.0F};
            std::array<float, 4> residentMatrix{};
            std::uint32_t residentRamp{};
            AudioVoiceState streamState{AudioVoiceState::Ready};
            std::uint32_t buffered{};
            bool sourceEnded{};
        };

        CallbackState callback;

        bool released{};
        bool streamVoice{};
        std::uint64_t sourceFrames{};
        bool seekable{};
        AudioChannelLayout layout{MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)};

        /** @brief Exclusive callback logical time and worker realization handshake. */
        struct StreamPlayback final {
            AudioVoiceCursor cursor;
            AudioVoiceLoop loop;
            bool virtualMode{};
            bool realizing{};
            bool terminalReported{};
            std::uint64_t catchupFrames{};
        };

        StreamPlayback streamPlayback;

        /** @brief Fixed aligned callback scratch; output pointers borrow this immovable owner. */
        struct RenderScratch final {
            struct alignas(64) Plane final {
                std::array<float, 4096> samples{};
            };

            std::array<Plane, 2> raw;
            std::array<Plane, 2> output;
            std::array<Plane, 2> converted;  // Per-chunk DSP destination must stay aligned.
            std::array<AudioSample *, 2> outputPointers{output[0].samples.data(), output[1].samples.data()};
        };

        RenderScratch scratch;

        /** @brief Validate fixed composition and charge both worst-case prepared pitch banks without overflow. */
        static bool Valid(const AudioVoiceRenderDescriptor &descriptor, const AudioResamplerDescriptor &conversion,
                          std::uint64_t coefficientBytes) noexcept;
        /** @brief Validate the captured route, graph format and spatial identities before preparing storage. */
        bool ValidRoute(const AudioVoiceRenderRequest &request, const MixerRenderPlan &graph,
                        std::optional<std::uint32_t> bus) const noexcept;
        /** @brief Own one immutable generation and its optional replacement pitch bank before queue admission. */
        Result<VoiceRenderSlot> PrepareSlot(const AudioVoiceRenderRequest &request, const MixerRenderPlan &graph,
                                            const AudioStereoSpatialTarget &target, std::uint32_t bus, std::int32_t index) const;
        /** @brief Resolve an exact retained publication on callback, acknowledging success or rejection without reclamation. */
        const ErrorCodeDescriptor *ApplyPublication(const AudioPublishVoiceStateCommand &publication, std::uint64_t sequence) noexcept;
        /** @brief Apply resident or stream lifecycle without reclamation or worker cancellation on callback. */
        const ErrorCodeDescriptor *Control(const AudioVoiceControlRequest &request) noexcept;
        /** @brief Execute ordinary stream lifecycle controls after exact-voice/state admission. */
        const ErrorCodeDescriptor *StreamLifecycle(AudioVoiceControl control) noexcept;
        /** @brief Start or stop through the canonical intermediate Scheduled/Stopping states. */
        const ErrorCodeDescriptor *StartStopStream(AudioVoiceControl control) noexcept;
        /** @brief Freeze or restore physical/virtual execution without changing cursor phase. */
        const ErrorCodeDescriptor *PauseResumeStream(AudioVoiceControl control) noexcept;
        /** @brief Commit a canonical stream state and suspend fills for terminal stop. */
        const ErrorCodeDescriptor *CommitStreamState(AudioVoiceState next) noexcept;
        /** @brief Apply logical virtual/loop/seek policy without decoding or allocating. */
        const ErrorCodeDescriptor *StreamControl(const AudioVoiceControlRequest &request) noexcept;
        /** @brief Commit an admitted Ready/Playing voice into silent virtual execution. */
        const ErrorCodeDescriptor *EnterVirtualStream(AudioVoiceState expected) noexcept;
        /** @brief Publish one worker preparation request while retaining virtual time. */
        const ErrorCodeDescriptor *RealizeStream() noexcept;
        /** @brief Validate and publish one seek/loop discontinuity without weakening current ownership. */
        const ErrorCodeDescriptor *PositionStream(const AudioVoiceControlRequest &request) noexcept;
        /** @brief Validate a requested stream seek or exclusive loop against admitted source facts. */
        bool ValidStreamPosition(const AudioVoiceControlRequest &request) const noexcept;
        /** @brief Validate admitted mono/stereo stream facts against the requested conversion before allocation. */
        static bool ValidStreamFormat(const AudioResamplerDescriptor &conversion, const AudioStreamDecoderSpec &facts) noexcept;
        /** @brief Prepare conversion, canonical voice and retained port; the state owns rollback after each acquired resource. */
        Result<void> PrepareStream(AudioStreamHandle streamHandle);
        /** @brief Admit seek/loop only in controllable states with no outstanding realization handshake. */
        bool CanPositionStream() const noexcept;
        /** @brief Publish canonical virtual EOF once and suspend future fills without reclamation. */
        AudioVoiceMixRenderResult FinishVirtualStream() noexcept;
        /** @brief Discard bounded preparation lag and resume physical execution only when current PCM is available. */
        AudioVoiceMixRenderResult TryRealizeStream(std::uint32_t frames, bool &rendered) noexcept;
        /** @brief Advance source time, preserving exclusive loop phase and bounded fractional state. */
        void AdvanceStream(double sourceAdvance) noexcept;
        /** @brief Silence-only virtual work and bounded catch-up of a worker-positioned ring. */
        AudioVoiceMixRenderResult RenderVirtualStream(std::uint32_t frames) noexcept;
        /** @brief Render owned resident PCM once, then apply the prepared spatial matrix without double rate conversion. */
        AudioVoiceMixRenderResult RenderResident(std::uint32_t frames) noexcept;
        /** @brief Feed retained stream PCM to its sole spatial converter, retaining every unconsumed input frame. */
        AudioVoiceMixRenderResult RenderStream(std::uint32_t frames) noexcept;
        /** @brief Feed the physical stream converter only after canonical execution-state admission. */
        AudioVoiceMixRenderResult RenderStreamPcm(std::uint32_t frames) noexcept;
        /** @brief Fill empty callback scratch once; return true when the sole port reports cancellation. */
        bool ReadStreamBlock(std::span<AudioSample *const> rawPointers) noexcept;
        /** @brief Convert one admitted scratch block, copy output and retain unconsumed input without allocation. */
        AudioResamplerProgress ConvertStreamBlock(std::uint32_t frames, std::uint32_t produced) noexcept;
        /** @brief Release a stream-only canonical slot on detached control, never from callback. */
        ~State();
    };

    static_assert(std::atomic<VoiceRenderSlot *>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

}  // namespace Horo::Audio
