#pragma once

/** @file AudioVoiceRenderRuntime.h
 * @brief Retained immutable voice routing/spatial generations and real playback-to-mixer execution.
 */

#include "Horo/Audio/AudioStreamingService.h"
#include "Horo/Audio/AudioVoicePlayback.h"
#include "Horo/Audio/CoreStereoSpatialRenderer.h"
#include "Horo/Audio/MixerGraphCompiler.h"

#include <memory>
#include <optional>

namespace Horo::Audio {
    /** @brief Immutable host composition; storage is prepared before callback entry. */
    struct AudioVoiceRenderDescriptor final {
        AudioCommandScope scope;
        AudioMemoryPoolId storageIdentity;
        AudioParameterId gainParameter;           /**< Linear trim address, multiplied with immutable authored/spatial gain. */
        std::uint32_t maximumFrames{256};         /**< In [1,4096], within the source and mixer limits. */
        std::size_t maximumStateBytes{1U << 20U}; /**< Owner, fixed scratch and both retained pitch banks; excludes source reservations. */
    };

    /** @brief Complete control-owned state request; copying retains values, never scene/component pointers. */
    struct AudioVoiceRenderRequest final {
        AudioBusId bus;
        AudioSpatialSource source;
        std::optional<AudioSpatialListener> listener;
        AudioStereoSpatialSettings spatial;
    };

    /** @brief One callback's borrowed mixer input; valid only until EndBlock and the next Render call. */
    struct AudioVoiceMixRenderResult final {
        MixerVoiceInput input;
        const ErrorCodeDescriptor *error{};
        bool terminal{};
    };

    /** @brief Control-side completed publication facts, not merely queue consumption. */
    struct AudioVoiceStateAcknowledgement final {
        std::uint64_t sequence{};
        bool applied{};
    };

    /**
     * @brief Two-slot voice-state owner integrating ordinary staging, prepared playback/spatial processing and mixer input.
     * @details Publish/Reconcile/Close/CompleteShutdown belong to one control owner; Apply/Render/EndBlock to one callback.
     * Immutable slot publication and completed-block reclamation use lock-free SC pointer/sequence atomics.
     * Only one publication may be pending. Rejection retains current state and accepts no hidden retry.
     * The registry is retained but still exclusively processing-owned: hosts must not access it concurrently.
     * Stream service methods stay on control; the sole port is retained until callback detachment. Retire/Shutdown
     * on that service remain forbidden while this owner holds its port. Jobs and package providers outlive the service.
     * Shared pins protect host-independent ownership, not permission for concurrent mutation.
     * No callback operation allocates, frees, queries scene/assets, waits, formats errors or selects a backend.
     */
    class AudioVoiceRenderRuntime final {
        struct State;

        /** @brief Factory-only construction capability; callers cannot bypass preparation. */
        class ConstructionKey final {
            friend class AudioVoiceRenderRuntime;
            ConstructionKey() = default;
        };

