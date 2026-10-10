#include "AllocationProbe.h"
#include "CanonicalJsonWriter.h"
#include "ScenePrefabExpansionTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <new>
#include <optional>
#include <semaphore>
#include <thread>
#include <type_traits>

using namespace Horo;
using namespace Horo::Prefab;
using namespace Horo::SceneSource;
using namespace Horo::SceneSource::ExpansionTestSupport;

TEST_CASE("Expansion cache exact hits preserve immutable identity and fresh Scene output", "[native][prefab][cache]") {
    ExpansionFixture fixture;
    const auto request = fixture.Request();
    PrefabExpansionCache cache;
    const auto &placement = request.document.prefabInstances.front();
    const auto first = cache.Resolve(request.resolver, fixture.asset, placement.instanceId, fixture.limits);
    REQUIRE(first.HasValue());
    const auto hit = cache.Resolve(request.resolver, fixture.asset, placement.instanceId, fixture.limits);
    REQUIRE(hit.HasValue());
    CHECK(first.Value() == hit.Value());
    const auto fresh = request.resolver.Resolve(fixture.asset, placement.instanceId, fixture.limits);
    REQUIRE(fresh.HasValue());
    CHECK(std::ranges::equal(hit.Value()->Objects(), fresh.Value().Objects()));
    const SceneSourceView view{request.document.objects, request.document.prefabInstances};
    const auto uncached = ConvertSceneSourceToRuntime(view, request.scene, request.revision, request.resolver, fixture.limits);
    const auto cached = ConvertSceneSourceToRuntime(view, request.scene, request.revision, request.resolver, fixture.limits, cache);
    REQUIRE(uncached.HasValue());
    REQUIRE(cached.HasValue());
    CHECK(cached.Value().Entities().size() == uncached.Value().Entities().size());
    CHECK(cached.Value().Entities()[1].object == uncached.Value().Entities()[1].object);
    CHECK(cached.Value().Entities()[1].localTransform == uncached.Value().Entities()[1].localTransform);
    CHECK(cached.Value().Id() == uncached.Value().Id());
    CHECK(cached.Value().Revision() == uncached.Value().Revision());
    CHECK(std::ranges::equal(cached.Value().AssetDependencies(), uncached.Value().AssetDependencies()));
    for (std::size_t index = 0; index < cached.Value().Entities().size(); ++index) {
        const auto &actual = cached.Value().Entities()[index];
        const auto &expected = uncached.Value().Entities()[index];
        CHECK(actual.object == expected.object);
        CHECK(actual.parent == expected.parent);
        CHECK(actual.localTransform == expected.localTransform);
        CHECK(actual.primitiveMesh == expected.primitiveMesh);
        CHECK(actual.components == expected.components);
    }
    REQUIRE(cache.Clear().HasValue());
    CHECK(first.Value()->Objects().front().object.name == "Root");
}

TEST_CASE("Expansion key includes actual source bytes registry occurrence and all settings", "[native][prefab][cache]") {
    ExpansionFixture fixture;
    PrefabExpansionCache cache;
    const auto resolver = fixture.Resolver();
    const auto instance = PrefabInstanceId::Create(4).Value();
    const auto first = cache.Resolve(resolver, fixture.asset, instance, fixture.limits).Value();
    SECTION("claimed source revision cannot hide changed canonical values") {
        const auto changed = fixture.Resolver("Edited", first->Revision().rootSource);
        const auto result = cache.Resolve(changed, fixture.asset, instance, fixture.limits);
        REQUIRE(result.HasValue());
        CHECK(result.Value() != first);
        CHECK(result.Value()->Objects().front().object.name == "Edited");
    }
    SECTION("ordinary resource publication conservatively invalidates") {
        fixture.Publish();
        const auto result = cache.Resolve(fixture.Resolver(), fixture.asset, instance, fixture.limits);
        REQUIRE(result.HasValue());
        CHECK(result.Value() != first);
    }
    SECTION("placement occurrence is not shared identity") {
        const auto result = cache.Resolve(resolver, fixture.asset, PrefabInstanceId::Create(5).Value(), fixture.limits);
        REQUIRE(result.HasValue());
        CHECK(result.Value()->Objects().front().key.instance != first->Objects().front().key.instance);
    }
    SECTION("even non-output limit policy fields participate") {
        auto policy = fixture.limits.Policy();
        --policy.maximumRuntimeSpawnDepth;
        const auto result = cache.Resolve(resolver, fixture.asset, instance, PrefabLimitProfile::Create(policy).Value());
        REQUIRE(result.HasValue());
        CHECK(result.Value() != first);
    }
    CHECK(first->Objects().front().object.name == "Root");
}

