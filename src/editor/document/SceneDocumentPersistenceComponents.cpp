#include "editor/document/SceneDocumentPersistenceInternal.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <variant>

namespace Horo::Editor::ScenePersistenceDetail {
    [[nodiscard]] Json BehaviorFieldValueJson(const Gameplay::BehaviorFieldValue &value) {
        return std::visit([]<typename T>(const T &typed) -> Json {
            if constexpr (std::is_same_v<T, std::monostate>) {
                return {{"type", "null"}, {"value", nullptr}};
            } else if constexpr (std::is_same_v<T, bool>) {
                return {{"type", "bool"}, {"value", typed}};
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return {{"type", "int"}, {"value", typed}};
            } else if constexpr (std::is_same_v<T, double>) {
                return {{"type", "number"}, {"value", typed}};
            } else if constexpr (std::is_same_v<T, std::string>) {
                return {{"type", "string"}, {"value", typed}};
            } else if constexpr (std::is_same_v<T, Math::Vec2>) {
                return {{"type", "vec2"}, {"value", Vec2Json(typed)}};
            } else if constexpr (std::is_same_v<T, Math::Vec3>) {
                return {{"type", "vec3"}, {"value", Vec3Json(typed)}};
            } else {
                return {{"type", "quaternion"}, {"value", QuaternionJson(typed)}};
            }
        }, value);
    }

    [[nodiscard]] Result<Gameplay::BehaviorFieldValue> ParseBehaviorFieldValue(const Json &value) {
        if (!value.is_object() || !value.contains("type") || !value["type"].is_string() || !value.contains("value")) {
            return Result<Gameplay::BehaviorFieldValue>::Failure(PersistenceError(SceneInvalid, "Behavior field is invalid."));
        }
        const std::string type = value["type"].get<std::string>();
        const Json &payload = value["value"];
        if (type == "null" && payload.is_null()) {
            return Result<Gameplay::BehaviorFieldValue>::Success(std::monostate{});
        }
        if (type == "bool" && payload.is_boolean()) {
            return Result<Gameplay::BehaviorFieldValue>::Success(payload.get<bool>());
        }
        if (type == "int" && payload.is_number_integer()) {
            return Result<Gameplay::BehaviorFieldValue>::Success(payload.get<std::int64_t>());
        }
        if (type == "number" && payload.is_number()) {
            return Result<Gameplay::BehaviorFieldValue>::Success(payload.get<double>());
        }
        if (type == "string" && payload.is_string()) {
            return Result<Gameplay::BehaviorFieldValue>::Success(payload.get<std::string>());
        }
        if (type == "vec2") {
            auto parsed = ParseVec2(payload);
            if (parsed.HasValue()) {
                return Result<Gameplay::BehaviorFieldValue>::Success(parsed.Value());
            }
        }
        if (type == "vec3") {
            auto parsed = ParseVec3(payload);
            if (parsed.HasValue()) {
                return Result<Gameplay::BehaviorFieldValue>::Success(parsed.Value());
            }
        }
        if (type == "quaternion") {
            auto parsed = ParseQuaternion(payload);
            if (parsed.HasValue()) {
                return Result<Gameplay::BehaviorFieldValue>::Success(parsed.Value());
            }
        }
        return Result<Gameplay::BehaviorFieldValue>::Failure(PersistenceError(SceneInvalid, "Behavior field type is unsupported."));
    }

    /** @brief Appends a navigation surface payload. */
    void AppendNavigationSurface(Json &value, const Runtime::NavigationSurfaceComponent &surface) {
        Json profiles = Json::array();
        for (const Navigation::NavigationAgentProfileId profile : surface.profiles)
            profiles.push_back(profile.Value());
        value["navigationSurface"] = {
            {"id", surface.id.Value()},
            {"definition", surface.definition.ToString()},
            {"schemaVersion", surface.schemaVersion},
            {"generation", surface.generation},
            {"bakeScope", surface.bakeScope == Runtime::NavigationBakeScope::ObjectSubtree ? "object_subtree" : "local_bounds"},
            {"localBounds", surface.localBounds ? Json{{"center", Vec3Json(surface.localBounds->center)},
                                                       {"halfExtents", Vec3Json(surface.localBounds->halfExtents)}}
                                                : Json(nullptr)},
            {"profiles", std::move(profiles)},
            {"enabled", surface.enabled},
        };
    }

