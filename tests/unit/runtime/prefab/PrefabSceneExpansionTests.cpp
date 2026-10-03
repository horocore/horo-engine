#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSceneExpansion.h"
#include "PrefabTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Prefab {
    namespace {
        PrefabLimitProfile ExpansionLimits(PrefabProjectPolicy policy = {}) {
            return PrefabLimitProfile::Create(policy).Value();
        }

        PrefabSourceRevision ExpansionRevision(const std::uint8_t value = 1) {
            Sha256Digest digest{};
            digest.bytes.back() = value;
            return {Application::ParseHoroVersion("1.2.3").Value(), digest};
        }

        PrefabObjectNode ExpansionObject(const std::uint32_t id, const std::optional<LocalObjectId> parent = std::nullopt) {
            return {.localId = {id}, .parentLocalId = parent, .name = "Display label"};
        }

        PrefabSourceResolverSnapshot ExpansionResolver(std::vector<PrefabObjectNode> objects, const std::uint8_t revision = 1) {
            Assets::AssetRegistry registry;
            const auto asset = Test::Asset();
            REQUIRE(registry
                        .Publish({{asset, Assets::AssetTypeId::Parse("core.prefab").Value(),
                                   ProjectPath::Parse("assets/prefabs/hierarchy.prefab").Value(),
                                   ProjectPath::Parse("assets/prefabs/hierarchy.prefab.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            auto document = PrefabDocument::Create({.projectVersion = ExpansionRevision().projectVersion,
                                                    .assetId = asset,
                                                    .objects = std::move(objects)},
                                                   ExpansionLimits());
            REQUIRE(document.HasValue());
            return BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {{std::move(document).Value(), ExpansionRevision(revision)}},
                                                     ExpansionLimits())
                .Value();
        }

        EffectivePrefabCandidate ExpansionCandidate(std::vector<PrefabObjectNode> objects) {
            return ExpansionResolver(std::move(objects))
                .Resolve(Test::Asset(), PrefabInstanceId::Create(7).Value(), ExpansionLimits())
                .Value();
        }

        PrefabSceneIdentityMap ExpansionIdentities(const EffectivePrefabCandidate &candidate) {
            return RemapPrefabCandidateToScene(candidate, {}, {}, ExpansionLimits()).Value();
        }

        std::vector<PrefabRuntimeComponentProjection> ExpansionProjections(const EffectivePrefabCandidate &candidate) {
            std::vector<PrefabRuntimeComponentProjection> components;
            for (const auto &object : candidate.Objects())
                components.emplace_back(object.key, Runtime::RuntimeComponentSet{.behaviors = object.object.behaviors});
            return components;
        }

        Gameplay::SerializedComponent ExpansionComponent(const std::size_t padding = 0) {
            std::string text = "{\"value\":\"" + std::string(padding, 'x') + "\"}";
            std::vector<std::byte> bytes;
            for (const char value : text)
                bytes.push_back(static_cast<std::byte>(value));
            return {.typeId = Gameplay::ComponentTypeId::Parse("game.tests.expansion").Value(), .payload = std::move(bytes)};
        }

        TEST_CASE("Prefab runtime expansion preserves ordered hierarchy stable identity and typed payloads", "[unit][prefab][expansion]") {
            auto root = ExpansionObject(0);
            root.localTransform.translation = {1, 2, 3};
            auto child = ExpansionObject(99, LocalObjectId{0});
            child.localTransform.translation = {0, 4, 0};
            child.behaviors = {{.instanceId = {4},
                                .typeId = Gameplay::BehaviorTypeId::Parse("game.tests.expansion").Value(),
                                .fields = {{.name = "speed", .value = 2.0}, {.name = "label", .value = std::string("kept")}}}};
            const auto candidate =
                ExpansionCandidate({root, child, ExpansionObject(3, LocalObjectId{0}), ExpansionObject(81, LocalObjectId{99})});
            const auto identities = ExpansionIdentities(candidate);
            auto components = ExpansionProjections(candidate);
            components[1].components.gameplayComponents = {ExpansionComponent()};
            components[2].components.camera = Runtime::CameraComponent{};
            std::ranges::reverse(components);
            PrefabRuntimePlacement placement{.rootTransform = {.translation = {10, 0, 0}}, .parent = Runtime::SceneObjectId{900}};
            const auto output = ExpandPrefabSceneSubtree(candidate, identities, components, placement, ExpansionLimits());
            REQUIRE(output.HasValue());
            REQUIRE(output.Value().Entities().size() == 4);
            const auto entities = output.Value().Entities();
            CHECK(entities[0].parent == placement.parent);
            CHECK(entities[0].localTransform.translation == Math::Vec3{11, 2, 3});
            CHECK(entities[1].localTransform == child.localTransform);
            CHECK(entities[1].parent == entities[0].object);
            CHECK(entities[2].parent == entities[0].object);
            CHECK(entities[3].parent == entities[1].object);
            CHECK(entities[1].components.behaviors == child.behaviors);
            CHECK(entities[1].components.gameplayComponents.front() == ExpansionComponent());
            CHECK(entities[2].components.camera.has_value());
            CHECK(output.Value().Revision() == candidate.Revision());
            for (std::size_t index = 0; index < entities.size(); ++index)
                CHECK(entities[index].object.value == identities.Find(candidate.Objects()[index].key)->value);
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{2}};
            builder.Add({.object = {900}});
            for (const auto &entity : entities)
                builder.Add(entity);
            const auto scene = std::move(builder).Build();
            REQUIRE(scene.HasValue());
            CHECK(scene.Value().Entities().size() == 5);
        }

        TEST_CASE("Prefab runtime expansion rechecks lower object depth and component policies atomically",
                  "[unit][prefab][expansion][boundary]") {
            const auto candidate =
                ExpansionCandidate({ExpansionObject(0), ExpansionObject(8, LocalObjectId{0}), ExpansionObject(17, LocalObjectId{8})});
            const auto identities = ExpansionIdentities(candidate);
            auto components = ExpansionProjections(candidate);
            PrefabProjectPolicy policy;
            policy.maximumObjectCount = 2;
            auto result = ExpandPrefabSceneSubtree(candidate, identities, components, {}, ExpansionLimits(policy));
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == PrefabErrors::ObjectCountExceeded.code.Value());
            policy.maximumObjectCount = 3;
            policy.maximumHierarchyDepth = 2;
            result = ExpandPrefabSceneSubtree(candidate, identities, components, {}, ExpansionLimits(policy));
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == PrefabErrors::HierarchyDepthExceeded.code.Value());
            policy.maximumHierarchyDepth = 3;
            policy.maximumComponentsPerObject = 1;
            components.back().components.camera = Runtime::CameraComponent{};
            components.back().components.light = Runtime::LightComponent{};
            result = ExpandPrefabSceneSubtree(candidate, identities, components, {}, ExpansionLimits(policy));
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == PrefabErrors::ComponentCountExceeded.code.Value());
            components.back().components.light.reset();
            result = ExpandPrefabSceneSubtree(candidate, identities, components, {}, ExpansionLimits(policy));
            REQUIRE(result.HasValue());
            CHECK(result.Value().Entities().size() == 3);
        }

        TEST_CASE("Prefab runtime expansion rejects missing duplicate and foreign typed projections",
                  "[unit][prefab][expansion][malformed]") {
            const auto candidate = ExpansionCandidate({ExpansionObject(0), ExpansionObject(8, LocalObjectId{0})});
            const auto identities = ExpansionIdentities(candidate);
            auto components = ExpansionProjections(candidate);
            SECTION("missing") {
                components.pop_back();
            }
            SECTION("duplicate") {
                components.back().object = components.front().object;
            }
            SECTION("foreign") {
                components.back().object.instance = PrefabInstanceId::Create(99).Value();
            }
            SECTION("invalid payload") {
                components.back().components.camera = Runtime::CameraComponent{.nearPlane = -1};
            }
            const auto result = ExpandPrefabSceneSubtree(candidate, identities, components, {}, ExpansionLimits());
            CHECK(result.HasError());
            CHECK(candidate.Objects().size() == 2);
            CHECK(identities.Mappings().size() == 2);
        }

        TEST_CASE("Prefab runtime expansion fences mismatched remaps while retained snapshots survive registry destruction",
                  "[unit][prefab][expansion][lifecycle]") {
            const auto candidate = ExpansionCandidate({ExpansionObject(0)});
            const auto components = ExpansionProjections(candidate);
            auto other = ExpansionResolver({ExpansionObject(0)}, 2)
                             .Resolve(Test::Asset(), PrefabInstanceId::Create(7).Value(), ExpansionLimits())
                             .Value();
            auto result = ExpandPrefabSceneSubtree(candidate, ExpansionIdentities(other), components, {}, ExpansionLimits());
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == PrefabErrors::ResolutionStale.code.Value());
            other = ExpansionResolver({ExpansionObject(0)})
                        .Resolve(Test::Asset(), PrefabInstanceId::Create(8).Value(), ExpansionLimits())
                        .Value();
            result = ExpandPrefabSceneSubtree(candidate, ExpansionIdentities(other), components, {}, ExpansionLimits());
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == PrefabErrors::IdentityCollision.code.Value());
            result = ExpandPrefabSceneSubtree(candidate, ExpansionIdentities(candidate), components, {}, ExpansionLimits());
            REQUIRE(result.HasValue());
            CHECK(result.Value().Entities().size() == 1);
        }

        TEST_CASE("Prefab runtime expansion bounds dynamic bytes and invalid placement without partial result",
                  "[unit][prefab][expansion][boundary]") {
            const auto candidate = ExpansionCandidate({ExpansionObject(0), ExpansionObject(7, LocalObjectId{0})});
            const auto identities = ExpansionIdentities(candidate);
            auto components = ExpansionProjections(candidate);
            components.back().components.gameplayComponents = {ExpansionComponent(20)};
            PrefabProjectPolicy policy;
            const auto &component = components.back().components.gameplayComponents.front();
            policy.maximumExpandedPayloadBytes = component.typeId.Value().size() + component.payload.size();
            REQUIRE(ExpandPrefabSceneSubtree(candidate, identities, components, {}, ExpansionLimits(policy)).HasValue());
            --policy.maximumExpandedPayloadBytes;
            const auto bytes = ExpandPrefabSceneSubtree(candidate, identities, components, {}, ExpansionLimits(policy));
            REQUIRE(bytes.HasError());
            CHECK(bytes.ErrorValue().code.Value() == PrefabErrors::PayloadTooLarge.code.Value());
            CHECK(ExpandPrefabSceneSubtree(candidate, identities, components, {.parent = Runtime::SceneObjectId{}}, ExpansionLimits())
                      .HasError());
            CHECK(ExpandPrefabSceneSubtree(candidate, identities, components,
                                           {.rootTransform = {.translation = {std::numeric_limits<float>::quiet_NaN(), 0, 0}}},
                                           ExpansionLimits())
                      .HasError());
            CHECK(ExpandPrefabSceneSubtree(candidate, identities, components, {.rootTransform = {.scale = {0, 1, 1}}}, ExpansionLimits())
                      .HasError());
        }

        TEST_CASE("Prefab runtime expansion rejects finite work exhaustion and overlong behavior projections",
                  "[unit][prefab][expansion][boundary]") {
            std::vector<PrefabObjectNode> objects{ExpansionObject(0)};
            for (std::uint32_t id = 1; id < 10; ++id)
                objects.push_back(ExpansionObject(id, LocalObjectId{0}));
            const auto candidate = ExpansionCandidate(std::move(objects));
            const auto identities = ExpansionIdentities(candidate);
            auto components = ExpansionProjections(candidate);
            PrefabProjectPolicy policy;
            policy.maximumObjectCount = 10;
            policy.maximumComponentsPerObject = 1;
            policy.maximumReferencedAssets = 1;
            policy.maximumDirectNestedPlacements = 1;
            policy.maximumVariantInheritanceDepth = 1;
            policy.maximumNestedPrefabDepth = 1;
            policy.maximumOverrideRecords = 1;
            policy.maximumPropertyPathSegments = 1;
            policy.maximumConflictAndOrphanRecords = 1;
            policy.maximumBindingUses = 1;
            const auto exhausted = ExpandPrefabSceneSubtree(candidate, identities, components, {}, ExpansionLimits(policy));
            REQUIRE(exhausted.HasError());
            CHECK(exhausted.ErrorValue().code.Value() == PrefabErrors::WorkBudgetExceeded.code.Value());
            components.back().components.behaviors = {
                {.instanceId = {1},
                 .typeId = Gameplay::BehaviorTypeId::Parse("game.tests.expansion").Value(),
                 .fields = std::vector<Gameplay::BehaviorField>(Gameplay::MaximumBehaviorFields + 1)}};
            const auto malformed = ExpandPrefabSceneSubtree(candidate, identities, components, {}, ExpansionLimits());
            REQUIRE(malformed.HasError());
            CHECK(malformed.ErrorValue().code.Value() == PrefabErrors::DocumentInvalid.code.Value());
        }

        TEST_CASE("Prefab runtime expansion accounts collider and navigation dynamic storage", "[unit][prefab][expansion][boundary]") {
            const auto candidate = ExpansionCandidate({ExpansionObject(0)});
            const auto identities = ExpansionIdentities(candidate);
            auto components = ExpansionProjections(candidate);
            PrefabProjectPolicy policy;
            policy.maximumExpandedPayloadBytes = 1;
            SECTION("collider binding storage") {
                components.front().components.colliders = {
                    Runtime::ColliderComponent{.materials = {Runtime::PhysicsColliderMaterialBinding{}}}};
            }
            SECTION("navigation profile storage") {
                components.front().components.navigationSurface = Runtime::NavigationSurfaceComponent{};
                components.front().components.navigationSurface->profiles.resize(1);
            }
            const auto result = ExpandPrefabSceneSubtree(candidate, identities, components, {}, ExpansionLimits(policy));
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == PrefabErrors::PayloadTooLarge.code.Value());
        }

        PrefabSourceResolverSnapshot NestedExpansionResolver() {
            const auto outer = Test::Asset(1);
            const auto inner = Test::Asset(2);
            Assets::AssetRegistry registry;
            REQUIRE(registry
                        .Publish({{outer, Assets::AssetTypeId::Parse("core.prefab").Value(),
                                   ProjectPath::Parse("assets/prefabs/outer.prefab").Value(),
                                   ProjectPath::Parse("assets/prefabs/outer.prefab.horo").Value()},
                                  {inner, Assets::AssetTypeId::Parse("core.prefab").Value(),
                                   ProjectPath::Parse("assets/prefabs/inner.prefab").Value(),
                                   ProjectPath::Parse("assets/prefabs/inner.prefab.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            const auto outerDocument =
                PrefabDocument::Create({.projectVersion = ExpansionRevision().projectVersion,
                                        .assetId = outer,
                                        .objects = {ExpansionObject(0), ExpansionObject(9, LocalObjectId{0})},
                                        .composition =
                                            PrefabComposition{
                                                .nestedPlacements = {{.placementLocalId = {17},
                                                                      .parentLocalId = LocalObjectId{9},
                                                                      .sourcePrefab = PrefabAssetReference::Create(inner).Value(),
                                                                      .authoredAgainst = ExpansionRevision(),
                                                                      .localRootTransform = {.translation = {0, 5, 0}}}}},
                                        .referencedAssets = {inner}},
                                       ExpansionLimits())
                    .Value();
            const auto innerDocument = PrefabDocument::Create({.projectVersion = ExpansionRevision().projectVersion,
                                                               .assetId = inner,
                                                               .objects = {ExpansionObject(0), ExpansionObject(4, LocalObjectId{0})}},
                                                              ExpansionLimits())
                                           .Value();
            return BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                     {{outerDocument, ExpansionRevision()}, {innerDocument, ExpansionRevision()}},
                                                     ExpansionLimits())
                .Value();
        }

        TEST_CASE("Prefab runtime expansion includes nested mounts in combined hierarchy depth", "[unit][prefab][expansion][nested]") {
            const auto resolver = NestedExpansionResolver();
            const auto outer = Test::Asset(1);
            const auto candidate = resolver.Resolve(outer, PrefabInstanceId::Create(1).Value(), ExpansionLimits()).Value();
            const auto identities = ExpansionIdentities(candidate);
            const auto components = ExpansionProjections(candidate);
            const auto expanded = ExpandPrefabSceneSubtree(candidate, identities, components,
                                                           {.rootTransform = {.translation = {10, 0, 0}}}, ExpansionLimits());
            REQUIRE(expanded.HasValue());
            REQUIRE(expanded.Value().Entities().size() == 4);
            CHECK(expanded.Value().Entities()[2].localTransform.translation == Math::Vec3{0, 5, 0});
            CHECK(expanded.Value().Entities()[2].parent == expanded.Value().Entities()[1].object);
            CHECK(expanded.Value().Entities()[3].parent == expanded.Value().Entities()[2].object);
            PrefabProjectPolicy policy;
            policy.maximumHierarchyDepth = 3;
            const auto denied = ExpandPrefabSceneSubtree(candidate, identities, components, {}, ExpansionLimits(policy));
            REQUIRE(denied.HasError());
            CHECK(denied.ErrorValue().code.Value() == PrefabErrors::HierarchyDepthExceeded.code.Value());
        }

        TEST_CASE("Prefab runtime expansion preserves typed reference remap and distinct repeated instance identity",
                  "[unit][prefab][expansion][identity]") {
            const auto resolver = ExpansionResolver({ExpansionObject(0), ExpansionObject(9, LocalObjectId{0})});
            const auto first = resolver.Resolve(Test::Asset(), PrefabInstanceId::Create(1).Value(), ExpansionLimits()).Value();
            const std::array requests{PrefabReferenceRewriteRequest{.owner = first.Objects()[0].key,
                                                                    .kind = PrefabReferenceKind::Entity,
                                                                    .objectTarget = first.Objects()[1].key}};
            const auto firstMap = RemapPrefabCandidateToScene(first, {}, requests, ExpansionLimits()).Value();
            const auto expanded = ExpandPrefabSceneSubtree(first, firstMap, ExpansionProjections(first), {}, ExpansionLimits()).Value();
            REQUIRE(firstMap.References().size() == 1);
            CHECK(firstMap.References().front().owner.value == expanded.Entities()[0].object.value);
            CHECK(firstMap.References().front().objectTarget->value == expanded.Entities()[1].object.value);
            const auto repeated = ExpandPrefabSceneSubtree(first, firstMap, ExpansionProjections(first), {}, ExpansionLimits()).Value();
            CHECK(repeated.Entities()[0].object == expanded.Entities()[0].object);
            const auto second = resolver.Resolve(Test::Asset(), PrefabInstanceId::Create(2).Value(), ExpansionLimits()).Value();
            const std::array occupied{PrefabSceneObjectId{expanded.Entities()[0].object.value},
                                      PrefabSceneObjectId{expanded.Entities()[1].object.value}};
            const auto secondMap = RemapPrefabCandidateToScene(second, occupied, {}, ExpansionLimits()).Value();
            const auto other = ExpandPrefabSceneSubtree(second, secondMap, ExpansionProjections(second), {}, ExpansionLimits()).Value();
            CHECK(other.Entities()[0].object != expanded.Entities()[0].object);
            CHECK(other.Entities()[1].object != expanded.Entities()[1].object);
        }
    }  // namespace
}  // namespace Horo::Prefab