TEST_CASE("Expansion cache finite capacity cancellation and malformed inputs preserve leases", "[native][prefab][cache]") {
    ExpansionFixture fixture;
    const auto resolver = fixture.Resolver();
    const auto instance = PrefabInstanceId::Create(4).Value();
    PrefabExpansionCache cache{{.maximumEntries = 1}};
    const auto first = cache.Resolve(resolver, fixture.asset, instance, fixture.limits).Value();
    SECTION("eviction retires only cache ownership") {
        REQUIRE(cache.Resolve(resolver, fixture.asset, PrefabInstanceId::Create(5).Value(), fixture.limits).HasValue());
        CHECK_FALSE(cache.Find(cache.CaptureKey(resolver, fixture.asset, instance, fixture.limits).Value()));
    }
    SECTION("next byte beyond key budget rejects before insertion") {
        PrefabExpansionCache tiny{{.maximumKeySourceBytes = 1}};
        CHECK(tiny.Resolve(resolver, fixture.asset, instance, fixture.limits).HasError());
        CHECK(tiny.RetainedBytes() == 0);
    }
    SECTION("retained storage overflow publishes no cache entry") {
        PrefabExpansionCache tiny{{.maximumRetainedBytes = 1}};
        CHECK(tiny.Resolve(resolver, fixture.asset, instance, fixture.limits).HasError());
        CHECK(tiny.RetainedBytes() == 0);
    }
    SECTION("cancelled hit is not successful reuse") {
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        CHECK(cache.Resolve(resolver, fixture.asset, instance, fixture.limits, cancellation.Token()).HasError());
    }
    SECTION("lower bound settings cannot reuse over-budget candidate") {
        auto policy = fixture.limits.Policy();
        policy.maximumObjectCount = 1;
        CHECK(cache.Resolve(resolver, fixture.asset, instance, PrefabLimitProfile::Create(policy).Value()).HasError());
    }
    SECTION("invalid occurrence cannot reuse") {
        CHECK(cache.Resolve(resolver, fixture.asset, {}, fixture.limits).HasError());
    }
    CHECK(first->Objects().size() == 2);
}

TEST_CASE("Expansion allocation failure never evicts the prior exact immutable candidate", "[native][prefab][cache][allocation]") {
    ExpansionFixture fixture;
    const auto resolver = fixture.Resolver();
    const auto firstInstance = PrefabInstanceId::Create(4).Value();
    const auto nextInstance = PrefabInstanceId::Create(5).Value();
    std::size_t allocations{};
    {
        PrefabExpansionCache probe{{.maximumEntries = 1}};
        REQUIRE(probe.Resolve(resolver, fixture.asset, firstInstance, fixture.limits).HasValue());
        const auto next = [&] {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            auto result = probe.Resolve(resolver, fixture.asset, nextInstance, fixture.limits);
            allocations = measurement.Snapshot().requests;
            return result;
        }();
        REQUIRE(next.HasValue());
    }
    REQUIRE(allocations > 0);
    REQUIRE(allocations < 4096);
    for (std::size_t failureIndex = 0; failureIndex < allocations; ++failureIndex) {
        PrefabExpansionCache cache{{.maximumEntries = 1}};
        const auto first = cache.Resolve(resolver, fixture.asset, firstInstance, fixture.limits).Value();
        const auto key = cache.CaptureKey(resolver, fixture.asset, firstInstance, fixture.limits).Value();
        const auto retained = cache.RetainedBytes();
        const auto failed = [&] {
            Tests::AllocationProbe::ScopedFailure failure{failureIndex};
            return cache.Resolve(resolver, fixture.asset, nextInstance, fixture.limits);
        }();
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == PrefabErrors::ExpansionCacheAllocationFailed.code.Value());
        CHECK(cache.Find(key) == first);
        CHECK(cache.RetainedBytes() == retained);
        CHECK(first->Objects().front().key.instance == firstInstance);
    }
}

