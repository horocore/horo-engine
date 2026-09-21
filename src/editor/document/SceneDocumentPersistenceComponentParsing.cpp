#include "editor/document/SceneDocumentPersistenceInternal.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace Horo::Editor::ScenePersistenceDetail {
    [[nodiscard]] Result<Runtime::CameraComponent> ParseCameraComponent(const Json &camera) {
        if (!camera.is_object() || !camera.contains("projection") || !camera["projection"].is_string()) {
            return Result<Runtime::CameraComponent>::Failure(PersistenceError(SceneInvalid, "Camera is invalid."));
        }
        const std::string projection = camera["projection"].get<std::string>();
        if (projection != "perspective" && projection != "orthographic") {
            return Result<Runtime::CameraComponent>::Failure(PersistenceError(SceneInvalid, "Camera projection is invalid."));
        }
        return Result<Runtime::CameraComponent>::Success(Runtime::CameraComponent{
            .projection = projection == "perspective" ? Runtime::CameraProjection::Perspective : Runtime::CameraProjection::Orthographic,
            .verticalFieldOfViewRadians = camera.at("verticalFieldOfViewRadians").get<float>(),
            .orthographicHeight = camera.at("orthographicHeight").get<float>(),
            .nearPlane = camera.at("nearPlane").get<float>(),
            .farPlane = camera.at("farPlane").get<float>(),
            .enabled = camera.value("enabled", true),
        });
    }

    [[nodiscard]] Result<Runtime::LightComponent> ParseLightComponent(const Json &light) {
        const std::string kind = light.at("kind").get<std::string>();
        auto color = ParseVec3(light.at("color"));
        if (color.HasError() || (kind != "directional" && kind != "point" && kind != "spot")) {
            return Result<Runtime::LightComponent>::Failure(PersistenceError(SceneInvalid, "Light is invalid."));
        }
        Runtime::LightKind lightKind = Runtime::LightKind::Spot;
        if (kind == "directional") {
            lightKind = Runtime::LightKind::Directional;
        } else if (kind == "point") {
            lightKind = Runtime::LightKind::Point;
        }
        return Result<Runtime::LightComponent>::Success(Runtime::LightComponent{
            .kind = lightKind,
            .color = color.Value(),
            .intensity = light.at("intensity").get<float>(),
            .range = light.at("range").get<float>(),
            .innerConeRadians = light.at("innerConeRadians").get<float>(),
            .outerConeRadians = light.at("outerConeRadians").get<float>(),
            .enabled = light.value("enabled", true),
        });
    }

    [[nodiscard]] Result<Runtime::TriggerVolumeComponent> ParseTriggerVolumeComponent(const Json &triggerVolume) {
        const std::uint8_t shape = triggerVolume.at("shape").get<std::uint8_t>();
        if (shape > static_cast<std::uint8_t>(Runtime::ColliderShapeType::StaticPlane)) {
            return Result<Runtime::TriggerVolumeComponent>::Failure(PersistenceError(SceneInvalid, "Trigger shape is invalid."));
        }
        return Result<Runtime::TriggerVolumeComponent>::Success(Runtime::TriggerVolumeComponent{
            .shape = static_cast<Runtime::ColliderShapeType>(shape),
            .enabled = triggerVolume.value("enabled", true),
        });
    }

    [[nodiscard]] Result<Audio::AudioSpatialMode> ParseAudioSpatialMode(const Json &audio) {
        using enum Audio::AudioSpatialMode;
        if (audio.contains("spatialMode")) {
            if (!audio["spatialMode"].is_string())
                return Result<Audio::AudioSpatialMode>::Failure(PersistenceError(SceneInvalid, "Audio spatial mode is invalid."));
            const std::string mode = audio["spatialMode"].get<std::string>();
            if (mode == "2d")
                return Result<Audio::AudioSpatialMode>::Success(TwoD);
            if (mode == "3d")
                return Result<Audio::AudioSpatialMode>::Success(ThreeD);
            return Result<Audio::AudioSpatialMode>::Failure(PersistenceError(SceneInvalid, "Audio spatial mode is invalid."));
        }

        // Scene files written before the typed spatial mode used a boolean.
        if (audio.contains("spatial")) {
            if (!audio["spatial"].is_boolean())
                return Result<Audio::AudioSpatialMode>::Failure(PersistenceError(SceneInvalid, "Audio spatial mode is invalid."));
            return Result<Audio::AudioSpatialMode>::Success(audio["spatial"].get<bool>() ? ThreeD : TwoD);
        }
        return Result<Audio::AudioSpatialMode>::Success(ThreeD);
    }

    [[nodiscard]] Result<Audio::AudioConcurrencyMode> ParseAudioConcurrencyMode(const Json &value) {
        using enum Audio::AudioConcurrencyMode;
        if (!value.is_string())
            return Result<Audio::AudioConcurrencyMode>::Failure(PersistenceError(SceneInvalid, "Audio concurrency mode is invalid."));
        const std::string mode = value.get<std::string>();
        if (mode == "allow")
            return Result<Audio::AudioConcurrencyMode>::Success(Allow);
        if (mode == "reject")
            return Result<Audio::AudioConcurrencyMode>::Success(Reject);
        if (mode == "steal_oldest")
            return Result<Audio::AudioConcurrencyMode>::Success(StealOldest);
        if (mode == "steal_quietest")
            return Result<Audio::AudioConcurrencyMode>::Success(StealQuietest);
        if (mode == "virtualize")
            return Result<Audio::AudioConcurrencyMode>::Success(Virtualize);
        return Result<Audio::AudioConcurrencyMode>::Failure(PersistenceError(SceneInvalid, "Audio concurrency mode is invalid."));
    }

    [[nodiscard]] Result<Audio::AudioConcurrencyGroupId> ParseAudioConcurrencyGroup(const Json &value) {
        if (!value.is_number_unsigned())
            return Result<Audio::AudioConcurrencyGroupId>::Failure(
                PersistenceError(SceneInvalid, "Audio concurrency group identity is invalid."));
        auto group = Audio::AudioConcurrencyGroupId::Create(value.get<std::uint64_t>());
        if (group.HasError())
            return Result<Audio::AudioConcurrencyGroupId>::Failure(
                PersistenceError(SceneInvalid, "Audio concurrency group identity is invalid."));
        return Result<Audio::AudioConcurrencyGroupId>::Success(group.Value());
    }

    [[nodiscard]] Result<std::uint16_t> ParseAudioConcurrencyMaximum(const Json &value) {
        if (!value.is_number_unsigned() || value.get<std::uint64_t>() > std::numeric_limits<std::uint16_t>::max())
            return Result<std::uint16_t>::Failure(PersistenceError(SceneInvalid, "Audio concurrency maximum is invalid."));
        return Result<std::uint16_t>::Success(value.get<std::uint16_t>());
    }

    [[nodiscard]] Result<Audio::AudioConcurrencyPolicy> ParseAudioConcurrencyPolicy(const Json &audio) {
        Audio::AudioConcurrencyPolicy policy;
        if (!audio.contains("concurrency"))
            return Result<Audio::AudioConcurrencyPolicy>::Success(policy);
        const Json &value = audio["concurrency"];
        if (!value.is_object())
            return Result<Audio::AudioConcurrencyPolicy>::Failure(PersistenceError(SceneInvalid, "Audio concurrency policy is invalid."));
        if (value.contains("group")) {
            auto group = ParseAudioConcurrencyGroup(value["group"]);
            if (group.HasError())
                return Result<Audio::AudioConcurrencyPolicy>::Failure(group.ErrorValue());
            policy.group = group.Value();
        }
        if (value.contains("maxInstances")) {
            auto maximum = ParseAudioConcurrencyMaximum(value["maxInstances"]);
            if (maximum.HasError())
                return Result<Audio::AudioConcurrencyPolicy>::Failure(maximum.ErrorValue());
            policy.maxInstances = maximum.Value();
        }
        if (value.contains("mode")) {
            auto mode = ParseAudioConcurrencyMode(value["mode"]);
            if (mode.HasError())
                return Result<Audio::AudioConcurrencyPolicy>::Failure(mode.ErrorValue());
            policy.mode = mode.Value();
        }
        return Result<Audio::AudioConcurrencyPolicy>::Success(policy);
    }

    [[nodiscard]] Result<Audio::AudioSceneLifecyclePolicy> ParseAudioSceneLifecyclePolicy(const Json &audio) {
        using enum Audio::AudioSceneLifecyclePolicy;
        if (!audio.contains("sceneLifecycle"))
            return Result<Audio::AudioSceneLifecyclePolicy>::Success(StopOnUnload);
        if (!audio["sceneLifecycle"].is_string())
            return Result<Audio::AudioSceneLifecyclePolicy>::Failure(
                PersistenceError(SceneInvalid, "Audio scene lifecycle policy is invalid."));
        const std::string policy = audio["sceneLifecycle"].get<std::string>();
        if (policy == "stop_on_unload")
            return Result<Audio::AudioSceneLifecyclePolicy>::Success(StopOnUnload);
        if (policy == "keep_alive_in_host_context")
            return Result<Audio::AudioSceneLifecyclePolicy>::Success(KeepAliveInHostContext);
        return Result<Audio::AudioSceneLifecyclePolicy>::Failure(
            PersistenceError(SceneInvalid, "Audio scene lifecycle policy is invalid."));
    }

    [[nodiscard]] Result<Audio::AudioPriority> ParseAudioPriority(const Json &audio) {
        if (!audio.contains("priority"))
            return Result<Audio::AudioPriority>::Success(128);
        const Json &value = audio["priority"];
        if (!value.is_number_unsigned() || value.get<std::uint64_t>() > Audio::MaximumAudioPriority)
            return Result<Audio::AudioPriority>::Failure(PersistenceError(SceneInvalid, "Audio priority is invalid."));
        return Result<Audio::AudioPriority>::Success(static_cast<Audio::AudioPriority>(value.get<std::uint64_t>()));
    }

    [[nodiscard]] Result<std::optional<Audio::AudioBusId>> ParseAudioBus(const Json &audio) {
        if (!audio.contains("bus"))
            return Result<std::optional<Audio::AudioBusId>>::Success(std::nullopt);
        if (!audio["bus"].is_number_unsigned())
            return Result<std::optional<Audio::AudioBusId>>::Failure(
                PersistenceError(SceneInvalid, "Audio source bus identity is invalid."));
        auto bus = Audio::AudioBusId::Create(audio["bus"].get<std::uint64_t>());
        if (bus.HasError())
            return Result<std::optional<Audio::AudioBusId>>::Failure(
                PersistenceError(SceneInvalid, "Audio source bus identity is invalid."));
        return Result<std::optional<Audio::AudioBusId>>::Success(bus.Value());
    }

    [[nodiscard]] Result<Runtime::AudioSourceComponent> ParseAudioSourceComponent(const Json &audio) {
        if (!audio.is_object()) {
            return Result<Runtime::AudioSourceComponent>::Failure(PersistenceError(SceneInvalid, "Audio source is invalid."));
        }
        const Json &reference = audio.contains("sound") && audio.at("sound").is_object() ? audio.at("sound") : audio;
        auto sound = ParseAudioSoundReference(reference);
        if (sound.HasError())
            return Result<Runtime::AudioSourceComponent>::Failure(sound.ErrorValue());
        auto bus = ParseAudioBus(audio);
        if (bus.HasError())
            return Result<Runtime::AudioSourceComponent>::Failure(bus.ErrorValue());
        auto spatialMode = ParseAudioSpatialMode(audio);
        if (spatialMode.HasError())
            return Result<Runtime::AudioSourceComponent>::Failure(spatialMode.ErrorValue());
        auto concurrency = ParseAudioConcurrencyPolicy(audio);
        if (concurrency.HasError())
            return Result<Runtime::AudioSourceComponent>::Failure(concurrency.ErrorValue());
        auto sceneLifecycle = ParseAudioSceneLifecyclePolicy(audio);
        if (sceneLifecycle.HasError())
            return Result<Runtime::AudioSourceComponent>::Failure(sceneLifecycle.ErrorValue());
        auto priority = ParseAudioPriority(audio);
        if (priority.HasError())
            return Result<Runtime::AudioSourceComponent>::Failure(priority.ErrorValue());
        return Result<Runtime::AudioSourceComponent>::Success(Runtime::AudioSourceComponent{
            .sound = std::move(sound).Value(),
            .playback = Audio::AudioSoundPlaybackDefaults{.gain = audio.at("gain").get<float>(),
                                                          .pitch = audio.value("pitch", 1.0F),
                                                          .bus = bus.Value(),
                                                          .loop = audio.value("loop", false),
                                                          .spatialMode = spatialMode.Value(),
                                                          .enableDoppler = audio.value("enableDoppler", false),
                                                          .playOnStart = audio.value("playOnStart", true),
                                                          .priority = priority.Value(),
                                                          .concurrency = concurrency.Value()},
            .sceneLifecycle = sceneLifecycle.Value(),
            .enabled = audio.value("enabled", true),
        });
    }

    template <typename Component, typename Parser>
    [[nodiscard]] Result<void> ParseOptionalComponent(const Json &value, const std::string_view name, std::optional<Component> &destination,
                                                      Parser &&parser) {
        if (!value.contains(name))
            return Result<void>::Success();
        auto parsed = std::invoke(std::forward<Parser>(parser), value[name]);
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        destination = std::move(parsed).Value();
        return Result<void>::Success();
    }

    [[nodiscard]] Result<SceneObjectComponentSet> ParseComponents(const Json &value) {
        if (!value.is_object()) {
            return Result<SceneObjectComponentSet>::Failure(PersistenceError(SceneInvalid, "Components must be an object."));
        }
        SceneObjectComponentSet components;
        const auto parse = [&]<typename Component, typename Parser>(const std::string_view name, std::optional<Component> &destination,
                                                                    Parser &&parser) -> Result<void> {
            return ParseOptionalComponent(value, name, destination, std::forward<Parser>(parser));
        };
        if (auto parsed = parse("camera", components.camera, ParseCameraComponent); parsed.HasError())
            return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
        if (auto parsed = parse("light", components.light, ParseLightComponent); parsed.HasError())
            return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
        if (auto parsed = parse("triggerVolume", components.triggerVolume, ParseTriggerVolumeComponent); parsed.HasError())
            return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
        if (auto parsed = parse("audioSource", components.audioSource, ParseAudioSourceComponent); parsed.HasError())
            return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
        if (auto parsed = parse("navigationSurface", components.navigationSurface, ParseNavigationSurface); parsed.HasError())
            return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
        if (auto parsed = parse("navigationRegion", components.navigationRegion, ParseNavigationRegion); parsed.HasError())
            return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
        if (auto parsed = parse("navigationModifier", components.navigationModifier, ParseNavigationModifier); parsed.HasError())
            return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
        if (auto parsed = parse("navigationLink", components.navigationLink, ParseNavigationLink); parsed.HasError())
            return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
        if (auto parsed = parse("rigidBody", components.rigidBody, ParseRigidBody); parsed.HasError())
            return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
        if (value.contains("colliders")) {
            auto colliders = ParseColliders(value["colliders"]);
            if (colliders.HasError())
                return Result<SceneObjectComponentSet>::Failure(colliders.ErrorValue());
            components.colliders = std::move(colliders).Value();
        }
        if (value.contains("physicsConstraints")) {
            auto constraints = ParsePhysicsConstraints(value["physicsConstraints"]);
            if (constraints.HasError())
                return Result<SceneObjectComponentSet>::Failure(constraints.ErrorValue());
            components.physicsConstraints = std::move(constraints).Value();
        }
        if (value.contains("behaviors")) {
            auto behaviors = ParseBehaviors(value["behaviors"]);
            if (behaviors.HasError()) {
                return Result<SceneObjectComponentSet>::Failure(behaviors.ErrorValue());
            }
            components.behaviors = std::move(behaviors).Value();
        }
        return Result<SceneObjectComponentSet>::Success(std::move(components));
    }

}  // namespace Horo::Editor::ScenePersistenceDetail
