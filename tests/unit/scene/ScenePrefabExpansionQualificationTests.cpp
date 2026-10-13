#include "Horo/Prefab/PrefabSceneIdentityRemap.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "ScenePrefabExpansionTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/generators/catch_generators.hpp>
#include <limits>
#include <set>

namespace {
    using namespace Horo;
    using namespace Horo::Prefab;
    using namespace Horo::SceneSource;
    using namespace Horo::SceneSource::ExpansionTestSupport;

    /** @brief Real repeated nested hierarchy with sparse local IDs, owned behavior values and a transitive resource. */
    struct QualificationFixture final {
        ExpansionFixture source;
        const Assets::AssetId child = Assets::AssetId::FromBytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 18});
        const Assets::AssetId resource = Assets::AssetId::FromBytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 19});

        QualificationFixture() {
            Publish();
        }

        void Publish(const std::string_view rootPath = "assets/root.prefab") {
            REQUIRE(
                source.registry
                    .Publish({{source.asset, Assets::AssetTypeId::Parse("core.prefab").Value(), ProjectPath::Parse(rootPath).Value(),
                               ProjectPath::Parse(std::string{rootPath} + ".horo").Value()},
                              {child, Assets::AssetTypeId::Parse("core.prefab").Value(), ProjectPath::Parse("assets/child.prefab").Value(),
                               ProjectPath::Parse("assets/child.prefab.horo").Value()},
                              {resource, Assets::AssetTypeId::Parse("core.mesh").Value(), ProjectPath::Parse("assets/mesh.obj").Value(),
                               ProjectPath::Parse("assets/mesh.obj.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
        }

        PrefabSourceResolverSnapshot Resolver(const float leafOffset = 4, const bool reverse = false) const {
            const auto nested =
                source.Source({.projectVersion = source.version,
                               .assetId = child,
                               .objects = {{.localId = {0}, .name = "Nested root", .localTransform = {.translation = {0, 1, 0}}},
                                           {.localId = {std::numeric_limits<std::uint32_t>::max()},
                                            .parentLocalId = LocalObjectId{0},
                                            .name = "İstanbul leaf",
                                            .localTransform = {.translation = {0, leafOffset, 0}},
                                            .behaviors = {{.instanceId = {4},
                                                           .typeId = Gameplay::BehaviorTypeId::Parse("game.tests.qualification").Value(),
                                                           .fields = {{.name = "label", .value = std::string{"Owned leaf"}},
                                                                      {.name = "speed", .value = 2.0}}}}}},
                               .referencedAssets = {resource}});
            const auto root = source.Source(
                {.projectVersion = source.version,
                 .assetId = source.asset,
                 .objects = {{.localId = {0}, .name = "Root", .localTransform = {.translation = {1, 0, 0}}},
                             {.localId = {7}, .parentLocalId = LocalObjectId{0}, .localTransform = {.translation = {0, 2, 0}}}},
                 .composition = PrefabComposition{.nestedPlacements = {{.placementLocalId = {10},
                                                                        .parentLocalId = LocalObjectId{7},
                                                                        .sourcePrefab = PrefabAssetReference::Create(child).Value(),
                                                                        .authoredAgainst = nested.sourceRevision,
                                                                        .localRootTransform = {.translation = {0, 0, 3}}},
                                                                       {.placementLocalId = {20},
                                                                        .sourcePrefab = PrefabAssetReference::Create(child).Value(),
                                                                        .authoredAgainst = nested.sourceRevision,
                                                                        .localRootTransform = {.translation = {0, 0, 6}}}}},
                 .referencedAssets = {child}});
            auto result = BuildPrefabSourceResolverSnapshot(source.registry.Snapshot(),
                                                            reverse ? std::vector{nested, root} : std::vector{root, nested}, source.limits);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        ScenePrefabExpansionRequest Request(const std::size_t count) const {
            auto request = source.Request();
            request.resolver = Resolver();
            request.document.prefabInstances.clear();
            for (std::size_t index = 0; index < count; ++index) {
                request.document.prefabInstances.push_back({.instanceId = PrefabInstanceId::Create(index + 1).Value(),
                                                            .sourcePrefab = PrefabAssetReference::Create(source.asset).Value(),
                                                            .parent = SceneObjectId{9},
                                                            .rootTransform = {.translation = {static_cast<float>(index * 10), 0, 0}}});
            }
            return request;
        }
    };

    /** @brief Compares all runtime fields and the canonical cooked bytes, then reloads without a source resolver. */
    void RequireEquivalent(const Runtime::RuntimeSceneDefinition &actual, const Runtime::RuntimeSceneDefinition &expected) {
        REQUIRE(actual.Id() == expected.Id());
        REQUIRE(actual.Revision() == expected.Revision());
        REQUIRE(std::ranges::equal(actual.AssetDependencies(), expected.AssetDependencies()));
        REQUIRE(actual.Entities().size() == expected.Entities().size());
        for (std::size_t index = 0; index < actual.Entities().size(); ++index) {
            const auto &left = actual.Entities()[index];
            const auto &right = expected.Entities()[index];
            CHECK(left.object == right.object);
            CHECK(left.parent == right.parent);
            CHECK(left.localTransform == right.localTransform);
            CHECK(left.primitiveMesh == right.primitiveMesh);
            CHECK(left.components == right.components);
        }
        const auto encoded = SceneCook::EncodeCookedSceneDefinition(actual);
        const auto reference = SceneCook::EncodeCookedSceneDefinition(expected);
        REQUIRE(encoded.HasValue());
        REQUIRE(reference.HasValue());
        CHECK(encoded.Value() == reference.Value());
        const auto decoded = SceneCook::DecodeCookedSceneDefinition(encoded.Value(), actual.Id(), actual.Revision());
        REQUIRE(decoded.HasValue());
        const auto roundTrip = SceneCook::EncodeCookedSceneDefinition(decoded.Value());
        REQUIRE(roundTrip.HasValue());
        CHECK(roundTrip.Value() == reference.Value());
        const std::string_view text{reinterpret_cast<const char *>(encoded.Value().data()), encoded.Value().size()};
        CHECK(text.find("assets/") == std::string_view::npos);
        CHECK(text.find("prefabInstances") == std::string_view::npos);
        CHECK(text.find("sourcePrefab") == std::string_view::npos);
        CHECK(text.find("İstanbul leaf") == std::string_view::npos);
    }

    /** @brief Uses the production containing-scene conversion without mutating any authored value. */
    Result<Runtime::RuntimeSceneDefinition> Convert(const ScenePrefabExpansionRequest &request) {
        return ConvertSceneSourceToRuntime({request.document.objects, request.document.prefabInstances}, request.scene, request.revision,
                                           request.resolver, request.limits);
    }

    /** @brief Checks actual cached conversion against the complete immutable reference definition. */
    void RequireCachedEquivalent(const ScenePrefabExpansionRequest &request, PrefabExpansionCache &cache,
                                 const Runtime::RuntimeSceneDefinition &baseline) {
        const auto cached = ConvertSceneSourceToRuntime({request.document.objects, request.document.prefabInstances}, request.scene,
                                                        request.revision, request.resolver, request.limits, cache);
        REQUIRE(cached.HasValue());
        RequireEquivalent(cached.Value(), baseline);
    }

    /** @brief Verifies every mapped object in actual Scene output before reserving its identity for the next placement. */
    void RequireConvertedHierarchy(const EffectivePrefabCandidate &candidate, const PrefabSceneIdentityMap &map,
                                   const Runtime::RuntimeSceneDefinition &definition, const ScenePrefabInstance &placement,
                                   std::vector<PrefabSceneObjectId> &occupied) {
        for (const auto &object : candidate.Objects()) {
            const auto mapped = map.Find(object.key);
            REQUIRE(mapped.has_value());
            const auto entity =
                std::ranges::find(definition.Entities(), Runtime::SceneObjectId{mapped->value}, &Runtime::RuntimeEntityDefinition::object);
            REQUIRE(entity != definition.Entities().end());
            const auto expectedParent = object.parent ? Runtime::SceneObjectId{map.Find(*object.parent)->value} : Runtime::SceneObjectId{9};
            CHECK(entity->parent == expectedParent);
            CHECK(entity->components.behaviors == object.object.behaviors);
            if (object.parent)
                CHECK(entity->localTransform == object.effectiveLocalTransform);
            else
                CHECK(entity->localTransform.translation == placement.rootTransform.translation + Math::Vec3{1, 0, 0});
            occupied.push_back(*mapped);
        }
    }

    /** @brief Requires the original typed error for a complete failed conversion, without accepting partial output. */
    void RequireConversionFailure(const ScenePrefabExpansionRequest &request, const ErrorCodeDescriptor &expected) {
        const auto failed = Convert(request);
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == expected.code.Value());
    }
}  // namespace

TEST_CASE("Reference scene nested placements qualify repeated cold hit and reordered source paths", "[native][prefab][qualification]") {
    QualificationFixture fixture;
    const auto count = GENERATE(std::size_t{1}, std::size_t{8}, std::size_t{32});
    const auto request = fixture.Request(count);
    const auto authored = EncodeSceneSource({request.document.objects, request.document.prefabInstances});
    const auto baseline = Convert(request);
    REQUIRE(baseline.HasValue());
    REQUIRE(baseline.Value().Entities().size() == 1 + 6 * count);
    PrefabExpansionCache cache{{.maximumEntries = 32}};
    std::set<std::uint64_t> identities;
    for (const auto &entity : baseline.Value().Entities())
        CHECK(identities.insert(entity.object.value).second);
    for (unsigned run = 0; run < 3; ++run) {
        RequireCachedEquivalent(request, cache, baseline.Value());
        for (const auto &placement : request.document.prefabInstances) {
            const auto key = cache.CaptureKey(request.resolver, fixture.source.asset, placement.instanceId, request.limits);
            REQUIRE(key.HasValue());
            const auto retained = cache.Find(key.Value());
            REQUIRE(retained);
            const auto hit = cache.Resolve(request.resolver, fixture.source.asset, placement.instanceId, request.limits);
            REQUIRE(hit.HasValue());
            CHECK(hit.Value() == retained);
        }
    }
    auto reordered = request;
    reordered.resolver = fixture.Resolver(4, true);
    const auto repeated = Convert(reordered);
    REQUIRE(repeated.HasValue());
    RequireEquivalent(repeated.Value(), baseline.Value());
    CHECK(EncodeSceneSource({request.document.objects, request.document.prefabInstances}) == authored);
}

TEST_CASE("Reference scene hierarchy and local references retain independent nested instance scopes",
          "[native][prefab][qualification][references]") {
    QualificationFixture fixture;
    const auto request = fixture.Request(2);
    const auto definition = Convert(request);
    REQUIRE(definition.HasValue());
    std::vector<PrefabSceneObjectId> occupied{{9}};
    for (const auto &placement : request.document.prefabInstances) {
        const auto candidate = request.resolver.Resolve(fixture.source.asset, placement.instanceId, request.limits);
        REQUIRE(candidate.HasValue());
        REQUIRE(candidate.Value().Objects().size() == 6);
        const auto objects = candidate.Value().Objects();
        const std::array references{PrefabReferenceRewriteRequest{.owner = objects[2].key,
                                                                  .kind = PrefabReferenceKind::Entity,
                                                                  .objectTarget = objects[3].key},
                                    PrefabReferenceRewriteRequest{.owner = objects[4].key,
                                                                  .kind = PrefabReferenceKind::Entity,
                                                                  .objectTarget = objects[5].key},
                                    PrefabReferenceRewriteRequest{.owner = objects[3].key,
                                                                  .kind = PrefabReferenceKind::Behavior,
                                                                  .objectTarget = objects[5].key,
                                                                  .behaviorTarget = Gameplay::BehaviorInstanceId{4}}};
        const auto map = RemapPrefabCandidateToScene(candidate.Value(), occupied, references, request.limits);
        REQUIRE(map.HasValue());
        REQUIRE(map.Value().References().size() == 3);
        CHECK(map.Value().References()[0].objectTarget != map.Value().References()[1].objectTarget);
        CHECK(map.Value().References()[2].objectTarget == map.Value().Find(objects[5].key));
        auto crossing = references;
        crossing.back().objectTarget->instance = PrefabInstanceId::Create(999).Value();
        const auto invalid = RemapPrefabCandidateToScene(candidate.Value(), occupied, crossing, request.limits);
        REQUIRE(invalid.HasError());
        CHECK(invalid.ErrorValue().code.Value() == PrefabErrors::ReferenceRewriteInvalid.code.Value());
        CHECK(map.Value().References().size() == references.size());
        RequireConvertedHierarchy(candidate.Value(), map.Value(), definition.Value(), placement, occupied);
        CHECK(objects[3].effectiveLocalTransform.translation == Math::Vec3{0, 4, 0});
        CHECK(objects[2].effectiveLocalTransform.translation == Math::Vec3{0, 1, 3});
        CHECK(objects[4].effectiveLocalTransform.translation == Math::Vec3{0, 1, 6});
        CHECK(objects[3].key.object.SourceObject().value == std::numeric_limits<std::uint32_t>::max());
    }
}

TEST_CASE("Reference scene source moves preserve output and stale nested revisions require fresh resolution",
          "[native][prefab][qualification][lifecycle]") {
    QualificationFixture fixture;
    auto request = fixture.Request(3);
    const auto baseline = Convert(request);
    REQUIRE(baseline.HasValue());
    auto retainedResult =
        BuildScenePrefabProjection({request.document.objects, request.document.prefabInstances}, request.resolver, request.limits);
    REQUIRE(retainedResult.HasValue());
    auto retained = std::move(retainedResult).Value();
    fixture.Publish("assets/moved folder/İstanbul.prefab");
    request.resolver = fixture.Resolver();
    const auto moved = Convert(request);
    REQUIRE(moved.HasValue());
    RequireEquivalent(moved.Value(), baseline.Value());
    const auto stillCurrent =
        ConvertScenePrefabProjectionToRuntime({request.document.objects, request.document.prefabInstances}, request.scene, request.revision,
                                              retained, request.resolver, request.limits);
    REQUIRE(stillCurrent.HasValue());
    RequireEquivalent(stillCurrent.Value(), baseline.Value());
    request.resolver = fixture.Resolver(9);
    const std::array changed{fixture.child};
    InvalidateScenePrefabProjection(retained, request.resolver, changed, request.limits);
    const auto stale = ConvertScenePrefabProjectionToRuntime({request.document.objects, request.document.prefabInstances}, request.scene,
                                                             request.revision, retained, request.resolver, request.limits);
    REQUIRE(stale.HasError());
    CHECK(stale.ErrorValue().code.Value() == PrefabErrors::ResolutionStale.code.Value());
    const auto replacement = Convert(request);
    REQUIRE(replacement.HasValue());
    CHECK(SceneCook::EncodeCookedSceneDefinition(replacement.Value()).Value() !=
          SceneCook::EncodeCookedSceneDefinition(baseline.Value()).Value());
    CHECK(baseline.Value().Entities()[4].localTransform.translation == Math::Vec3{0, 4, 0});
}

TEST_CASE("Reference scene exact expansion bounds and identity pressure reject complete candidates",
          "[native][prefab][qualification][boundary]") {
    QualificationFixture fixture;
    auto request = fixture.Request(2);
    const auto previous = Convert(request);
    REQUIRE(previous.HasValue());
    auto policy = request.limits.Policy();
    policy.maximumObjectCount = 6;
    policy.maximumHierarchyDepth = 4;
    request.limits = PrefabLimitProfile::Create(policy).Value();
    REQUIRE(Convert(request).HasValue());
    SECTION("one expanded object beyond captured limit") {
        policy.maximumObjectCount = 5;
        request.limits = PrefabLimitProfile::Create(policy).Value();
        RequireConversionFailure(request, PrefabErrors::ObjectCountExceeded);
    }
    SECTION("one expanded hierarchy level beyond captured limit") {
        policy.maximumHierarchyDepth = 3;
        request.limits = PrefabLimitProfile::Create(policy).Value();
        RequireConversionFailure(request, PrefabErrors::HierarchyDepthExceeded);
    }
    SECTION("authored identity occupies a deterministic expansion ID") {
        request.document.objects.push_back({.id = {previous.Value().Entities().back().object.value}});
        RequireConversionFailure(request, PrefabErrors::IdentityCollision);
    }
    SECTION("later placement repeats an existing instance ID") {
        request.document.prefabInstances.back().instanceId = request.document.prefabInstances.front().instanceId;
        RequireConversionFailure(request, PrefabErrors::IdentityCollision);
    }
    SECTION("later required source is absent") {
        request.document.prefabInstances.back().sourcePrefab = PrefabAssetReference::Create(fixture.resource).Value();
        RequireConversionFailure(request, PrefabErrors::DependencyUnavailable);
    }
    CHECK(previous.Value().Entities().size() == 13);
}

TEST_CASE("Reference scene accepts hard object and hierarchy ceilings with deterministic sparse identities",
          "[native][prefab][qualification][boundary]") {
    ExpansionFixture fixture;
    const bool deep = GENERATE(false, true);
    const auto count = deep ? fixture.limits.Policy().maximumHierarchyDepth : fixture.limits.Policy().maximumObjectCount;
    PrefabDocumentData data{.projectVersion = fixture.version, .assetId = fixture.asset};
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto local = index * 1009;
        std::optional<LocalObjectId> parent;
        if (index != 0) {
            const std::uint32_t parentSlot = deep ? (index - 1) * 1009 : 0;
            parent = LocalObjectId{parentSlot};
        }
        data.objects.push_back({.localId = {local}, .parentLocalId = parent, .name = "Boundary member"});
    }
    const auto source = fixture.Source(std::move(data));
    const auto resolver = BuildPrefabSourceResolverSnapshot(fixture.registry.Snapshot(), {source}, fixture.limits);
    REQUIRE(resolver.HasValue());
    auto request = fixture.Request();
    request.resolver = resolver.Value();
    const auto baseline = Convert(request);
    REQUIRE(baseline.HasValue());
    CHECK(baseline.Value().Entities().size() == count + 1);
    PrefabExpansionCache cache;
    for (unsigned run = 0; run < 2; ++run) {
        RequireCachedEquivalent(request, cache, baseline.Value());
    }
    auto policy = request.limits.Policy();
    if (deep)
        --policy.maximumHierarchyDepth;
    else
        --policy.maximumObjectCount;
    request.limits = PrefabLimitProfile::Create(policy).Value();
    const auto rejected = Convert(request);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() ==
          (deep ? PrefabErrors::HierarchyDepthExceeded.code.Value() : PrefabErrors::ObjectCountExceeded.code.Value()));
}

TEST_CASE("Reference scene real owned jobs qualify stale cancellation replacement and shutdown fences",
          "[native][prefab][qualification][jobs]") {
    QualificationFixture fixture;
    auto request = fixture.Request(8);
    const auto baseline = Convert(request);
    REQUIRE(baseline.HasValue());
    JobSystem jobs{JobSystemConfig{.workerCount = 2}};
    ScenePrefabExpansionOwner owner{jobs};
    CancellationSource cancellation;
    REQUIRE(owner.Submit(request, cancellation.Token()).HasValue());
    REQUIRE(owner.Join(TestJoin).HasValue());
    SECTION("complete output matches direct and cooked definitions") {
        const auto completed = owner.TakeCompleted(request);
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value().has_value());
        RequireEquivalent(*completed.Value(), baseline.Value());
    }
    SECTION("late cancellation cannot publish the complete worker result") {
        cancellation.RequestCancellation();
        CHECK(owner.TakeCompleted(request).HasError());
    }
    SECTION("nested source edit invalidates all affected placements") {
        request.resolver = fixture.Resolver(9);
        CHECK(owner.TakeCompleted(request).HasError());
    }
    SECTION("document close retires the result and closes admission") {
        REQUIRE(owner.CloseDocument(TestJoin).HasValue());
        CHECK_FALSE(owner.TakeCompleted(request).Value());
        CHECK(owner.Submit(request).HasError());
    }
    REQUIRE(owner.ReplaceScene(TestJoin).HasValue());
    ++request.documentSession;
    REQUIRE(owner.Submit(request).HasValue());
    REQUIRE(owner.Join(TestJoin).HasValue());
    const auto replacement = owner.TakeCompleted(request);
    REQUIRE(replacement.HasValue());
    REQUIRE(replacement.Value().has_value());
    const auto current = Convert(request);
    REQUIRE(current.HasValue());
    RequireEquivalent(*replacement.Value(), current.Value());
    REQUIRE(owner.Shutdown(TestJoin).HasValue());
    CHECK(owner.Submit(request).HasError());
}