TEST_CASE("Every captured project policy field changes the exact expansion key", "[native][prefab][cache]") {
    ExpansionFixture fixture;
    PrefabExpansionCache cache;
    const auto resolver = fixture.Resolver();
    const auto instance = PrefabInstanceId::Create(4).Value();
    const auto baseline = cache.CaptureKey(resolver, fixture.asset, instance, fixture.limits).Value();
    const std::array fields{&PrefabProjectPolicy::maximumHierarchyDepth,
                            &PrefabProjectPolicy::maximumObjectCount,
                            &PrefabProjectPolicy::maximumSourcePayloadBytes,
                            &PrefabProjectPolicy::maximumExpandedPayloadBytes,
                            &PrefabProjectPolicy::maximumCookedPayloadBytes,
                            &PrefabProjectPolicy::maximumComponentsPerObject,
                            &PrefabProjectPolicy::maximumReferencedAssets,
                            &PrefabProjectPolicy::maximumDirectNestedPlacements,
                            &PrefabProjectPolicy::maximumVariantInheritanceDepth,
                            &PrefabProjectPolicy::maximumNestedPrefabDepth,
                            &PrefabProjectPolicy::maximumOverrideRecords,
                            &PrefabProjectPolicy::maximumConflictAndOrphanRecords,
                            &PrefabProjectPolicy::maximumPropertyPathSegments,
                            &PrefabProjectPolicy::maximumOverrideValueBytes,
                            &PrefabProjectPolicy::maximumOverrideSetBytes,
                            &PrefabProjectPolicy::maximumBindingSlots,
                            &PrefabProjectPolicy::maximumBindingUses,
                            &PrefabProjectPolicy::maximumInstanceBindings,
                            &PrefabProjectPolicy::maximumRuntimeSpawnDepth};
    for (const auto field : fields) {
        auto policy = fixture.limits.Policy();
        --(policy.*field);
        const auto limits = PrefabLimitProfile::Create(policy);
        REQUIRE(limits.HasValue());
        const auto changed = cache.CaptureKey(resolver, fixture.asset, instance, limits.Value());
        REQUIRE(changed.HasValue());
        CHECK(changed.Value() != baseline);
    }
}

TEST_CASE("Canonical serialization survives every production allocation failure", "[native][prefab][allocation]") {
    ExpansionFixture fixture;
    const auto name =
        GENERATE("Root", "Long owned source text with UTF-8: İstanbul, a quote \" and escaped newline\n and backslash \\ end");
    const auto resolver = fixture.Resolver(name);
    const auto &document = resolver.Sources().front().document;
    const auto canonical = document.SerializeCanonical().Value();
    std::size_t allocations{};
    const auto measured = [&] {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        auto result = document.SerializeCanonical();
        allocations = measurement.Snapshot().requests;
        return result;
    }();
    REQUIRE(measured.HasValue());
    REQUIRE(allocations > 0);
    REQUIRE(allocations < 4096);
    for (std::size_t index = 0; index < allocations; ++index) {
        INFO("canonical allocation index " << index);
        const auto failed = [&] {
            Tests::AllocationProbe::ScopedFailure failure{index};
            return document.SerializeCanonical();
        }();
        REQUIRE(failed.HasError());
        RequireAllocationFailurePreservesSource(failed.ErrorValue(), document, canonical);
    }
}

TEST_CASE("Resolver admission computes actual immutable evidence transactionally under every allocation failure",
          "[native][prefab][allocation]") {
    ExpansionFixture fixture;
    const auto resolver = fixture.Resolver();
    const auto registry = fixture.registry.Snapshot();
    const auto &document = resolver.Sources().front().document;
    const auto canonical = document.SerializeCanonical().Value();
    std::size_t allocations{};
    auto sources = resolver.Sources();
    auto ownedSources = std::vector<PrefabDependencySource>{sources.begin(), sources.end()};
    const auto measured = [&] {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        auto result = BuildPrefabSourceResolverSnapshot(registry, std::move(ownedSources), fixture.limits);
        allocations = measurement.Snapshot().requests;
        return result;
    }();
    REQUIRE(measured.HasValue());
    REQUIRE(allocations > 0);
    REQUIRE(allocations < 4096);
    REQUIRE(measured.Value().CanonicalSourceCommitments().size() == 1);
    CHECK(measured.Value().CanonicalSourceCommitments().front().digest == ComputeSha256(std::as_bytes(std::span{canonical})));
    CHECK(measured.Value().CanonicalSourceCommitments().front().encodedBytes == canonical.size());
    for (std::size_t index = 0; index < allocations; ++index) {
        ownedSources.assign(sources.begin(), sources.end());
        const auto failed = [&] {
            Tests::AllocationProbe::ScopedFailure failure{index};
            return BuildPrefabSourceResolverSnapshot(registry, std::move(ownedSources), fixture.limits);
        }();
        REQUIRE(failed.HasError());
        RequireAllocationFailurePreservesSource(failed.ErrorValue(), document, canonical);
        CHECK(resolver.CanonicalSourceCommitments().front().encodedBytes == canonical.size());
    }
}