    /** @brief Appends a navigation region payload. */
    void AppendNavigationRegion(Json &value, const Runtime::NavigationRegionComponent &region) {
        value["navigationRegion"] = {
            {"id", region.id.Value()},
            {"surface", region.surface.Value()},
            {"schemaVersion", region.schemaVersion},
            {"generation", region.generation},
            {"localBounds", {{"center", Vec3Json(region.localBounds.center)}, {"halfExtents", Vec3Json(region.localBounds.halfExtents)}}},
            {"sourceSelection", region.sourceSelection == Runtime::NavigationRegionSourceSelection::ExplicitContributors
                                    ? "explicit_contributors"
                                    : "static_collision_in_bounds"},
            {"mode", region.mode == Runtime::NavigationRegionMode::Include ? "include" : "exclude"},
            {"enabled", region.enabled},
        };
    }

    /** @brief Appends a navigation modifier payload. */
    void AppendNavigationModifier(Json &value, const Runtime::NavigationModifierComponent &modifier) {
        Json volume;
        if (const auto *box = std::get_if<Runtime::NavigationLocalBounds>(&modifier.volume)) {
            volume = {{"shape", "box"}, {"center", Vec3Json(box->center)}, {"halfExtents", Vec3Json(box->halfExtents)}};
        } else {
            const Runtime::NavigationCylinderVolume &cylinder = std::get<Runtime::NavigationCylinderVolume>(modifier.volume);
            volume = {{"shape", "cylinder"},
                      {"center", Vec3Json(cylinder.center)},
                      {"radius", cylinder.radius},
                      {"halfHeight", cylinder.halfHeight}};
        }
        using enum Runtime::NavigationModifierOperation;
        const char *operation = "exclude";
        if (modifier.operation == OverrideArea)
            operation = "override_area";
        else if (modifier.operation == OverrideAreaAndCost)
            operation = "override_area_and_cost";
        value["navigationModifier"] = {
            {"id", modifier.id.Value()},
            {"surface", modifier.surface.Value()},
            {"schemaVersion", modifier.schemaVersion},
            {"generation", modifier.generation},
            {"volume", std::move(volume)},
            {"operation", operation},
            {"area", modifier.area ? Json(modifier.area->Value()) : Json(nullptr)},
            {"traversalCost", modifier.traversalCost.has_value() ? Json(*modifier.traversalCost) : Json(nullptr)},
            {"enabled", modifier.enabled},
        };
    }

    /** @brief Appends a navigation link payload. */
    void AppendNavigationLink(Json &value, const Runtime::NavigationLinkComponent &link) {
        Json profiles = Json::array();
        for (const Navigation::NavigationAgentProfileId profile : link.profiles)
            profiles.push_back(profile.Value());
        using enum Runtime::NavigationLinkKind;
        const char *kind = "teleport";
        if (link.kind == Jump)
            kind = "jump";
        else if (link.kind == Ladder)
            kind = "ladder";
        else if (link.kind == Door)
            kind = "door";
        value["navigationLink"] = {
            {"id", link.id.Value()},
            {"schemaVersion", link.schemaVersion},
            {"generation", link.generation},
            {"start",
             {{"surface", link.start.surface.Value()},
              {"localPosition", Vec3Json(link.start.localPosition)},
              {"connectionRadiusMeters", link.start.connectionRadiusMeters}}},
            {"end",
             {{"surface", link.end.surface.Value()},
              {"localPosition", Vec3Json(link.end.localPosition)},
              {"connectionRadiusMeters", link.end.connectionRadiusMeters}}},
            {"kind", kind},
            {"direction", link.direction == Runtime::NavigationLinkDirection::StartToEnd ? "start_to_end" : "bidirectional"},
            {"profiles", std::move(profiles)},
            {"traversalCost", link.traversalCost},
            {"enabled", link.enabled},
        };
    }

    /** @brief Appends optional navigation authoring payloads without increasing the core component serializer's branching. */
    void AppendNavigationComponents(Json &value, const SceneObjectComponentSet &components) {
        if (components.navigationSurface)
            AppendNavigationSurface(value, *components.navigationSurface);
        if (components.navigationRegion)
            AppendNavigationRegion(value, *components.navigationRegion);
        if (components.navigationModifier)
            AppendNavigationModifier(value, *components.navigationModifier);
        if (components.navigationLink)
            AppendNavigationLink(value, *components.navigationLink);
    }

