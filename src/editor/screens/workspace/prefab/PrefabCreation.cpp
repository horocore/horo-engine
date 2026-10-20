#include "editor/screens/workspace/prefab/PrefabCreation.h"

#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Runtime/Scene/BuiltinSceneComponents.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "Horo/Scene/SceneSource.h"

#include <algorithm>
#include <map>
#include <new>
#include <nlohmann/json.hpp>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;
        using Kind = Runtime::BuiltinSceneComponentKind;
        using Id = SceneSource::SceneObjectId;

        /** @brief Reports an unsupported portable representation while retaining the owning authored site. */
        Error Unsupported() {
            return MakeError(Prefab::PrefabErrors::ReferenceRewriteInvalid,
                             "Selection contains unsupported or boundary-crossing references; inspect every source/target site.");
        }

        /** @brief Collects a complete bounded parent-first subtree from a canonical document snapshot. */
        Result<std::vector<const SceneSource::SceneObjectSnapshot *>> Subtree(const SceneTransactionSnapshot &snapshot, const Id root) {
            using Return = Result<std::vector<const SceneSource::SceneObjectSnapshot *>>;
            if (snapshot.objects.size() > 16'384)
                return Return::Failure(MakeError(Prefab::PrefabErrors::ObjectCountExceeded));
            const auto selected = std::ranges::find(snapshot.objects, root, &SceneSource::SceneObjectSnapshot::id);
            if (selected == snapshot.objects.end())
                return Return::Failure(MakeError(Prefab::PrefabErrors::HierarchyInvalid));
            std::vector<const SceneSource::SceneObjectSnapshot *> result{std::to_address(selected)};
            for (std::size_t cursor = 0; cursor < result.size(); ++cursor)
                for (const auto &object : snapshot.objects)
                    if (object.parent == result[cursor]->id) {
                        if (result.size() == snapshot.objects.size())
                            return Return::Failure(MakeError(Prefab::PrefabErrors::HierarchyInvalid));
                        result.push_back(&object);
                    }
            return Return::Success(std::move(result));
        }

        /** @brief Audits typed surface-owner references without interpreting catalog IDs as object identities. */
        void AuditNavigationReferences(const SceneSource::SceneObjectSnapshot &source, const std::map<Navigation::SurfaceId, Id> &surfaces,
                                       const std::vector<const SceneSource::SceneObjectSnapshot *> &selection,
                                       std::vector<PrefabCreationBoundary> &boundaries) {
            const auto selected = [&selection](const Id id) {
                return std::ranges::any_of(selection, [id](const auto *object) {
                    return object->id == id;
                });
            };
            const auto surface = [&](const Navigation::SurfaceId id, const char *property) {
                if (const auto target = surfaces.find(id); target != surfaces.end() && (selected(source.id) || selected(target->second)))
                    boundaries.push_back({source.id, target->second, property});
            };
            for (const auto &navigation : source.components.navigationSources)
                surface(navigation.surface, "navigationSources.surface");
            if (source.components.navigationRegion)
                surface(source.components.navigationRegion->surface, "navigationRegion.surface");
            if (source.components.navigationModifier)
                surface(source.components.navigationModifier->surface, "navigationModifier.surface");
            if (source.components.navigationLink) {
                surface(source.components.navigationLink->start.surface, "navigationLink.start.surface");
                surface(source.components.navigationLink->end.surface, "navigationLink.end.surface");
            }
        }

        /** @brief Rejects preserved data whose existing prefab representation cannot carry the complete authored contract. */
        void AuditSelectedObject(const SceneSource::SceneObjectSnapshot &object, std::vector<PrefabCreationBoundary> &boundaries) {
            if (object.generated || object.editorState.locked || !object.editorState.visible)
                boundaries.push_back({object.id, {}, "editor.protected_or_nonportable_state"});
            if (object.components.aiController)
                boundaries.push_back({object.id, {}, "aiController.unsupported_portable_asset_reference"});
            if (object.meshAsset)
                boundaries.push_back({object.id, {}, "meshAsset.unsupported_portable_schema"});
            if (!object.components.navigationSources.empty())
                boundaries.push_back({object.id, {}, "navigationSources.unsupported_portable_reference"});
            if (object.components.navigationRegion || object.components.navigationModifier || object.components.navigationLink)
                boundaries.push_back({object.id, {}, "navigation.unsupported_portable_reference"});
        }

        /** @brief Audits explicit body references on both sides of the replacement boundary, including disabled data. */
        void AuditReferences(const SceneTransactionSnapshot &snapshot,
                             const std::vector<const SceneSource::SceneObjectSnapshot *> &selection,
                             std::vector<PrefabCreationBoundary> &boundaries) {
            const auto selected = [&selection](const Id id) {
                return std::ranges::any_of(selection, [id](const auto *object) {
                    return object->id == id;
                });
            };
            std::map<Navigation::SurfaceId, Id> surfaces;
            for (const auto &object : snapshot.objects)
                if (object.components.navigationSurface)
                    surfaces.emplace(object.components.navigationSurface->id, object.id);
            for (const auto &object : snapshot.objects) {
                const bool inside = selected(object.id);
                AuditNavigationReferences(object, surfaces, selection, boundaries);
                const auto body = [&](const Runtime::PhysicsBodyReference &reference, std::string property) {
                    const Id target{reference.object.value};
                    if (inside != selected(target))
                        boundaries.push_back({object.id, target, std::move(property)});
                };
                for (const auto &collider : object.components.colliders)
                    body(collider.body, "colliders.body");
                for (const auto &constraint : object.components.physicsConstraints) {
                    body(constraint.first.body, "physicsConstraints.first.body");
                    if (const auto *endpoint = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&constraint.second))
                        body(endpoint->body, "physicsConstraints.second.body");
                }
                if (object.components.characterController && object.components.characterController->collision.excludedBody) {
                    const Id target{object.components.characterController->collision.excludedBody->object.value};
                    if (inside || selected(target))
                        boundaries.push_back({object.id, target, "characterController.collision.excludedBody"});
                }
                // Opaque bytes cannot prove either outgoing or incoming relationship safety.
                for (const auto &component : object.components.gameplayComponents)
                    boundaries.push_back({object.id, {}, component.typeId.Value()});
                if (!inside)
                    continue;
                AuditSelectedObject(object, boundaries);
            }
            for (const auto &placement : snapshot.prefabInstances)
                if (placement.parent && selected(*placement.parent))
                    boundaries.push_back({*placement.parent, {}, "prefabInstances.nested_boundary"});
        }

        /** @brief Rewrite only canonical typed body endpoints into the existing prefab-local identity domain. */
        void RewriteBodyReferences(Json &encoded, const std::vector<const SceneSource::SceneObjectSnapshot *> &selection) {
            const auto rewrite = [&selection](Json &body) {
                const Id id{body.at("object").get<std::uint64_t>()};
                const auto target = std::ranges::find_if(selection, [id](const auto *object) {
                    return object->id == id;
                });
                // Complete boundary admission precedes encoding; canonical captures contain every internal target.
                body["object"] = static_cast<std::uint32_t>(target - selection.begin());
            };
            auto &components = encoded.at("components");
            if (components.contains("colliders"))
                for (auto &collider : components.at("colliders"))
                    rewrite(collider.at("body"));
            if (components.contains("physicsConstraints"))
                for (auto &constraint : components.at("physicsConstraints")) {
                    rewrite(constraint.at("first").at("body"));
                    auto &second = constraint.at("second");
                    if (second.at("kind") == "body")
                        rewrite(second.at("body"));
                }
        }

        /** @brief Bound the complete incoming/outgoing reference audit before allocating per-site diagnostics. */
        bool WithinAuditBudget(const SceneTransactionSnapshot &snapshot) {
            std::size_t sites{};
            for (const auto &object : snapshot.objects) {
                sites += object.components.colliders.size() + object.components.physicsConstraints.size() * 2 +
                         object.components.gameplayComponents.size() + object.components.navigationSources.size() + 6;
                if (sites > 131'072)
                    return false;
            }
            return true;
        }

        /** @brief Collects already validated built-in occurrences without activating any runtime owner. */
        struct ComponentProjection final {
            Runtime::RuntimeComponentSet components;
            std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
        };

        /** @brief Encodes and verifies one complete occurrence through the canonical prefab built-in codec. */
        Result<void> AppendBuiltinOccurrence(const Kind kind, const Json &value, Prefab::PrefabObjectNode &node,
                                             std::uint64_t &nextInstance, ComponentProjection &projection) {
            const auto schemas = Runtime::BuiltinSceneComponentSchemas();
            const auto schema = std::ranges::find(schemas, kind, &Runtime::BuiltinSceneComponentSchema::kind);
            if (schema == schemas.end())
                return Result<void>::Failure(Unsupported());
            const auto text = value.dump();
            const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
            Prefab::RawComponentPayload payload{Prefab::PrefabComponentInstanceId::Create(nextInstance++).Value(),
                                                {Gameplay::ComponentTypeId::Parse(schema->typeId).Value(), 1,
                                                 Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                                 std::vector<std::byte>{bytes.begin(), bytes.end()}}};
            const auto projected = SceneSource::ProjectPrefabBuiltinComponent(payload, projection.components, projection.primitive);
            if (projected.HasError())
                return Result<void>::Failure(projected.ErrorValue());
            if (!projected.Value())
                return Result<void>::Failure(Unsupported());
            node.components.push_back(std::move(payload));
            return Result<void>::Success();
        }

        /** @brief Preserves collection occurrence boundaries without serializing an opaque component bag. */
        Result<void> AppendComponentField(const Kind kind, const Json &value, Prefab::PrefabObjectNode &node, std::uint64_t &nextInstance,
                                          ComponentProjection &projection) {
            if (kind != Kind::Collider && kind != Kind::PhysicsConstraint)
                return AppendBuiltinOccurrence(kind, value, node, nextInstance, projection);
            for (const auto &occurrence : value)
                if (const auto added = AppendBuiltinOccurrence(kind, occurrence, node, nextInstance, projection); added.HasError())
                    return added;
            return Result<void>::Success();
        }

        /** @brief Uses only declared canonical Scene fields and the existing built-in decoder, never opaque reference heuristics. */
        Result<void> AppendComponents(const SceneSource::SceneObjectSnapshot &object, const Json &encoded, Prefab::PrefabObjectNode &node,
                                      std::uint64_t &nextInstance) {
            const std::array fields{std::pair{"camera", Kind::Camera},
                                    std::pair{"light", Kind::Light},
                                    std::pair{"audioSource", Kind::AudioSource},
                                    std::pair{"audioListener", Kind::AudioListener},
                                    std::pair{"navigationSurface", Kind::NavigationSurface},
                                    std::pair{"navigationRegion", Kind::NavigationRegion},
                                    std::pair{"navigationModifier", Kind::NavigationModifier},
                                    std::pair{"navigationLink", Kind::NavigationLink},
                                    std::pair{"navigationAgent", Kind::NavigationAgent},
                                    std::pair{"aiAgent", Kind::AiAgent},
                                    std::pair{"aiController", Kind::AiController},
                                    std::pair{"characterController", Kind::CharacterController},
                                    std::pair{"rigidBody", Kind::RigidBody},
                                    std::pair{"colliders", Kind::Collider},
                                    std::pair{"physicsConstraints", Kind::PhysicsConstraint}};
            ComponentProjection projection;
            if (object.primitiveMesh)
                if (const auto added =
                        AppendBuiltinOccurrence(Kind::PrimitiveMesh, encoded.at("primitiveMesh"), node, nextInstance, projection);
                    added.HasError())
                    return added;
            for (const auto &[field, value] : encoded.at("components").items()) {
                if (field == "behaviors" || field == "gameplayComponents")
                    continue;
                const auto mapping = std::ranges::find_if(fields, [&field](const auto &entry) {
                    return field == entry.first;
                });
                if (mapping == fields.end())
                    return Result<void>::Failure(Unsupported());
                if (const auto added = AppendComponentField(mapping->second, value, node, nextInstance, projection); added.HasError())
                    return added;
            }
            return Result<void>::Success();
        }

        /** @brief Requires an exact active scalar behavior schema; preserved unknown fields never become portable by guesswork. */
        bool PortableBehavior(const Gameplay::BehaviorComponent &component, const Gameplay::BehaviorRegistry *registry) {
            if (!registry || !registry->IsFrozen())
                return false;
            const auto *registration = registry->Find(component.typeId);
            if (!registration || registration->descriptor.schemaVersion != component.schemaVersion ||
                Gameplay::ValidateBehaviorComponent(component).HasError())
                return false;
            for (const auto &field : component.fields) {
                const auto declaration =
                    std::ranges::find(registration->descriptor.fields, field.name, &Gameplay::BehaviorFieldDescriptor::name);
                if (declaration == registration->descriptor.fields.end() || declaration->defaultValue.index() != field.value.index())
                    return false;
            }
            return true;
        }

        /** @brief Audit the selected behavior schema against its frozen authority before encoding any source. */
        void AuditSelectedBehaviors(const std::vector<const SceneSource::SceneObjectSnapshot *> &selection,
                                    const Gameplay::BehaviorRegistry *registry, std::vector<PrefabCreationBoundary> &boundaries) {
            for (const auto *object : selection)
                for (const auto &behavior : object->components.behaviors)
                    if (!PortableBehavior(behavior, registry))
                        boundaries.push_back({object->id, {}, behavior.typeId.Value()});
        }

        /** @brief Captures typed AssetIds from all supported authored references, including disabled components. */
        Result<void> AppendAssets(const SceneSource::SceneObjectSnapshot &object, const Assets::AssetRegistrySnapshot &registry,
                                  std::vector<Assets::AssetId> &references) {
            const auto add = [&](const Assets::AssetId id, const std::string_view expected = {}) -> Result<void> {
                const auto *record = registry.Find(id);
                if (!record || (!expected.empty() && record->type.Value() != expected))
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::DependencyTypeMismatch));
                if (std::ranges::find(references, id) == references.end())
                    references.push_back(id);
                return Result<void>::Success();
            };
            const auto &components = object.components;
            if (components.audioSource) {
                const auto audio = std::visit([&]<typename T>(const T &target) -> Result<void> {
                    if constexpr (std::is_same_v<T, std::monostate>)
                        return Result<void>::Success();
                    else if constexpr (std::is_same_v<T, Audio::AudioSoundExtensionReference>)
                        return add(target.definition.Asset());
                    else
                        return add(target.Asset());
                }, components.audioSource->sound.target);
                if (audio.HasError())
                    return audio;
            }
            if (components.navigationSurface)
                if (auto added = add(components.navigationSurface->definition, "core.navmesh"); added.HasError())
                    return added;
            if (components.aiController)
                return Result<void>::Failure(Unsupported());  // Decision-asset identity has no portable AssetId projection in this tier.
            if (components.characterController && components.characterController->collision.defaultMaterial)
                if (auto added = add(components.characterController->collision.defaultMaterial->material, "core.physics_material");
                    added.HasError())
                    return added;
            for (const auto &collider : components.colliders) {
                if (const auto *shape = std::get_if<Runtime::PhysicsShapeAssetReference>(&collider.source))
                    if (auto added = add(shape->asset, "core.physics_shape"); added.HasError())
                        return added;
                for (const auto &material : collider.materials)
                    if (auto added = add(material.material, "core.physics_material"); added.HasError())
                        return added;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PreparePrefabSelectionSource */
    Result<Prefab::PrefabDocument> PreparePrefabSelectionSource(const SceneTransactionSnapshot &snapshot, const Id root,
                                                                const Assets::AssetId asset, const Application::HoroVersion &version,
                                                                const Assets::AssetRegistrySnapshot &registry,
                                                                const Gameplay::BehaviorRegistry *behaviors,
                                                                std::vector<PrefabCreationBoundary> &boundaries) {
        using Return = Result<Prefab::PrefabDocument>;
        boundaries.clear();
        auto selected = Subtree(snapshot, root);
        if (selected.HasError())
            return Return::Failure(selected.ErrorValue());
        if (selected.Value().size() > Prefab::PrefabHardLimits::SourceObjectCount)
            return Return::Failure(MakeError(Prefab::PrefabErrors::ObjectCountExceeded));
        if (!WithinAuditBudget(snapshot))
            return Return::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
        AuditReferences(snapshot, selected.Value(), boundaries);
        AuditSelectedBehaviors(selected.Value(), behaviors, boundaries);
        if (!boundaries.empty())
            return Return::Failure(Unsupported());
        auto limits = Prefab::PrefabLimitProfile::Create({});
        if (limits.HasError())
            return Return::Failure(limits.ErrorValue());
        Prefab::PrefabDocumentData data{.projectVersion = version, .assetId = asset};
        std::uint64_t nextInstance{1};
        for (std::size_t index = 0; index < selected.Value().size(); ++index) {
            const auto &object = *selected.Value()[index];
            Prefab::PrefabObjectNode node{.localId = {static_cast<std::uint32_t>(index)},
                                          .name = object.name,
                                          .localTransform = index == 0 ? Math::Transform{} : object.localTransform,
                                          .behaviors = object.components.behaviors};
            if (index != 0) {
                const auto parent = std::ranges::find_if(selected.Value(), [&](const auto *candidate) {
                    return candidate->id == object.parent;
                });
                node.parentLocalId = Prefab::LocalObjectId{static_cast<std::uint32_t>(parent - selected.Value().begin())};
            }
            auto encoded = Json::parse(SceneSource::EncodeSceneSource({std::span{&object, 1}, {}})).at("objects").front();
            RewriteBodyReferences(encoded, selected.Value());
            if (auto components = AppendComponents(object, encoded, node, nextInstance); components.HasError()) {
                boundaries.push_back({object.id, {}, "components.unsupported_portable_schema"});
                return Return::Failure(components.ErrorValue());
            }
            if (auto references = AppendAssets(object, registry, data.referencedAssets); references.HasError())
                return Return::Failure(references.ErrorValue());
            data.objects.push_back(std::move(node));
        }
        return Prefab::PrefabDocument::Create(std::move(data), limits.Value());
    }
}  // namespace Horo::Editor