TEST_CASE("Cache retained and canonical source byte ceilings accept their exact boundary", "[native][prefab][cache]") {
    ExpansionFixture fixture;
    const auto resolver = fixture.Resolver();
    const auto instance = PrefabInstanceId::Create(4).Value();
    const auto sourceBytes = resolver.Sources().front().document.SerializeCanonical().Value().size();
    PrefabExpansionCache measured;
    REQUIRE(measured.Resolve(resolver, fixture.asset, instance, fixture.limits).HasValue());
    const auto retained = measured.RetainedBytes();
    PrefabExpansionCache exact{{.maximumEntries = 1, .maximumRetainedBytes = retained, .maximumKeySourceBytes = sourceBytes}};
    REQUIRE(exact.Resolve(resolver, fixture.asset, instance, fixture.limits).HasValue());
    PrefabExpansionCache nextByte{{.maximumEntries = 1, .maximumRetainedBytes = retained - 1, .maximumKeySourceBytes = sourceBytes}};
    CHECK(nextByte.Resolve(resolver, fixture.asset, instance, fixture.limits).HasError());
    CHECK(nextByte.RetainedBytes() == 0);
    PrefabExpansionCache nextSourceByte{{.maximumKeySourceBytes = sourceBytes - 1}};
    CHECK(nextSourceByte.Resolve(resolver, fixture.asset, instance, fixture.limits).HasError());
}

TEST_CASE("Transitive source commitments are identity associated and cannot reuse changed nested content", "[native][prefab][cache]") {
    ExpansionFixture fixture;
    const auto childAsset = Assets::AssetId::FromBytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 18});
    fixture.PublishDependency(childAsset, "core.prefab", "child.prefab");
    const auto child =
        fixture.Source({.projectVersion = fixture.version, .assetId = childAsset, .objects = {{.localId = {0}, .name = "Nested"}}});
    const auto root = fixture.Source(
        {.projectVersion = fixture.version,
         .assetId = fixture.asset,
         .objects = {{.localId = {0}, .name = "Root"}},
         .composition = PrefabComposition{.nestedPlacements = {{.placementLocalId = {1},
                                                                .sourcePrefab = PrefabAssetReference::Create(childAsset).Value(),
                                                                .authoredAgainst = child.sourceRevision}}},
         .referencedAssets = {childAsset}});
    const auto snapshot = fixture.registry.Snapshot();
    const auto ordered = BuildPrefabSourceResolverSnapshot(snapshot, {root, child}, fixture.limits);
    const auto reversed = BuildPrefabSourceResolverSnapshot(snapshot, {child, root}, fixture.limits);
    REQUIRE(ordered.HasValue());
    REQUIRE(reversed.HasValue());
    PrefabExpansionCache cache;
    const auto instance = PrefabInstanceId::Create(4).Value();
    const auto first = cache.Resolve(ordered.Value(), fixture.asset, instance, fixture.limits);
    REQUIRE(first.HasValue());
    REQUIRE(first.Value()->Objects().size() == 2);
    const auto reordered = cache.Resolve(reversed.Value(), fixture.asset, instance, fixture.limits);
    REQUIRE(reordered.HasValue());
    CHECK(reordered.Value() == first.Value());
    auto changedData = child.document.Data();
    changedData.objects.front().name = "Changed nested value";
    const auto changedChild = fixture.Source(std::move(changedData), child.sourceRevision);
    const auto changed = BuildPrefabSourceResolverSnapshot(snapshot, {root, changedChild}, fixture.limits);
    REQUIRE(changed.HasValue());
    const auto next = cache.Resolve(changed.Value(), fixture.asset, instance, fixture.limits);
    REQUIRE(next.HasValue());
    CHECK(next.Value() != first.Value());
    const auto fresh = changed.Value().Resolve(fixture.asset, instance, fixture.limits);
    REQUIRE(fresh.HasValue());
    CHECK(std::ranges::equal(next.Value()->Objects(), fresh.Value().Objects()));
    CHECK(first.Value()->Objects()[1].object.name == "Nested");
}