    public:
        /** @brief Adopt complete factory-prepared storage through std::make_unique.
         * @param key Private capability issued only by the validated factories.
         * @param state Complete preparation-time owner, never null.
         * @pre Only CreateResident/CreateStream may issue the construction capability.
         */
        explicit AudioVoiceRenderRuntime(ConstructionKey key, std::unique_ptr<State> state) noexcept;
        /** @brief Copy resident PCM and prepare fixed output before transferring processing ownership.
         * @param registry Retained canonical registry, exclusively accessible during preparation/detached control.
         * @param source Borrowed PCM copied by AudioVoicePlayback before return.
         * @param playback Source conversion/loop reservations; initial gain and pitch must be unity to avoid double application.
         * @param descriptor Exact epoch and bounded state reservations; output is stereo at the admitted mix rate.
         * @return Complete owner or typed rejection with no leaked voice.
         */
        [[nodiscard]] static Result<std::unique_ptr<AudioVoiceRenderRuntime>> CreateResident(
            std::shared_ptr<AudioVoiceStateMachine> registry, AudioResamplerInput source, const AudioVoicePlaybackConfig &playback,
            const AudioVoiceRenderDescriptor &descriptor);
        /** @brief Retain the sole stream port and prepare its raw-PCM-to-stereo conversion outside callback.
         * @param registry Retained canonical registry; factory admits a Ready voice.
         * @param service Retained control service; its source/jobs remain host-owned dependencies.
         * @param stream Exact admitted stream, whose ring cannot be retired until CompleteShutdown releases the port.
         * @param conversion Explicit Linear ClipToMix format matching the stream's decoder facts, with unity pitch/speed.
         * @param maximumCoefficientBytes Prepared converter reservation.
         * @param descriptor Exact epoch, parameter address and bounded owner storage.
         * @return Complete owner or typed rejection. Failure does not retire caller-owned stream storage.
         */
        [[nodiscard]] static Result<std::unique_ptr<AudioVoiceRenderRuntime>> CreateStream(std::shared_ptr<AudioVoiceStateMachine> registry,
                                                                                           std::shared_ptr<AudioStreamingService> service,
                                                                                           AudioStreamHandle stream,
                                                                                           const AudioResamplerDescriptor &conversion,
                                                                                           std::uint64_t maximumCoefficientBytes,
                                                                                           const AudioVoiceRenderDescriptor &descriptor);
        /** @brief Destroy on control only after CompleteShutdown has proved callback detachment. */
        ~AudioVoiceRenderRuntime();
        AudioVoiceRenderRuntime(const AudioVoiceRenderRuntime &) = delete;
        AudioVoiceRenderRuntime &operator=(const AudioVoiceRenderRuntime &) = delete;
        /** @brief Read immutable admitted identity. @return Exact voice. */
        [[nodiscard]] AudioVoiceHandle Voice() const noexcept;
        /** @brief Prepare a copied spatial/parameter/route generation and enqueue its exact storage reference.
         * @param request Complete owned values; source/listener contexts must match this owner.
         * @param graph Pinned candidate/current plan used only during preparation; resolves a non-Return stereo bus.
         * @param staging Same control-owner transport; its Pump cannot run concurrently with Publish.
         * @return Typed preparation failure or explicit admission/retry; failure preserves the current generation.
         * @details Full state/route changes never coalesce. Only ordinary adjacent gain-parameter commands may coalesce
         * in existing staging, with no start/stop/resource/unload/reset reordering.
         */
        [[nodiscard]] Result<AudioCommandAdmission> Publish(const AudioVoiceRenderRequest &request, const MixerRenderPlan &graph,
                                                            AudioCommandStaging &staging);
        /** @brief Apply one exact FIFO record at a callback boundary; no source/registry discovery.
         * @param record Retained publication, voice control, gain update or matching lifecycle barrier.
         * @return Null on success or fixed rejection; invalid records preserve current state.
         */
        [[nodiscard]] const ErrorCodeDescriptor *Apply(const AudioCommandRecord &record) noexcept;
        /** @brief Execute actual resident/stream playback and produce retained stereo input for the mixer.
         * @param frames Positive block length within prepared bounds.
         * @return Borrowed stereo output plus fixed error/terminal evidence. Closed admitted voices retain their
         * exact route with silence. Before initial state admission or after detachment input is empty; omit it from the mixer.
         * @pre This callback owns processing resources; consume input with MixerGraphRuntime before EndBlock.
         */
        [[nodiscard]] AudioVoiceMixRenderResult Render(std::uint32_t frames) noexcept;
        /** @brief Publish completion after the mixer's last use of every borrowed voice input in this block. */
        void EndBlock() noexcept;
        /** @brief Reclaim replaced pitch/state storage only after completed-block acknowledgement.
         * @return Latest publication sequence and whether callback admission succeeded.
         */
        [[nodiscard]] AudioVoiceStateAcknowledgement Reconcile() noexcept;
        /** @brief Close publication and request callback silence, retaining every source and pending generation. */
        void Close() noexcept;
        /** @brief Release ports/playback/state on control after exact-epoch native stop/join proof.
         * @param callbackDetached Host proof that no in-flight or future callback can use this owner.
         * @return False for unclosed or unproved teardown, preserving all ownership; repeated success is harmless.
         * @details Host reconciles accepted staging work/outcomes before releasing its transport. Queue consumption,
         * silence, Close or a timeout alone never authorize detachment. Stream retirement is a later control operation.
         */
        [[nodiscard]] bool CompleteShutdown(bool callbackDetached) noexcept;

    private:
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Audio
