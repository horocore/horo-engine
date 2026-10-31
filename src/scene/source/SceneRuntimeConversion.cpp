#include "Horo/Scene/SceneRuntimeConversion.h"

#include "Horo/Gameplay/Component.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSceneExpansion.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "NavigationAgentJson.h"
#include "ScenePrefabProjectionContext.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>

namespace Horo::SceneSource {
    using Detail::AddInstanceContext;

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
        const ErrorCodeDescriptor
            TriggerVolumeProjectBindingMissing{.domain = ErrorDomainId{"horo.scene.conversion"},
                                               .code = ErrorCode{"scene_conversion.trigger_volume_project_binding_missing"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary =
                                                   "An enabled legacy trigger requires captured project collision and material bindings.",
                                               .remediationHint = "Select a project sensor profile and cooked Physics material, then "
                                                                  "recapture the scene conversion."};

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

        /** @brief Validates legacy geometry before disabled/conflict/binding precedence is evaluated. */
        Result<Runtime::PhysicsColliderSource> TriggerGeometry(const SceneObjectSnapshot &object) {
            using enum Runtime::ColliderShapeType;
            switch (object.components.triggerVolume->shape) {
                case Box:
                    return Result<Runtime::PhysicsColliderSource>::Success(Runtime::PhysicsAnalyticCollider{Runtime::PhysicsBoxCollider{}});
                case Sphere:
                    return Result<Runtime::PhysicsColliderSource>::Success(
                        Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{}});
                case Capsule:
                    return Result<Runtime::PhysicsColliderSource>::Success(
                        Runtime::PhysicsAnalyticCollider{Runtime::PhysicsCapsuleCollider{}});
                case StaticPlane:
                    return Result<Runtime::PhysicsColliderSource>::Success(
                        Runtime::PhysicsAnalyticCollider{Runtime::PhysicsStaticPlaneCollider{}});
                default:
                    return Result<Runtime::PhysicsColliderSource>::Failure(
                        MakeError(TriggerVolumeSchemaUnsupported,
                                  std::format("Scene object {} contains an unsupported legacy trigger shape value.", object.id.value)));
            }
        }

        /**
         * @brief Converts one enabled legacy trigger into canonical inert Physics producers.
         * @param object Immutable authored object whose component set is being projected.
         * @param components Runtime component set receiving the normalized
         *                    producers.
         * @return Success or a stable migration diagnostic; no authored state is mutated.
         */
        [[nodiscard]] Result<void> MigrateTriggerVolume(const SceneObjectSnapshot &object, Runtime::RuntimeComponentSet &components,
                                                        Runtime::SceneDefinitionBuilder &builder,
                                                        const std::optional<TriggerVolumeConversionContext> &triggerBinding) {
            if (!object.components.triggerVolume)
                return Result<void>::Success();

            auto source = TriggerGeometry(object);
            if (source.HasError())
                return Result<void>::Failure(source.ErrorValue());
            if (!object.components.triggerVolume->enabled)
                return Result<void>::Success();

            if (object.components.rigidBody || !object.components.colliders.empty() || !object.components.physicsConstraints.empty())
                return Result<void>::Failure(
                    MakeError(TriggerVolumeCanonicalConflict,
                              std::format("Scene object {} contains both a legacy trigger volume and canonical Physics producers.",
                                          object.id.value)));

            if (!triggerBinding || triggerBinding->collisionFilterGeneration == 0)
                return Result<void>::Failure(
                    MakeError(TriggerVolumeProjectBindingMissing,
                              std::format("Scene object {} requires an explicit project trigger binding.", object.id.value)));
            const auto &binding = triggerBinding->binding;
            if (auto admitted = builder.RequireTriggerVolume(
                    {Runtime::SceneObjectId{object.id.value}, LegacyTriggerVolumeBodySlot, LegacyTriggerVolumeColliderSlot, binding});
                admitted.HasError())
                return admitted;

            components.rigidBody = Runtime::RigidBodyComponent{.id = LegacyTriggerVolumeBodyComponent,
                                                               .body = LegacyTriggerVolumeBodySlot,
                                                               .motion = Runtime::AuthoredPhysicsMotionType::Static,
                                                               .mass = Runtime::AuthoredPhysicsNoMass{}};
            components.colliders = {Runtime::ColliderComponent{
                .id = LegacyTriggerVolumeColliderComponent,
                .collider = LegacyTriggerVolumeColliderSlot,
                .body = {.object = Runtime::SceneObjectId{object.id.value}, .body = LegacyTriggerVolumeBodySlot},
                .source = std::move(source).Value(),
                .localPose = {.translation = Math::Vec3{}, .rotation = Math::Quaternion::Identity()},
                .scale = {1.0F, 1.0F, 1.0F},
                .collisionProfile = binding.profile,
                .materials = {{.slot = LegacyTriggerVolumeMaterialSlot, .material = binding.material.id}},
                .sensor = true,
            }};
            return Result<void>::Success();
        }

