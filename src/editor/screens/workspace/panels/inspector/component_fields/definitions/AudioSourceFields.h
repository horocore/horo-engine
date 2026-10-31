#pragma once
/** @file AudioSourceFields.h @brief Typed AudioSourceFields presentation fields and canonical default accessors. */
#include "../InspectorFieldSchema.h"
#include "Horo/Runtime/Scene/SceneComponents.h"

#include <numbers>

namespace Horo::Editor::InspectorFields {
    inline constexpr std::array spatialModes{Option(Audio::AudioSpatialMode::TwoD, "workspace.value.off"),
                                             Option(Audio::AudioSpatialMode::ThreeD, "workspace.value.on")};
    inline constexpr std::array audioLifecycles{Option(Audio::AudioSceneLifecyclePolicy::StopOnUnload,
                                                       "workspace.inspector.audio.stop_unload"),
                                                Option(Audio::AudioSceneLifecyclePolicy::KeepAliveInHostContext,
                                                       "workspace.inspector.audio.keep_alive")};
    inline constexpr std::array audioConcurrency{Option(Audio::AudioConcurrencyMode::Allow, "workspace.inspector.audio.allow"),
                                                 Option(Audio::AudioConcurrencyMode::Reject, "workspace.inspector.audio.reject"),
                                                 Option(Audio::AudioConcurrencyMode::StealOldest, "workspace.inspector.audio.steal_oldest"),
                                                 Option(Audio::AudioConcurrencyMode::StealQuietest,
                                                        "workspace.inspector.audio.steal_quietest"),
                                                 Option(Audio::AudioConcurrencyMode::Virtualize, "workspace.inspector.audio.virtualize"),
                                                 Option(Audio::AudioConcurrencyMode::StealLowestPriority,
                                                        "workspace.inspector.audio.steal_priority"),
                                                 Option(Audio::AudioConcurrencyMode::StealFurthest,
                                                        "workspace.inspector.audio.steal_furthest"),
                                                 Option(Audio::AudioConcurrencyMode::Replace, "workspace.inspector.audio.replace")};

    /** @brief Static field registry for Runtime::AudioSourceComponent; no state, registration or activation side effects. */
    template <> struct Schema<Runtime::AudioSourceComponent> {
        using Model = Runtime::AudioSourceComponent;
        inline static constexpr auto fields = Checked(std::array{
            Bind<Model,
                 +[](Model &v) -> auto & {
            return v.playback.gain;
        }, Control::Number>("audio_source_gain", "workspace.inspector.audio_source_gain")
                .Number({.speed = 0.01F})
                .Valid(+[](const Model &v) {
            return v.playback.gain >= 0;
        }),
            Bind<Model,
                 +[](Model &v) -> auto & {
            return v.playback.pitch;
        }, Control::Number>("audio_source_pitch", "workspace.inspector.audio_source_pitch")
                .Number({.speed = 0.01F})
                .Valid(+[](const Model &v) {
            return v.playback.pitch > 0 && v.playback.pitch <= 8;
        }),
            Bind<Model,
                 +[](Model &v) -> auto & {
            return v.playback.spatialMode;
        }, Control::Choice>("audio_source_spatial", "workspace.inspector.audio_source_spatial")
                .Options(spatialModes),
            Bind<Model,
                 +[](Model &v) -> auto & {
            return v.playback.loop;
        }, Control::Boolean>("workspace.inspector.audio.loop", "workspace.inspector.audio.loop"),
            Bind<Model,
                 +[](Model &v) -> auto & {
            return v.playback.playOnStart;
        }, Control::Boolean>("workspace.inspector.audio.play_on_start", "workspace.inspector.audio.play_on_start"),
            Bind<Model,
                 +[](Model &v) -> auto & {
            return v.playback.enableDoppler;
        }, Control::Boolean>("workspace.inspector.audio.doppler", "workspace.inspector.audio.doppler"),
            Bind<Model,
                 +[](Model &v) -> auto & {
            return v.playback.priority;
        }, Control::Unsigned>("audio_priority", "workspace.inspector.audio.priority")
                .Valid(+[](const Model &v) {
            return v.playback.priority <= Audio::MaximumAudioPriority;
        }),
            Bind<Model,
                 +[](Model &v) -> auto & {
            return v.playback.concurrency.maxInstances;
        }, Control::Unsigned>("audio_max_instances", "workspace.inspector.audio.max_instances"),
            Bind<Model,
                 +[](Model &v) -> auto & {
            return v.playback.concurrency.mode;
        }, Control::Choice>("audio_concurrency", "workspace.inspector.audio.concurrency")
                .Options(audioConcurrency),
            Bind<Model, &Model::sceneLifecycle, Control::Choice>("audio_lifecycle", "workspace.inspector.audio.lifecycle")
                .Options(audioLifecycles),
        });
    };
}  // namespace Horo::Editor::InspectorFields
