#include "editor/document/RuntimeSceneConversion.h"

#include "Horo/Gameplay/Component.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSceneIdentityRemap.h"
#include "editor/document/NavigationAgentJson.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId SceneConversionDomain{"horo.editor.scene_conversion"};
        const ErrorCodeDescriptor PrefabResolutionRequired{
            .domain = SceneConversionDomain,
            .code = ErrorCode{"scene_conversion.prefab_resolution_required"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene prefab instances require a pinned resolver snapshot before runtime conversion.",
            .remediationHint = "Resolve every authored prefab instance before preparing a runtime scene candidate.",
            .retryable = true,
            .userActionable = false,
        };
        const ErrorCodeDescriptor PrefabComponentProjectionUnsupported{
            .domain = SceneConversionDomain,
            .code = ErrorCode{"scene_conversion.prefab_component_projection_unsupported"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "A prefab component cannot be represented by the runtime scene definition.",
            .remediationHint = "Register a typed runtime projection for the component before converting the scene.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor TriggerVolumeSchemaUnsupported{
            .domain = SceneConversionDomain,
            .code = ErrorCode{"scene_conversion.trigger_volume_schema_unsupported"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "A legacy trigger volume cannot be represented by the canonical Physics scene schema.",
            .remediationHint = "Use one of the supported analytic trigger shapes and reload the scene if its schema is stale.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor TriggerVolumeCanonicalConflict{
            .domain = SceneConversionDomain,
            .code = ErrorCode{"scene_conversion.trigger_volume_canonical_conflict"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "A legacy trigger volume overlaps an explicit Physics component declaration.",
            .remediationHint = "Keep either the legacy trigger authoring component or the canonical body/collider declaration, then retry.",
            .retryable = false,
            .userActionable = true,
        };
        constexpr std::string_view NavigationAgentPrefabComponentType = "game.horo.navigation_agent";
        constexpr std::string_view AiAgentPrefabComponentType = "game.horo.ai_agent";
        constexpr std::string_view AiControllerPrefabComponentType = "game.horo.ai_controller";
        using Json = nlohmann::json;

        // These identities are derived-only compatibility values for the shape-only
        // authoring component. They are never persisted and are not project defaults.
        // The project collision/material schema owns the eventual replacement IDs.
        constexpr Physics::CollisionProfileId LegacyTriggerVolumeCollisionProfile = Physics::CollisionProfileId::FromBytes(
            {0x48, 0x6f, 0x72, 0x6f, 0x50, 0x68, 0x79, 0x73, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02});
        constexpr Assets::AssetId LegacyTriggerVolumeMaterial =
            Assets::AssetId::FromBytes({0x48, 0x6f, 0x72, 0x6f, 0x50, 0x68, 0x79, 0x73, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04});
        constexpr Physics::PhysicsMaterialSlotId LegacyTriggerVolumeMaterialSlot = Physics::PhysicsMaterialSlotId::FromValue(1);
        constexpr Runtime::PhysicsComponentId LegacyTriggerVolumeBodyComponent = {1};
        constexpr Runtime::PhysicsBodySlotId LegacyTriggerVolumeBodySlot = {1};
        constexpr Runtime::PhysicsComponentId LegacyTriggerVolumeColliderComponent = {2};
        constexpr Runtime::PhysicsColliderSlotId LegacyTriggerVolumeColliderSlot = {1};

        template <typename Component> [[nodiscard]] std::optional<Component> ActiveComponent(const std::optional<Component> &component) {
            return component.has_value() && component->enabled ? component : std::nullopt;
        }

        template <typename Component> [[nodiscard]] std::vector<Component> ActiveComponents(const std::vector<Component> &components) {
            std::vector<Component> active;
            active.reserve(components.size());
            std::ranges::copy_if(components, std::back_inserter(active), &Component::enabled);
            return active;
        }

        [[nodiscard]] std::string PayloadBytes(const std::span<const std::byte> payload) {
            std::string bytes;
            bytes.reserve(payload.size());
            for (const std::byte byte : payload)
                bytes.push_back(static_cast<char>(byte));
            return bytes;
        }

        /**
         * @brief Converts one enabled legacy trigger into canonical inert Physics producers.
         * @param object Immutable authored object whose component set is being projected.
         * @param components Runtime component set receiving the normalized
         *                    producers.
         * @return Success or a stable migration diagnostic; no authored state is mutated.
         */
        [[nodiscard]] Result<void> MigrateTriggerVolume(const SceneObjectSnapshot &object, Runtime::RuntimeComponentSet &components) {
            if (!object.components.triggerVolume)
                return Result<void>::Success();

            Runtime::PhysicsColliderSource source;
            using enum Runtime::ColliderShapeType;
            switch (object.components.triggerVolume->shape) {
                case Box:
                    source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsBoxCollider{}};
                    break;
                case Sphere:
                    source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{}};
                    break;
                case Capsule:
                    source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsCapsuleCollider{}};
                    break;
                case StaticPlane:
                    source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsStaticPlaneCollider{}};
                    break;
                default:
                    return Result<void>::Failure(
                        MakeError(TriggerVolumeSchemaUnsupported,
                                  std::format("Scene object {} contains an unsupported legacy trigger shape value.", object.id.value)));
            }
            if (!object.components.triggerVolume->enabled)
                return Result<void>::Success();

            if (object.components.rigidBody || !object.components.colliders.empty() || !object.components.physicsConstraints.empty())
                return Result<void>::Failure(
                    MakeError(TriggerVolumeCanonicalConflict,
                              std::format("Scene object {} contains both a legacy trigger volume and canonical Physics producers.",
                                          object.id.value)));

            components.rigidBody = Runtime::RigidBodyComponent{.id = LegacyTriggerVolumeBodyComponent,
                                                               .body = LegacyTriggerVolumeBodySlot,
                                                               .motion = Runtime::AuthoredPhysicsMotionType::Static,
                                                               .mass = Runtime::AuthoredPhysicsNoMass{}};
            components.colliders = {Runtime::ColliderComponent{
                .id = LegacyTriggerVolumeColliderComponent,
                .collider = LegacyTriggerVolumeColliderSlot,
                .body = {.object = Runtime::SceneObjectId{object.id.value}, .body = LegacyTriggerVolumeBodySlot},
                .source = std::move(source),
                .localPose = {.translation = Math::Vec3{}, .rotation = Math::Quaternion::Identity()},
                .scale = {1.0F, 1.0F, 1.0F},
                .collisionProfile = LegacyTriggerVolumeCollisionProfile,
                .materials = {{.slot = LegacyTriggerVolumeMaterialSlot, .material = LegacyTriggerVolumeMaterial}},
                .sensor = true,
            }};
            return Result<void>::Success();
        }

        /** @brief Preserves instance identity while adding scene-conversion context to a resolver failure. */
        void AddInstanceContext(Error &error, const ScenePrefabInstanceProjection &projection) {
            error.message = std::format("Required prefab instance {} ({}) failed: {}", projection.authored.instanceId.Value(),
                                        projection.authored.sourcePrefab.Asset().ToString(), error.message);
        }

        [[nodiscard]] Result<Math::Transform> ApplyInstanceTransform(const ScenePrefabInstance &instance,
                                                                     const Prefab::ResolvedPrefabObject &object) {
            if (!object.key.object.NestedInstanceScope().empty() || !object.key.object.SourceObject().IsRoot())
                return Result<Math::Transform>::Success(object.effectiveLocalTransform);
            const auto composed =
                Math::TryDecomposeAffineTRS(Math::Multiply(instance.rootTransform.ToMatrix(), object.effectiveLocalTransform.ToMatrix()));
            if (composed.HasError())
                return Result<Math::Transform>::Failure(composed.ErrorValue());
            return composed;
        }

        [[nodiscard]] Result<Runtime::NavigationAgentComponent> ParsePrefabNavigationAgent(const Prefab::RawComponentPayload &payload) {
            if (payload.component.typeId.Value() != NavigationAgentPrefabComponentType ||
                payload.component.encoding != Gameplay::ComponentPayloadEncoding::CanonicalJson || payload.component.schemaVersion != 1)
                return Result<Runtime::NavigationAgentComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));

            try {
                const Json value = Json::parse(PayloadBytes(payload.component.payload));
                auto parsed = Detail::ParseNavigationAgentJson(value);
                if (parsed.HasError())
                    return Result<Runtime::NavigationAgentComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                return Result<Runtime::NavigationAgentComponent>::Success(std::move(parsed).Value());
            } catch (const nlohmann::json::exception &) {
                return Result<Runtime::NavigationAgentComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
            }
        }

        [[nodiscard]] Result<Json> ParsePrefabCanonicalJson(const Prefab::RawComponentPayload &payload,
                                                            const std::string_view expectedType) {
            if (payload.component.typeId.Value() != expectedType ||
                payload.component.encoding != Gameplay::ComponentPayloadEncoding::CanonicalJson || payload.component.schemaVersion != 1)
                return Result<Json>::Failure(MakeError(PrefabComponentProjectionUnsupported));
            try {
                return Result<Json>::Success(Json::parse(PayloadBytes(payload.component.payload)));
            } catch (const nlohmann::json::exception &) {
                return Result<Json>::Failure(MakeError(PrefabComponentProjectionUnsupported));
            }
        }

        [[nodiscard]] Result<AI::AiStartupPolicy> ParsePrefabAiStartupPolicy(const Json &value) {
            using enum AI::AiStartupPolicy;
            if (!value.is_string())
                return Result<AI::AiStartupPolicy>::Failure(MakeError(PrefabComponentProjectionUnsupported));
            const std::string policy = value.get<std::string>();
            if (policy == "scene_activation")
                return Result<AI::AiStartupPolicy>::Success(OnSceneActivation);
            if (policy == "on_enable")
                return Result<AI::AiStartupPolicy>::Success(OnEnable);
            if (policy == "manual")
                return Result<AI::AiStartupPolicy>::Success(Manual);
            return Result<AI::AiStartupPolicy>::Failure(MakeError(PrefabComponentProjectionUnsupported));
        }

        [[nodiscard]] Result<AI::DecisionPlanKind> ParsePrefabAiDecisionKind(const Json &value) {
            using enum AI::DecisionPlanKind;
            if (!value.is_string())
                return Result<AI::DecisionPlanKind>::Failure(MakeError(PrefabComponentProjectionUnsupported));
            const std::string kind = value.get<std::string>();
            if (kind == "behavior_tree")
                return Result<AI::DecisionPlanKind>::Success(BehaviorTree);
            if (kind == "state_machine")
                return Result<AI::DecisionPlanKind>::Success(StateMachine);
            if (kind == "utility")
                return Result<AI::DecisionPlanKind>::Success(Utility);
            return Result<AI::DecisionPlanKind>::Failure(MakeError(PrefabComponentProjectionUnsupported));
        }

        [[nodiscard]] Result<AI::AiAgentComponent> ParsePrefabAiAgent(const Prefab::RawComponentPayload &payload) {
            auto value = ParsePrefabCanonicalJson(payload, AiAgentPrefabComponentType);
            if (value.HasError())
                return Result<AI::AiAgentComponent>::Failure(value.ErrorValue());
            try {
                const Json &json = value.Value();
                if (!json.is_object() || !json.contains("agent") || !json["agent"].is_number_unsigned() ||
                    !json.contains("schemaVersion") || !json["schemaVersion"].is_number_unsigned() || !json.contains("startupPolicy") ||
                    !json.contains("enabled") || !json["enabled"].is_boolean() ||
                    json["schemaVersion"].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
                    return Result<AI::AiAgentComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                const auto agent = AI::AgentId::Create(json["agent"].get<std::uint64_t>());
                const auto policy = ParsePrefabAiStartupPolicy(json["startupPolicy"]);
                if (agent.HasError() || policy.HasError())
                    return Result<AI::AiAgentComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                AI::AiAgentComponent component{.agent = agent.Value(),
                                               .schemaVersion = json["schemaVersion"].get<std::uint32_t>(),
                                               .startupPolicy = policy.Value(),
                                               .enabled = json["enabled"].get<bool>()};
                if (AI::ValidateAiAgentComponent(component).HasError())
                    return Result<AI::AiAgentComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                return Result<AI::AiAgentComponent>::Success(std::move(component));
            } catch (const nlohmann::json::exception &) {
                return Result<AI::AiAgentComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
            }
        }

        [[nodiscard]] Result<AI::AiControllerComponent> ParsePrefabAiController(const Prefab::RawComponentPayload &payload) {
            auto value = ParsePrefabCanonicalJson(payload, AiControllerPrefabComponentType);
            if (value.HasError())
                return Result<AI::AiControllerComponent>::Failure(value.ErrorValue());
            try {
                const Json &json = value.Value();
                if (!json.is_object() || !json.contains("controller") || !json["controller"].is_number_unsigned() ||
                    !json.contains("decisionAsset") || !json["decisionAsset"].is_number_unsigned() || !json.contains("blackboardSchema") ||
                    !json["blackboardSchema"].is_number_unsigned() || !json.contains("decisionKind") ||
                    !json.contains("requiredCapabilities") || !json["requiredCapabilities"].is_number_unsigned() ||
                    !json.contains("schemaVersion") || !json["schemaVersion"].is_number_unsigned() || !json.contains("startupPolicy") ||
                    !json.contains("enabled") || !json["enabled"].is_boolean() ||
                    json["requiredCapabilities"].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max() ||
                    json["schemaVersion"].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
                    return Result<AI::AiControllerComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                const auto controller = AI::ControllerTypeId::Create(json["controller"].get<std::uint64_t>());
                const auto decision = AI::DecisionGraphAssetId::Create(json["decisionAsset"].get<std::uint64_t>());
                const auto schema = AI::BlackboardSchemaId::Create(json["blackboardSchema"].get<std::uint64_t>());
                const auto kind = ParsePrefabAiDecisionKind(json["decisionKind"]);
                const auto policy = ParsePrefabAiStartupPolicy(json["startupPolicy"]);
                if (controller.HasError() || decision.HasError() || schema.HasError() || kind.HasError() || policy.HasError())
                    return Result<AI::AiControllerComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                AI::AiControllerComponent component{.controller = controller.Value(),
                                                    .decisionAsset = decision.Value(),
                                                    .blackboardSchema = schema.Value(),
                                                    .decisionKind = kind.Value(),
                                                    .requiredCapabilities =
                                                        AI::AiCapabilitySet{json["requiredCapabilities"].get<std::uint32_t>()},
                                                    .schemaVersion = json["schemaVersion"].get<std::uint32_t>(),
                                                    .startupPolicy = policy.Value(),
                                                    .enabled = json["enabled"].get<bool>()};
                if (AI::ValidateAiControllerComponent(component).HasError())
                    return Result<AI::AiControllerComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                return Result<AI::AiControllerComponent>::Success(std::move(component));
            } catch (const nlohmann::json::exception &) {
                return Result<AI::AiControllerComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
            }
        }

        [[nodiscard]] Result<void> ProjectPrefabComponent(const Prefab::RawComponentPayload &payload,
                                                          const Prefab::PrefabSceneObjectId sceneObject,
                                                          Runtime::RuntimeComponentSet &components) {
            if (payload.component.typeId.Value() == NavigationAgentPrefabComponentType) {
                if (components.navigationAgent.has_value())
                    return Result<void>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                auto parsed = ParsePrefabNavigationAgent(payload);
                if (parsed.HasError())
                    return Result<void>::Failure(parsed.ErrorValue());
                auto component = std::move(parsed).Value();
                if (component.enabled)
                    components.navigationAgent = std::move(component);
                return Result<void>::Success();
            }
            if (payload.component.typeId.Value() == AiAgentPrefabComponentType) {
                if (components.aiAgent.has_value())
                    return Result<void>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                auto parsed = ParsePrefabAiAgent(payload);
                if (parsed.HasError())
                    return Result<void>::Failure(parsed.ErrorValue());
                auto component = std::move(parsed).Value();
                if (component.enabled) {
                    const auto remapped = AI::AgentId::Create(sceneObject.value);
                    if (remapped.HasError())
                        return Result<void>::Failure(remapped.ErrorValue());
                    component.agent = remapped.Value();
                    components.aiAgent = std::move(component);
                }
                return Result<void>::Success();
            }
            if (payload.component.typeId.Value() == AiControllerPrefabComponentType) {
                if (components.aiController.has_value())
                    return Result<void>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                auto parsed = ParsePrefabAiController(payload);
                if (parsed.HasError())
                    return Result<void>::Failure(parsed.ErrorValue());
                auto component = std::move(parsed).Value();
                if (component.enabled)
                    components.aiController = std::move(component);
                return Result<void>::Success();
            }
            return Result<void>::Failure(MakeError(PrefabComponentProjectionUnsupported));
        }

        /** @brief Projects one resolved prefab object's supported typed components into the runtime component set. */
        [[nodiscard]] Result<Runtime::RuntimeComponentSet> ProjectPrefabComponents(const Prefab::ResolvedPrefabObject &object,
                                                                                   const Prefab::PrefabSceneObjectId sceneObject) {
            Runtime::RuntimeComponentSet components{.behaviors = object.object.behaviors};
            for (const Prefab::RawComponentPayload &payload : object.object.components) {
                if (const Result<void> projected = ProjectPrefabComponent(payload, sceneObject, components); projected.HasError())
                    return Result<Runtime::RuntimeComponentSet>::Failure(projected.ErrorValue());
            }
            return Result<Runtime::RuntimeComponentSet>::Success(std::move(components));
        }

        [[nodiscard]] Result<void> AddAuthoredObjects(const SceneDocumentSnapshot &document, Runtime::SceneDefinitionBuilder &builder,
                                                      std::vector<Prefab::PrefabSceneObjectId> &occupied) {
            occupied.reserve(document.objects.size());
            for (const SceneObjectSnapshot &object : document.objects) {
                occupied.emplace_back(object.id.value);
                Runtime::RuntimeComponentSet components{
                    .camera = ActiveComponent(object.components.camera),
                    .light = ActiveComponent(object.components.light),
                    .audioSource = ActiveComponent(object.components.audioSource),
                    .navigationSurface = ActiveComponent(object.components.navigationSurface),
                    .navigationRegion = ActiveComponent(object.components.navigationRegion),
                    .navigationModifier = ActiveComponent(object.components.navigationModifier),
                    .navigationLink = ActiveComponent(object.components.navigationLink),
                    .navigationAgent = ActiveComponent(object.components.navigationAgent),
                    .aiAgent = ActiveComponent(object.components.aiAgent),
                    .aiController = ActiveComponent(object.components.aiController),
                    .rigidBody = ActiveComponent(object.components.rigidBody),
                    .colliders = ActiveComponents(object.components.colliders),
                    .physicsConstraints = ActiveComponents(object.components.physicsConstraints),
                    .behaviors = object.components.behaviors,
                    .gameplayComponents = object.components.gameplayComponents,
                };
                if (const Result<void> migrated = MigrateTriggerVolume(object, components); migrated.HasError())
                    return migrated;
                builder.Add(Runtime::RuntimeEntityDefinition{
                    .object = Runtime::SceneObjectId{object.id.value},
                    .parent = object.parent ? std::optional{Runtime::SceneObjectId{object.parent->value}} : std::nullopt,
                    .localTransform = object.localTransform,
                    .primitiveMesh = object.primitiveMesh,
                    .components = components,
                });
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AddPrefabCandidate(const ScenePrefabInstance &instance,
                                                      const Prefab::EffectivePrefabCandidate &candidate,
                                                      const Prefab::PrefabSceneIdentityMap &identityMap,
                                                      Runtime::SceneDefinitionBuilder &builder) {
            for (const Prefab::ResolvedPrefabObject &object : candidate.Objects()) {
                const std::optional<Prefab::PrefabSceneObjectId> sceneId = identityMap.Find(object.key);
                if (!sceneId)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::IdentityCollision));
                auto components = ProjectPrefabComponents(object, *sceneId);
                if (components.HasError())
                    return Result<void>::Failure(components.ErrorValue());

                std::optional<Runtime::SceneObjectId> parent;
                if (object.parent) {
                    const auto parentId = identityMap.Find(*object.parent);
                    if (!parentId)
                        return Result<void>::Failure(MakeError(Prefab::PrefabErrors::HierarchyInvalid));
                    parent = Runtime::SceneObjectId{parentId->value};
                } else if (object.key.object.NestedInstanceScope().empty() && instance.parent)
                    parent = Runtime::SceneObjectId{instance.parent->value};

                const Result<Math::Transform> transform = ApplyInstanceTransform(instance, object);
                if (transform.HasError())
                    return Result<void>::Failure(transform.ErrorValue());
                builder.Add(Runtime::RuntimeEntityDefinition{
                    .object = Runtime::SceneObjectId{sceneId->value},
                    .parent = parent,
                    .localTransform = transform.Value(),
                    .primitiveMesh = std::nullopt,
                    .components = std::move(components).Value(),
                });
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ScenePrefabProjection::HasBrokenInstances */
    bool ScenePrefabProjection::HasBrokenInstances() const noexcept {
        return std::ranges::any_of(instances, &ScenePrefabInstanceProjection::IsBroken);
    }

    /** @copydoc BuildScenePrefabProjection */
    Result<ScenePrefabProjection> BuildScenePrefabProjection(const SceneDocumentSnapshot &document,
                                                             const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                             const Prefab::PrefabLimitProfile &limits) {
        ScenePrefabProjection projection;
        projection.instances.reserve(document.prefabInstances.size());
        for (const ScenePrefabInstance &instance : document.prefabInstances) {
            ScenePrefabInstanceProjection entry{.authored = instance};
            if (auto candidate = resolver.Resolve(instance.sourcePrefab.Asset(), instance.instanceId, limits); candidate.HasError()) {
                Error error = std::move(candidate).ErrorValue();
                AddInstanceContext(error, entry);
                entry.failure = std::move(error);
            } else
                entry.expanded = std::move(candidate).Value();
            projection.instances.push_back(std::move(entry));
        }
        return Result<ScenePrefabProjection>::Success(std::move(projection));
    }

    /** @copydoc ConvertSceneDocumentToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertSceneDocumentToRuntime(const SceneDocumentSnapshot &document,
                                                                          const Runtime::SceneDefinitionId sceneId) {
        if (!document.prefabInstances.empty())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(PrefabResolutionRequired));
        Runtime::SceneDefinitionBuilder builder{sceneId, Runtime::SceneDefinitionRevision{document.state.value}};
        std::vector<Prefab::PrefabSceneObjectId> occupied;
        if (const auto authored = AddAuthoredObjects(document, builder, occupied); authored.HasError())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(authored.ErrorValue());
        return std::move(builder).Build();
    }

    /** @copydoc ConvertSceneDocumentToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertSceneDocumentToRuntime(const SceneDocumentSnapshot &document,
                                                                          const Runtime::SceneDefinitionId sceneId,
                                                                          const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                                          const Prefab::PrefabLimitProfile &limits) {
        Runtime::SceneDefinitionBuilder builder{sceneId, Runtime::SceneDefinitionRevision{document.state.value}};
        std::vector<Prefab::PrefabSceneObjectId> occupied;
        if (const auto authored = AddAuthoredObjects(document, builder, occupied); authored.HasError())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(authored.ErrorValue());

        for (const ScenePrefabInstance &instance : document.prefabInstances) {
            if (auto candidate = resolver.Resolve(instance.sourcePrefab.Asset(), instance.instanceId, limits); candidate.HasError()) {
                Error error = std::move(candidate).ErrorValue();
                ScenePrefabInstanceProjection context{.authored = instance};
                AddInstanceContext(error, context);
                return Result<Runtime::RuntimeSceneDefinition>::Failure(std::move(error));
            } else {
                Prefab::EffectivePrefabCandidate resolved = std::move(candidate).Value();
                auto remapped = Prefab::RemapPrefabCandidateToScene(resolved, occupied, {}, limits);
                if (remapped.HasError()) {
                    Error error = std::move(remapped).ErrorValue();
                    ScenePrefabInstanceProjection context{.authored = instance};
                    AddInstanceContext(error, context);
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(std::move(error));
                }
                Prefab::PrefabSceneIdentityMap identityMap = std::move(remapped).Value();
                for (const Prefab::PrefabSceneIdentityMapping &mapping : identityMap.Mappings())
                    occupied.push_back(mapping.scene);
                if (const auto added = AddPrefabCandidate(instance, resolved, identityMap, builder); added.HasError()) {
                    Error error = added.ErrorValue();
                    ScenePrefabInstanceProjection context{.authored = instance};
                    AddInstanceContext(error, context);
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(std::move(error));
                }
            }
        }
        return std::move(builder).Build();
    }
}  // namespace Horo::Editor