    [[nodiscard]] Json PhysicsPoseJson(const Runtime::AuthoredPhysicsPose &pose) {
        return {{"translation", Vec3Json(pose.translation)},
                {"rotation", {pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w}}};
    }

    [[nodiscard]] Json PhysicsBodyReferenceJson(const Runtime::PhysicsBodyReference &reference) {
        return {{"object", reference.object.value}, {"body", reference.body.value}};
    }

    [[nodiscard]] const char *PhysicsMotionName(const Runtime::AuthoredPhysicsMotionType motion) noexcept {
        using enum Runtime::AuthoredPhysicsMotionType;
        switch (motion) {
            case Static:
                return "static";
            case Kinematic:
                return "kinematic";
            case Dynamic:
                return "dynamic";
            case Count:
                break;
        }
        return "dynamic";
    }

    [[nodiscard]] Json PhysicsMassJson(const Runtime::AuthoredPhysicsMassPolicy &policy) {
        return std::visit([]<typename Mass>(const Mass &mass) -> Json {
            if constexpr (std::is_same_v<Mass, Runtime::AuthoredPhysicsNoMass>)
                return {{"kind", "none"}};
            else if constexpr (std::is_same_v<Mass, Runtime::AuthoredPhysicsMass>)
                return {{"kind", "mass"}, {"kilograms", mass.kilograms}};
            else
                return {{"kind", "density"}, {"kilogramsPerCubicMeter", mass.kilogramsPerCubicMeter}};
        }, policy);
    }

    [[nodiscard]] Json PhysicsColliderSourceJson(const Runtime::PhysicsColliderSource &source) {
        if (const auto *asset = std::get_if<Runtime::PhysicsShapeAssetReference>(&source))
            return {{"kind", "asset"}, {"asset", asset->asset.ToString()}, {"subresource", asset->subresource.value}};
        const Runtime::PhysicsAnalyticCollider &analytic = std::get<Runtime::PhysicsAnalyticCollider>(source);
        if (const auto *box = std::get_if<Runtime::PhysicsBoxCollider>(&analytic))
            return {{"kind", "box"}, {"halfExtentsMeters", Vec3Json(box->halfExtentsMeters)}};
        if (const auto *sphere = std::get_if<Runtime::PhysicsSphereCollider>(&analytic))
            return {{"kind", "sphere"}, {"radiusMeters", sphere->radiusMeters}};
        if (const auto *capsule = std::get_if<Runtime::PhysicsCapsuleCollider>(&analytic))
            return {{"kind", "capsule"},
                    {"radiusMeters", capsule->radiusMeters},
                    {"cylindricalHalfHeightMeters", capsule->cylindricalHalfHeightMeters}};
        const auto &plane = std::get<Runtime::PhysicsStaticPlaneCollider>(analytic);
        return {{"kind", "static_plane"}, {"normal", Vec3Json(plane.normal)}, {"signedDistanceMeters", plane.signedDistanceMeters}};
    }

    void AppendRigidBody(Json &value, const Runtime::RigidBodyComponent &body) {
        value["rigidBody"] = {{"id", body.id.value},
                              {"body", body.body.value},
                              {"schemaVersion", body.schemaVersion},
                              {"generation", body.generation},
                              {"motion", PhysicsMotionName(body.motion)},
                              {"mass", PhysicsMassJson(body.mass)},
                              {"initialLinearVelocity", Vec3Json(body.initialLinearVelocity)},
                              {"initialAngularVelocity", Vec3Json(body.initialAngularVelocity)},
                              {"linearDampingPerSecond", body.linearDampingPerSecond},
                              {"angularDampingPerSecond", body.angularDampingPerSecond},
                              {"maximumLinearSpeed", body.maximumLinearSpeed},
                              {"maximumAngularSpeed", body.maximumAngularSpeed},
                              {"enabled", body.enabled}};
    }