TEST_CASE("Resource dependencies remain exact graph evidence without requiring a prefab source document", "[native][prefab][cache]") {
    ExpansionFixture fixture;
    const auto mesh = Assets::AssetId::FromBytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 19});
    fixture.PublishDependency(mesh, "core.mesh", "mesh.obj");
    auto source = fixture.Source({.projectVersion = fixture.version,
                                  .assetId = fixture.asset,
                                  .objects = {{.localId = {0}, .name = "Root"}},
                                  .referencedAssets = {mesh}});
    auto resolver = BuildPrefabSourceResolverSnapshot(fixture.registry.Snapshot(), {std::move(source)}, fixture.limits);
    REQUIRE(resolver.HasValue());
    PrefabExpansionCache cache;
    const auto instance = PrefabInstanceId::Create(4).Value();
    const auto result = cache.Resolve(resolver.Value(), fixture.asset, instance, fixture.limits);
    REQUIRE(result.HasValue());
    CHECK(result.Value()->Revision().dependencies.size() == 2);
    const auto hit = cache.Resolve(resolver.Value(), fixture.asset, instance, fixture.limits);
    REQUIRE(hit.HasValue());
    CHECK(hit.Value() == result.Value());
}

using OrderedWireValue = Horo::JsonEncoding::Detail::CanonicalJsonValue<false>;
using SortedWireValue = Horo::JsonEncoding::Detail::CanonicalJsonValue<true>;

TEMPLATE_TEST_CASE("Recursive canonical copies preserve source and destination at every allocation failure",
                   "[native][prefab][wire][allocation]", OrderedWireValue, SortedWireValue) {
    using Wire = TestType;
    STATIC_REQUIRE(std::is_nothrow_destructible_v<Wire>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<Wire>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<Wire>);
    const bool arrayRoot = GENERATE(false, true);
    const bool assignment = GENERATE(false, true);
    const Wire child{{"values", Wire::array({std::string(128, 'x'), Wire{{"nested", Wire::array({1, 2, 3})}}})}};
    const Wire source = arrayRoot ? Wire::array({child, child}) : Wire{{"first", child}, {"second", child}};
    const std::string sourceBytes = source.dump(2);
    std::optional<Wire> retiring{source};
    const std::size_t freedBefore = Tests::AllocationProbe::FreeCount();
    std::size_t destructionAllocations{};
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        Tests::AllocationProbe::ScopedFailure failure{0};
        retiring.reset();
        destructionAllocations = measurement.Snapshot().requests;
    }
    CHECK(destructionAllocations == 0);
    CHECK(Tests::AllocationProbe::FreeCount() > freedBefore);
    CHECK(source.dump(2) == sourceBytes);
    const Wire previous{{"preserved", std::string(128, 'p')}};
    const std::string previousBytes = previous.dump(2);
    std::size_t allocations{};
    {
        Wire destination{previous};
        std::optional<Wire> constructed;
        Tests::AllocationProbe::ScopedMeasurement measurement;
        if (assignment)
            destination = source;
        else
            constructed.emplace(source);
        allocations = measurement.Snapshot().requests;
    }
    REQUIRE(allocations > 0);
    REQUIRE(allocations < 4096);
    for (std::size_t index = 0; index < allocations; ++index) {
        INFO("recursive copy allocation index " << index << ", array root " << arrayRoot << ", assignment " << assignment);
        Wire destination{previous};
        std::optional<Wire> constructed;
        bool rejected{};
        {
            Tests::AllocationProbe::ScopedFailure failure{index};
            try {
                if (assignment)
                    destination = source;
                else
                    constructed.emplace(source);
            } catch (const std::bad_alloc &) {
                rejected = true;
            }
        }
        REQUIRE(rejected);
        CHECK_FALSE(constructed.has_value());
        CHECK(destination.dump(2) == previousBytes);
        CHECK(source.dump(2) == sourceBytes);
        constructed.emplace(source);
        CHECK(constructed->dump(2) == sourceBytes);
        destination = source;
        CHECK(destination.dump(2) == sourceBytes);
    }
}
