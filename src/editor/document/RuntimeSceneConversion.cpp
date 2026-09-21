#include "editor/document/RuntimeSceneConversion.h"

#include "Horo/Gameplay/Component.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSceneIdentityRemap.h"
#include "editor/document/NavigationAgentJson.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <nlohmann/json.hpp>
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
        constexpr std::string_view NavigationAgentPrefabComponentType = "game.horo.navigation_agent";
        using Json = nlohmann::json;

        template <typename Component> [[nodiscard]] std::optional<Component> ActiveComponent(const std::optional<Component> &component) {
            return component.has_value() && component->enabled ? component : std::nullopt;
        }

        template <typename Component> [[nodiscard]] std::vector<Component> ActiveComponents(const std::vector<Component> &components) {
            std::vector<Component> active;
            active.reserve(components.size());
            std::ranges::copy_if(components, std::back_inserter(active), &Component::enabled);
            return active;
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
                std::string bytes;
                bytes.reserve(payload.component.payload.size());
                for (const std::byte byte : payload.component.payload)
                    bytes.push_back(static_cast<char>(byte));
                const Json value = Json::parse(bytes);
                auto parsed = Detail::ParseNavigationAgentJson(value);
                if (parsed.HasError())
                    return Result<Runtime::NavigationAgentComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                return Result<Runtime::NavigationAgentComponent>::Success(std::move(parsed).Value());
            } catch (const nlohmann::json::exception &) {
                return Result<Runtime::NavigationAgentComponent>::Failure(MakeError(PrefabComponentProjectionUnsupported));
            }
        }

        /** @brief Projects one resolved prefab object's supported typed components into the runtime component set. */
        [[nodiscard]] Result<Runtime::RuntimeComponentSet> ProjectPrefabComponents(const Prefab::ResolvedPrefabObject &object) {
            Runtime::RuntimeComponentSet components{.behaviors = object.object.behaviors};
            for (const Prefab::RawComponentPayload &payload : object.object.components) {
                if (payload.component.typeId.Value() != NavigationAgentPrefabComponentType)
                    return Result<Runtime::RuntimeComponentSet>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                if (components.navigationAgent.has_value())
                    return Result<Runtime::RuntimeComponentSet>::Failure(MakeError(PrefabComponentProjectionUnsupported));
                auto parsed = ParsePrefabNavigationAgent(payload);
                if (parsed.HasError())
                    return Result<Runtime::RuntimeComponentSet>::Failure(parsed.ErrorValue());
                if (parsed.Value().enabled)
                    components.navigationAgent = std::move(parsed).Value();
            }
            return Result<Runtime::RuntimeComponentSet>::Success(std::move(components));
        }

        [[nodiscard]] Result<void> AddAuthoredObjects(const SceneDocumentSnapshot &document, Runtime::SceneDefinitionBuilder &builder,
                                                      std::vector<Prefab::PrefabSceneObjectId> &occupied) {
            occupied.reserve(document.objects.size());
            for (const SceneObjectSnapshot &object : document.objects) {
                occupied.emplace_back(object.id.value);
                const Runtime::RuntimeComponentSet components{
                    .camera = ActiveComponent(object.components.camera),
                    .light = ActiveComponent(object.components.light),
                    .triggerVolume = ActiveComponent(object.components.triggerVolume),
                    .audioSource = ActiveComponent(object.components.audioSource),
                    .navigationSurface = ActiveComponent(object.components.navigationSurface),
                    .navigationRegion = ActiveComponent(object.components.navigationRegion),
                    .navigationModifier = ActiveComponent(object.components.navigationModifier),
                    .navigationLink = ActiveComponent(object.components.navigationLink),
                    .navigationAgent = ActiveComponent(object.components.navigationAgent),
                    .rigidBody = ActiveComponent(object.components.rigidBody),
                    .colliders = ActiveComponents(object.components.colliders),
                    .physicsConstraints = ActiveComponents(object.components.physicsConstraints),
                    .behaviors = object.components.behaviors,
                    .gameplayComponents = object.components.gameplayComponents,
                };
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
                auto components = ProjectPrefabComponents(object);
                if (components.HasError())
                    return Result<void>::Failure(components.ErrorValue());
                const std::optional<Prefab::PrefabSceneObjectId> sceneId = identityMap.Find(object.key);
                if (!sceneId)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::IdentityCollision));

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