    void AppendColliders(Json &value, const std::vector<Runtime::ColliderComponent> &components) {
        Json colliders = Json::array();
        for (const Runtime::ColliderComponent &collider : components) {
            Json materials = Json::array();
            for (const Runtime::PhysicsColliderMaterialBinding &binding : collider.materials)
                materials.push_back({{"slot", binding.slot.Value()}, {"material", binding.material.ToString()}});
            colliders.push_back({{"id", collider.id.value},
                                 {"collider", collider.collider.value},
                                 {"schemaVersion", collider.schemaVersion},
                                 {"generation", collider.generation},
                                 {"body", PhysicsBodyReferenceJson(collider.body)},
                                 {"source", PhysicsColliderSourceJson(collider.source)},
                                 {"localPose", PhysicsPoseJson(collider.localPose)},
                                 {"scale", Vec3Json(collider.scale)},
                                 {"collisionProfile", collider.collisionProfile.ToString()},
                                 {"materials", std::move(materials)},
                                 {"sensor", collider.sensor},
                                 {"enabled", collider.enabled}});
        }
        if (!colliders.empty())
            value["colliders"] = std::move(colliders);
    }

    [[nodiscard]] Json PhysicsConstraintEndpointJson(const Runtime::PhysicsConstraintSecondEndpoint &endpoint) {
        if (const auto *body = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&endpoint))
            return {{"kind", "body"}, {"body", PhysicsBodyReferenceJson(body->body)}, {"frame", PhysicsPoseJson(body->localFrame)}};
        return {{"kind", "world"}, {"frame", PhysicsPoseJson(std::get<Runtime::PhysicsConstraintWorldEndpoint>(endpoint).frame)}};
    }

    [[nodiscard]] Json PhysicsConstraintParametersJson(
        const std::variant<Runtime::PhysicsFixedConstraint, Runtime::PhysicsDistanceConstraint> &parameters) {
        if (const auto *distance = std::get_if<Runtime::PhysicsDistanceConstraint>(&parameters))
            return {{"kind", "distance"}, {"minimumMeters", distance->minimumMeters}, {"maximumMeters", distance->maximumMeters}};
        return {{"kind", "fixed"}};
    }

    void AppendPhysicsConstraints(Json &value, const std::vector<Runtime::PhysicsConstraintComponent> &components) {
        Json constraints = Json::array();
        for (const Runtime::PhysicsConstraintComponent &constraint : components) {
            constraints.push_back(
                {{"id", constraint.id.value},
                 {"constraint", constraint.constraint.value},
                 {"schemaVersion", constraint.schemaVersion},
                 {"generation", constraint.generation},
                 {"first",
                  {{"body", PhysicsBodyReferenceJson(constraint.first.body)}, {"frame", PhysicsPoseJson(constraint.first.localFrame)}}},
                 {"second", PhysicsConstraintEndpointJson(constraint.second)},
                 {"parameters", PhysicsConstraintParametersJson(constraint.parameters)},
                 {"enabled", constraint.enabled}});
        }
        if (!constraints.empty())
            value["physicsConstraints"] = std::move(constraints);
    }

    void AppendPhysicsComponents(Json &value, const SceneObjectComponentSet &components) {
        if (components.rigidBody)
            AppendRigidBody(value, *components.rigidBody);
        AppendColliders(value, components.colliders);
        AppendPhysicsConstraints(value, components.physicsConstraints);
    }

    [[nodiscard]] const char *AudioSoundReferenceKindName(const Audio::AudioSoundReferenceKind kind) {
        using enum Audio::AudioSoundReferenceKind;
        switch (kind) {
            case Unassigned:
                return "unassigned";
            case Clip:
                return "clip";
            case Variation:
                return "variation";
            case Stream:
                return "stream";
            case Music:
                return "music";
            case Extension:
                return "extension";
        }
        return "unassigned";
    }

    [[nodiscard]] const char *AudioSpatialModeName(const Audio::AudioSpatialMode mode) {
        switch (mode) {
            case Audio::AudioSpatialMode::TwoD:
                return "2d";
            case Audio::AudioSpatialMode::ThreeD:
                return "3d";
        }
        return "3d";
    }

    [[nodiscard]] const char *AudioConcurrencyModeName(const Audio::AudioConcurrencyMode mode) {
        using enum Audio::AudioConcurrencyMode;
        switch (mode) {
            case Allow:
                return "allow";
            case Reject:
                return "reject";
            case StealOldest:
                return "steal_oldest";
            case StealQuietest:
                return "steal_quietest";
            case Virtualize:
                return "virtualize";
        }
        return "allow";
    }

    [[nodiscard]] const char *AudioSceneLifecyclePolicyName(const Audio::AudioSceneLifecyclePolicy policy) {
        switch (policy) {
            case Audio::AudioSceneLifecyclePolicy::StopOnUnload:
                return "stop_on_unload";
            case Audio::AudioSceneLifecyclePolicy::KeepAliveInHostContext:
                return "keep_alive_in_host_context";
        }
        return "stop_on_unload";
    }

    [[nodiscard]] Json AudioConcurrencyPolicyJson(const Audio::AudioConcurrencyPolicy &policy) {
        Json value{{"mode", AudioConcurrencyModeName(policy.mode)}, {"maxInstances", policy.maxInstances}};
        if (policy.group.has_value())
            value["group"] = policy.group->Value();
        return value;
    }

    [[nodiscard]] Json AudioSoundReferenceJson(const Audio::AudioSoundReference &reference) {
        using enum Audio::AudioSoundReferenceKind;
        Json value{{"kind", AudioSoundReferenceKindName(reference.kind)}};
        if (reference.kind == Clip) {
            if (const auto *clip = std::get_if<Audio::AudioClipId>(&reference.target))
                value["asset"] = clip->Asset().ToString();
        } else if (reference.kind == Variation || reference.kind == Stream || reference.kind == Music) {
            if (const auto *sound = std::get_if<Audio::AudioSoundId>(&reference.target))
                value["asset"] = sound->Asset().ToString();
        } else if (reference.kind == Extension) {
            if (const auto *extension = std::get_if<Audio::AudioSoundExtensionReference>(&reference.target)) {
                value["asset"] = extension->definition.Asset().ToString();
                value["contribution"] = extension->contribution.Value();
                value["contractMajor"] = extension->contractVersion.major;
                value["contractMinor"] = extension->contractVersion.minor;
            }
        }
        return value;
    }

    [[nodiscard]] Result<Assets::AssetId> ParseAudioReferenceAsset(const Json &value) {
        if (!value.contains("asset") || !value["asset"].is_string())
            return Result<Assets::AssetId>::Failure(PersistenceError(SceneInvalid, "Audio sound reference asset is invalid."));
        auto asset = Assets::AssetId::Parse(value["asset"].get<std::string>());
        if (asset.HasError())
            return Result<Assets::AssetId>::Failure(PersistenceError(SceneInvalid, "Audio sound reference asset is invalid."));
        return asset;
    }

    [[nodiscard]] Result<Audio::AudioSoundReference> ParseAudioSoundReference(const Json &value) {
        if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string())
            return Result<Audio::AudioSoundReference>::Failure(PersistenceError(SceneInvalid, "Audio sound reference is incomplete."));
        const std::string kind = value["kind"].get<std::string>();

        // These names were emitted before the typed reference contract existed. They carried no identity, so
        // preserve the scene component as an editor-unassigned source rather than inventing one.
        if (kind == "native_clip" || kind == "middleware_event" || kind == "unassigned")
            return Result<Audio::AudioSoundReference>::Success({});

        auto asset = ParseAudioReferenceAsset(value);
        if (asset.HasError())
            return Result<Audio::AudioSoundReference>::Failure(asset.ErrorValue());
        using enum Audio::AudioSoundReferenceKind;
        if (kind == "clip") {
            const auto clip = Audio::AudioClipId::Create(asset.Value());
            if (clip.HasError())
                return Result<Audio::AudioSoundReference>::Failure(clip.ErrorValue());
            return Audio::AudioSoundReference::ForClip(clip.Value());
        }
        auto sound = Audio::AudioSoundId::Create(asset.Value());
        if (sound.HasError())
            return Result<Audio::AudioSoundReference>::Failure(sound.ErrorValue());
        if (kind == "variation")
            return Audio::AudioSoundReference::ForVariation(sound.Value());
        if (kind == "stream")
            return Audio::AudioSoundReference::ForStream(sound.Value());
        if (kind == "music")
            return Audio::AudioSoundReference::ForMusic(sound.Value());
        if (kind != "extension" || !value.contains("contribution") || !value["contribution"].is_number_unsigned())
            return Result<Audio::AudioSoundReference>::Failure(PersistenceError(SceneInvalid, "Audio sound reference kind is invalid."));
        const auto contribution = Audio::AudioContributionId::Create(value["contribution"].get<std::uint64_t>());
        if (contribution.HasError())
            return Result<Audio::AudioSoundReference>::Failure(
                PersistenceError(SceneInvalid, "Audio sound contribution identity is invalid."));
        const auto contractVersion = Audio::AudioSoundDefinitionSchemaVersion{value.value("contractMajor", std::uint16_t{1}),
                                                                              value.value("contractMinor", std::uint16_t{0})};
        return Audio::AudioSoundReference::ForExtension(contribution.Value(), sound.Value(), contractVersion);
    }

    /** @brief Appends the camera component payload. */
    void AppendCameraJson(Json &value, const Runtime::CameraComponent &camera) {
        value["camera"] = {
            {"projection", camera.projection == Runtime::CameraProjection::Perspective ? "perspective" : "orthographic"},
            {"verticalFieldOfViewRadians", camera.verticalFieldOfViewRadians},
            {"orthographicHeight", camera.orthographicHeight},
            {"nearPlane", camera.nearPlane},
            {"farPlane", camera.farPlane},
            {"enabled", camera.enabled},
        };
    }

    /** @brief Appends the light component payload. */
    void AppendLightJson(Json &value, const Runtime::LightComponent &light) {
        using enum Runtime::LightKind;
        const char *kind = "spot";
        if (light.kind == Directional)
            kind = "directional";
        else if (light.kind == Point)
            kind = "point";
        value["light"] = {
            {"kind", kind},
            {"color", Vec3Json(light.color)},
            {"intensity", light.intensity},
            {"range", light.range},
            {"innerConeRadians", light.innerConeRadians},
            {"outerConeRadians", light.outerConeRadians},
            {"enabled", light.enabled},
        };
    }

    /** @brief Appends the trigger-volume component payload. */
    void AppendTriggerVolumeJson(Json &value, const Runtime::TriggerVolumeComponent &triggerVolume) {
        value["triggerVolume"] = {
            {"shape", static_cast<std::uint8_t>(triggerVolume.shape)},
            {"enabled", triggerVolume.enabled},
        };
    }

    /** @brief Appends the audio-source component payload. */
    void AppendAudioSourceJson(Json &value, const Runtime::AudioSourceComponent &audio) {
        value["audioSource"] = {
            {"sound", AudioSoundReferenceJson(audio.sound)},
            {"gain", audio.playback.gain},
            {"pitch", audio.playback.pitch},
            {"loop", audio.playback.loop},
            {"spatialMode", AudioSpatialModeName(audio.playback.spatialMode)},
            {"enableDoppler", audio.playback.enableDoppler},
            {"playOnStart", audio.playback.playOnStart},
            {"priority", audio.playback.priority},
            {"concurrency", AudioConcurrencyPolicyJson(audio.playback.concurrency)},
            {"sceneLifecycle", AudioSceneLifecyclePolicyName(audio.sceneLifecycle)},
            {"enabled", audio.enabled},
        };
        if (audio.playback.bus.has_value())
            value["audioSource"]["bus"] = audio.playback.bus->Value();
    }

    /** @brief Appends attached behavior payloads. */
    void AppendBehaviorsJson(Json &value, const std::vector<Gameplay::BehaviorComponent> &behaviors) {
        if (behaviors.empty())
            return;
        Json serialized = Json::array();
        for (const Gameplay::BehaviorComponent &behavior : behaviors) {
            Json fields = Json::array();
            for (const Gameplay::BehaviorField &field : behavior.fields)
                fields.push_back({{"name", field.name}, {"value", BehaviorFieldValueJson(field.value)}});
            serialized.push_back({
                {"instanceId", behavior.instanceId.value},
                {"typeId", behavior.typeId.Value()},
                {"schemaVersion", behavior.schemaVersion},
                {"enabled", behavior.enabled},
                {"fields", std::move(fields)},
            });
        }
        value["behaviors"] = std::move(serialized);
    }

    [[nodiscard]] Json ComponentsJson(const SceneObjectComponentSet &components) {
        Json value = Json::object();
        if (components.camera)
            AppendCameraJson(value, *components.camera);
        if (components.light)
            AppendLightJson(value, *components.light);
        if (components.triggerVolume)
            AppendTriggerVolumeJson(value, *components.triggerVolume);
        if (components.audioSource)
            AppendAudioSourceJson(value, *components.audioSource);
        AppendNavigationComponents(value, components);
        AppendPhysicsComponents(value, components);
        AppendBehaviorsJson(value, components.behaviors);
        return value;
    }

}  // namespace Horo::Editor::ScenePersistenceDetail