        /** @brief Preserves typed failure and identifies its required instance at the runtime boundary. */
        [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> InstanceFailure(Error error,
                                                                              const ScenePrefabInstanceProjection &projection) {
            AddInstanceContext(error, projection);
            return Result<Runtime::RuntimeSceneDefinition>::Failure(std::move(error));
        }

        /** @brief Uses the same inert codec as cooked hosts, preserving legacy source identity/disabled admission policy. */
        [[nodiscard]] Result<void> ProjectPrefabComponent(const Prefab::RawComponentPayload &payload,
                                                          Prefab::PrefabRuntimeComponentProjection &projection) {
            using enum Runtime::BuiltinSceneComponentKind;
            auto projected = ProjectPrefabBuiltinComponent(payload, projection.components, projection.primitiveMesh);
            if (projected.HasError())
                return Result<void>::Failure(WrapError(PrefabComponentProjectionUnsupported, projected.ErrorValue()));
            if (!projected.Value())
                return Result<void>::Failure(MakeError(PrefabComponentProjectionUnsupported));
            const auto kind = Runtime::FindBuiltinSceneComponent(payload.component.typeId.Value());
            auto &components = projection.components;
            if (kind == NavigationAgent && !components.navigationAgent->enabled)
                components.navigationAgent.reset();
            if (kind == AiAgent && !components.aiAgent->enabled)
                components.aiAgent.reset();
            if (kind == AiController && !components.aiController->enabled)
                components.aiController.reset();
            return Result<void>::Success();
        }

        /** @brief Preserves complete typed schema output and original resolved-object ownership. */
        [[nodiscard]] Result<Prefab::PrefabRuntimeComponentProjection> ProjectPrefabComponents(const Prefab::ResolvedPrefabObject &object) {
            Prefab::PrefabRuntimeComponentProjection projection{.object = object.key, .components = {.behaviors = object.object.behaviors}};
            for (const Prefab::RawComponentPayload &payload : object.object.components) {
                if (auto projected = ProjectPrefabComponent(payload, projection); projected.HasError())
                    return Result<Prefab::PrefabRuntimeComponentProjection>::Failure(projected.ErrorValue());
            }
            return Result<Prefab::PrefabRuntimeComponentProjection>::Success(std::move(projection));
        }

        [[nodiscard]] Result<void> AddAuthoredObjects(const SceneSourceView &document, Runtime::SceneDefinitionBuilder &builder,
                                                      std::vector<Prefab::PrefabSceneObjectId> &occupied,
                                                      const std::optional<TriggerVolumeConversionContext> &triggerBinding) {
            occupied.reserve(document.objects.size());
            for (const SceneObjectSnapshot &object : document.objects) {
                occupied.emplace_back(object.id.value);
                Runtime::RuntimeComponentSet components{
                    .camera = ActiveComponent(object.components.camera),
                    .light = ActiveComponent(object.components.light),
                    .audioSource = ActiveComponent(object.components.audioSource),
                    .audioListener = ActiveComponent(object.components.audioListener),
                    .navigationSurface = ActiveComponent(object.components.navigationSurface),
                    .navigationRegion = ActiveComponent(object.components.navigationRegion),
                    .navigationModifier = ActiveComponent(object.components.navigationModifier),
                    .navigationLink = ActiveComponent(object.components.navigationLink),
                    .navigationAgent = ActiveComponent(object.components.navigationAgent),
                    .aiAgent = ActiveComponent(object.components.aiAgent),
                    .aiController = ActiveComponent(object.components.aiController),
                    .characterController = object.components.characterController,
                    .rigidBody = ActiveComponent(object.components.rigidBody),
                    .colliders = ActiveComponents(object.components.colliders),
                    .physicsConstraints = ActiveComponents(object.components.physicsConstraints),
                    .behaviors = object.components.behaviors,
                    .gameplayComponents = object.components.gameplayComponents,
                };
                if (const Result<void> migrated = MigrateTriggerVolume(object, components, builder, triggerBinding); migrated.HasError())
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

        /** @brief Owns the scene builder and its occupied identity set throughout authored and optional prefab admission. */
        struct SceneAssembly final {
            Runtime::SceneDefinitionBuilder builder;
            std::vector<Prefab::PrefabSceneObjectId> occupied;
        };

        /** @brief Admits authored objects once for both public conversion paths, keeping collision state beside its builder. */
        Result<SceneAssembly> BeginSceneAssembly(const SceneSourceView &document, const Runtime::SceneDefinitionId sceneId,
                                                 const Runtime::SceneDefinitionRevision revision,
                                                 const std::optional<TriggerVolumeConversionContext> &triggerBinding) {
            SceneAssembly assembly{Runtime::SceneDefinitionBuilder{sceneId, revision}, {}};
            if (auto authored = AddAuthoredObjects(document, assembly.builder, assembly.occupied, triggerBinding); authored.HasError())
                return Result<SceneAssembly>::Failure(authored.ErrorValue());
            return Result<SceneAssembly>::Success(std::move(assembly));
        }

        /** @brief Rewrites only typed local body endpoints through the canonical source occurrence identity map. */
        Result<void> RemapPrefabBodyReferences(const Prefab::ExpandedPrefabObjectKey &owner, Runtime::RuntimeComponentSet &components,
                                               const Prefab::PrefabSceneIdentityMap &identityMap) {
            const auto remap = [&](Runtime::PhysicsBodyReference &reference) {
                if (reference.object.value > std::numeric_limits<std::uint32_t>::max())
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ReferenceRewriteInvalid,
                                                           "Prefab Physics endpoint exceeds the source local identity range."));
                const auto local = Prefab::LocalObjectId{static_cast<std::uint32_t>(reference.object.value)};
                auto address = Prefab::PrefabObjectAddress::Create(owner.object.NestedInstanceScope(), local);
                if (address.HasError())
                    return Result<void>::Failure(address.ErrorValue());
                const auto target = identityMap.Find({owner.instance, std::move(address).Value()});
                if (!target)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::IdentityCollision,
                                                           "Prefab Physics endpoint is outside its exact source occurrence."));
                reference.object = Runtime::SceneObjectId{target->value};
                return Result<void>::Success();
            };
            for (auto &collider : components.colliders)
                if (auto valid = remap(collider.body); valid.HasError())
                    return valid;
            for (auto &constraint : components.physicsConstraints) {
                if (auto valid = remap(constraint.first.body); valid.HasError())
                    return valid;
                if (auto *second = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&constraint.second))
                    if (auto valid = remap(second->body); valid.HasError())
                        return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Rejects malformed source-occurrence identities before the established authored Scene-ID remap. */
        Result<void> RemapSourceAgentIdentities(const Prefab::EffectivePrefabCandidate &candidate,
                                                std::span<Prefab::PrefabRuntimeComponentProjection> projections,
                                                const Prefab::PrefabSceneIdentityMap &identityMap) {
            for (std::size_t index = 0; index < projections.size(); ++index) {
                const auto &agent = projections[index].components.aiAgent;
                if (!agent)
                    continue;
                const auto &source = candidate.Objects()[index];
                for (std::size_t previous = 0; previous < index; ++previous) {
                    const auto &other = projections[previous].components.aiAgent;
                    const auto &origin = candidate.Objects()[previous];
                    if (other && other->agent == agent->agent && source.sourcePrefab == origin.sourcePrefab &&
                        std::ranges::equal(source.key.object.NestedInstanceScope(), origin.key.object.NestedInstanceScope()))
                        return Result<void>::Failure(MakeError(Prefab::PrefabErrors::CorruptedPayload,
                                                               "One prefab source occurrence contains duplicate authored AI identities."));
                }
            }
            for (auto &projection : projections) {
                if (!projection.components.aiAgent)
                    continue;
                const auto scene = identityMap.Find(projection.object);
                if (!scene)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::IdentityCollision));
                auto identity = AI::AgentId::Create(scene->value);
                if (identity.HasError())
                    return Result<void>::Failure(WrapError(PrefabComponentProjectionUnsupported, identity.ErrorValue()));
                projection.components.aiAgent->agent = identity.Value();
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AddPrefabCandidate(const ScenePrefabInstance &instance,
                                                      const Prefab::EffectivePrefabCandidate &candidate,
                                                      const Prefab::PrefabSceneIdentityMap &identityMap,
                                                      Runtime::SceneDefinitionBuilder &builder, const Prefab::PrefabLimitProfile &limits) {
            std::vector<Prefab::PrefabRuntimeComponentProjection> projections;
            projections.reserve(candidate.Objects().size());
            for (const Prefab::ResolvedPrefabObject &object : candidate.Objects()) {
                if (!identityMap.Find(object.key))
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::IdentityCollision));
                auto components = ProjectPrefabComponents(object);
                if (components.HasError())
                    return Result<void>::Failure(components.ErrorValue());
                auto projected = std::move(components).Value();
                if (auto references = RemapPrefabBodyReferences(object.key, projected.components, identityMap); references.HasError())
                    return references;
                projections.push_back(std::move(projected));
            }
            if (auto identities = RemapSourceAgentIdentities(candidate, projections, identityMap); identities.HasError())
                return identities;
            auto expanded =
                Prefab::ExpandPrefabSceneSubtree(candidate, identityMap, projections,
                                                 Prefab::PrefabRuntimePlacement{.rootTransform = instance.rootTransform,
                                                                                .parent = instance.parent
                                                                                              ? std::optional{Runtime::SceneObjectId{
                                                                                                    instance.parent->value}}
                                                                                              : std::nullopt},
                                                 limits);
            if (expanded.HasError())
                return Result<void>::Failure(expanded.ErrorValue());
            for (const auto &entity : expanded.Value().Entities())
                builder.Add(entity);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ScenePrefabProjection::HasBrokenInstances */
    bool ScenePrefabProjection::HasBrokenInstances() const noexcept {
        return std::ranges::any_of(instances, &ScenePrefabInstanceProjection::IsBroken);
    }

    /** @copydoc BuildScenePrefabProjection */
    Result<ScenePrefabProjection> BuildScenePrefabProjection(const SceneSourceView &document,
                                                             const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                             const Prefab::PrefabLimitProfile &limits,
                                                             const Prefab::PrefabValueSchemaSnapshot *schemas) {
        ScenePrefabProjection projection;
        Detail::ScenePrefabDebugCaptureBudget debugBudget;
        projection.instances.reserve(document.prefabInstances.size());
        for (const ScenePrefabInstance &instance : document.prefabInstances) {
            ScenePrefabInstanceProjection entry{.authored = instance};
            if (auto candidate = resolver.Resolve(instance.sourcePrefab.Asset(), instance.instanceId, limits); candidate.HasError()) {
                Error error = std::move(candidate).ErrorValue();
                AddInstanceContext(error, entry);
                entry.failure = std::move(error);
            } else
                entry.expanded = std::move(candidate).Value();
            Detail::ApplyInstanceValues(entry, schemas, limits, debugBudget);
            projection.instances.push_back(std::move(entry));
        }
        return Result<ScenePrefabProjection>::Success(std::move(projection));
    }

    /** @copydoc InvalidateScenePrefabProjection */
    void InvalidateScenePrefabProjection(ScenePrefabProjection &projection, const Prefab::PrefabSourceResolverSnapshot &current,
                                         const std::span<const Assets::AssetId> changedAssets, const Prefab::PrefabLimitProfile &limits) {
        for (auto &entry : projection.instances) {
            if (!entry.expanded || entry.stale)
                continue;
            const auto inspected =
                current.ValidateRevisionPublication(entry.expanded->RootAsset(), entry.expanded->Revision(), changedAssets, limits);
            if (inspected.HasError()) {
                entry.stale = true;
                entry.failure = inspected.ErrorValue();
                AddInstanceContext(*entry.failure, entry);
            }
        }
    }

    /** @copydoc ConvertScenePrefabProjectionToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertScenePrefabProjectionToRuntime(
        const SceneSourceView &document, const Runtime::SceneDefinitionId sceneId, const Runtime::SceneDefinitionRevision revision,
        const ScenePrefabProjection &projection, const ScenePrefabResolutionContext &context,
        const std::optional<TriggerVolumeConversionContext> &triggerBinding) {
        const auto &[resolver, limits, schemas] = context;
        if (projection.instances.size() != document.prefabInstances.size())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(Prefab::PrefabErrors::ResolutionStale));
        auto prepared = BeginSceneAssembly(document, sceneId, revision, triggerBinding);
        if (prepared.HasError())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(prepared.ErrorValue());
        auto assembly = std::move(prepared).Value();
        auto &builder = assembly.builder;
        auto &occupied = assembly.occupied;
        for (std::size_t index = 0; index < projection.instances.size(); ++index) {
            const auto &entry = projection.instances[index];
            if (entry.authored != document.prefabInstances[index] || !entry.IsSynchronized())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(
                    entry.failure.value_or(MakeError(Prefab::PrefabErrors::ResolutionStale)));
            if (auto values = Detail::ValidateInstanceValues(entry, schemas, limits); values.HasError())
                return InstanceFailure(values.ErrorValue(), entry);
            const auto &candidate = *entry.expanded;
            if (auto valid = resolver.ValidateRevisionPublication(candidate.RootAsset(), candidate.Revision(), {}, limits);
                valid.HasError())
                return InstanceFailure(valid.ErrorValue(), entry);
            auto remapped = Prefab::RemapPrefabCandidateToScene(candidate, occupied, {}, limits);
            if (remapped.HasError())
                return InstanceFailure(remapped.ErrorValue(), entry);
            const auto &identityMap = remapped.Value();
            for (const auto &mapping : identityMap.Mappings())
                occupied.push_back(mapping.scene);
            if (const auto added = AddPrefabCandidate(entry.authored, candidate, identityMap, builder, limits); added.HasError())
                return InstanceFailure(added.ErrorValue(), entry);
        }
        return std::move(builder).Build();
    }

    /** @copydoc ConvertSceneSourceToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertSceneSourceToRuntime(
        const SceneSourceView &document, const Runtime::SceneDefinitionId sceneId, const Runtime::SceneDefinitionRevision revision,
        const std::optional<TriggerVolumeConversionContext> &triggerBinding) {
        if (!document.prefabInstances.empty())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(PrefabResolutionRequired));
        auto assembly = BeginSceneAssembly(document, sceneId, revision, triggerBinding);
        if (assembly.HasError())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(assembly.ErrorValue());
        auto owned = std::move(assembly).Value();
        return std::move(owned.builder).Build();
    }

    /** @copydoc ConvertSceneSourceToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertSceneSourceToRuntime(
        const SceneSourceView &document, const Runtime::SceneDefinitionId sceneId, const Runtime::SceneDefinitionRevision revision,
        const Prefab::PrefabSourceResolverSnapshot &resolver, const Prefab::PrefabLimitProfile &limits,
        const Prefab::PrefabValueSchemaSnapshot *schemas, const std::optional<TriggerVolumeConversionContext> &triggerBinding) {
        auto projection = BuildScenePrefabProjection(document, resolver, limits, schemas);
        if (projection.HasError())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(projection.ErrorValue());
        return ConvertScenePrefabProjectionToRuntime(document, sceneId, revision, projection.Value(), {resolver, limits, schemas},
                                                     triggerBinding);
    }

}  // namespace Horo::SceneSource
