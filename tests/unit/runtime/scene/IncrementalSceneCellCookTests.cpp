#include "../world_streaming/StreamingCellCandidateTestSupport.h"
#include "Horo/Runtime/Scene/IncrementalSceneCellCook.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Runtime {
    namespace {
        namespace W = WorldStreaming;
        using W::CandidateTestSupport::Cell;
        using W::CandidateTestSupport::Hash;
        using W::TestSupport::Asset;
        using W::TestSupport::World;

        SceneCellCookCacheLimits CacheLimits() {
            return {8, 32, 64, 32, 4 * 1024 * 1024, 4 * 1024 * 1024};
        }

        SceneCellPayloadLimits PayloadLimits() {
            return {32, 32, 1024 * 1024};
        }

        SceneCellPayloadIdentity Identity(std::int32_t x = 0, std::uint64_t revision = 1) {
            return {World(), Cell(x), {static_cast<std::uint64_t>(77 + x)}, {revision}};
        }

        RuntimeEntityDefinition Entity(std::uint64_t object = 1) {
            return {.object = {object}};
        }

        SceneCellCookInput Input(std::span<const RuntimeEntityDefinition> entities, std::int32_t x = 0,
                                 std::span<const SceneAssetDependency> dependencies = {}) {
            return {{Identity(x), entities, dependencies}, Identity(x)};
        }

        void Equivalent(const RuntimeSceneCellPayload &a, const RuntimeSceneCellPayload &b) {
            REQUIRE(a.Identity() == b.Identity());
            REQUIRE(a.RetainedBytes() == b.RetainedBytes());
            REQUIRE(std::ranges::equal(a.Definition().AssetDependencies(), b.Definition().AssetDependencies()));
            REQUIRE(a.Definition().Entities().size() == b.Definition().Entities().size());
            for (std::size_t i = 0; i < a.Definition().Entities().size(); ++i) {
                const auto &left = a.Definition().Entities()[i];
                const auto &right = b.Definition().Entities()[i];
                CHECK(left.object == right.object);
                CHECK(left.parent == right.parent);
                CHECK(left.localTransform == right.localTransform);
                CHECK(left.primitiveMesh == right.primitiveMesh);
                CHECK(left.components == right.components);
            }
        }

        void ErrorIs(const Result<SceneCellCookReport> &result, const ErrorCodeDescriptor &code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code == code.code);
        }
    }  // namespace

    TEST_CASE("Incremental cells reuse complete immutable baselines and cook only changed source", "[scene][incremental_cell_cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        IncrementalSceneCellCook cache{CacheLimits()};
        std::array first{Entity(), Entity(2)};
        first[1].parent = SceneObjectId{1};
        first[0].components.camera = CameraComponent{};
        first[1].primitiveMesh = PrimitiveMeshDescriptor::Defaults(PrimitiveMeshType::Sphere);
        std::array second{Entity(3)};
        std::array inputs{Input(second, 1), Input(first)};
        auto run = [&] {
            return cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), cache.Revision(), PayloadLimits());
        };
        auto initial = run();
        REQUIRE(initial.HasValue());
        CHECK(initial.Value().cooked == 2);
        CHECK(initial.Value().reused == 0);
        CHECK(initial.Value().cells[0].payload->Identity().cell == Cell());
        auto reused = run();
        REQUIRE(reused.HasValue());
        CHECK(reused.Value().cooked == 0);
        CHECK(reused.Value().reused == 2);
        CHECK(reused.Value().cells[0].payload == initial.Value().cells[0].payload);
        // Typed content is authoritative even if the caller did not advance its source revision.
        first[0].components.camera->farPlane = 2000;
        first[1].primitiveMesh->parameters = SphereMeshParameters{.radius = 2};
        auto changed = run();
        REQUIRE(changed.HasValue());
        CHECK(changed.Value().cooked == 1);
        CHECK(changed.Value().reused == 1);
        CHECK(changed.Value().cells[1].payload == initial.Value().cells[1].payload);
        auto fresh = CookRuntimeSceneCellPayload(manifest.Descriptor(), inputs[1].source, inputs[1].expected, PayloadLimits());
        REQUIRE(fresh.HasValue());
        Equivalent(*changed.Value().cells[0].payload, fresh.Value());
        IncrementalSceneCellCook clean{CacheLimits()};
        auto cleanResult = clean.Cook(manifest.Descriptor(), inputs, {}, Hash(), 0, PayloadLimits());
        REQUIRE(cleanResult.HasValue());
        for (std::size_t i = 0; i < changed.Value().cells.size(); ++i) {
            CHECK(cleanResult.Value().cells[i].key == changed.Value().cells[i].key);
            Equivalent(*cleanResult.Value().cells[i].payload, *changed.Value().cells[i].payload);
        }
    }

    TEST_CASE("Dependency closure invalidates exactly dependent cells and canonical graph order preserves hits",
              "[scene][incremental_cell_cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        IncrementalSceneCellCook cache{CacheLimits()};
        const std::array entities{Entity()};
        const auto type = Assets::AssetTypeId::Parse("core.mesh").Value();
        const std::array firstDeps{SceneAssetDependency{Asset(1), type}};
        const std::array secondDeps{SceneAssetDependency{Asset(3), type}};
        const std::array rootEdges{Asset(2)};
        std::array catalog{SceneCellCookDependency{Asset(1), Hash(1), 1, rootEdges}, SceneCellCookDependency{Asset(2), Hash(2), 1, {}},
                           SceneCellCookDependency{Asset(3), Hash(3), 1, {}}, SceneCellCookDependency{Asset(4), Hash(4), 1, {}}};
        const std::array inputs{Input(entities, 0, firstDeps), Input(entities, 1, secondDeps), Input(entities, 2, firstDeps)};
        auto run = [&] {
            return cache.Cook(manifest.Descriptor(), inputs, catalog, Hash(), cache.Revision(), PayloadLimits());
        };
        REQUIRE(run().Value().cooked == 3);
        std::ranges::reverse(catalog);
        REQUIRE(run().Value().reused == 3);
        catalog[0].content = Hash(9);  // Unreferenced Asset(4).
        REQUIRE(run().Value().reused == 3);
        catalog[2].content = Hash(10);  // Transitive Asset(2).
        auto changed = run();
        REQUIRE(changed.HasValue());
        CHECK(changed.Value().cooked == 2);
        CHECK(changed.Value().reused == 1);
        IncrementalSceneCellCook clean{CacheLimits()};
        const auto fresh = clean.Cook(manifest.Descriptor(), inputs, catalog, Hash(), 0, PayloadLimits());
        REQUIRE(fresh.HasValue());
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(changed.Value().cells[i].key == fresh.Value().cells[i].key);
            Equivalent(*changed.Value().cells[i].payload, *fresh.Value().cells[i].payload);
        }
        catalog[2].revision = 2;
        CHECK(run().Value().cooked == 2);
        auto settings = cache.Cook(manifest.Descriptor(), inputs, catalog, Hash(99), cache.Revision(), PayloadLimits());
        REQUIRE(settings.HasValue());
        CHECK(settings.Value().cooked == 3);
    }

    TEST_CASE("Incremental cook validates graph and fences transactionally", "[scene][incremental_cell_cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        IncrementalSceneCellCook cache{CacheLimits()};
        const std::array entities{Entity()};
        std::array inputs{Input(entities)};
        auto run = [&](std::span<const SceneCellCookDependency> dependencies = {}) {
            return cache.Cook(manifest.Descriptor(), inputs, dependencies, Hash(), cache.Revision(), PayloadLimits());
        };
        auto original = run();
        REQUIRE(original.HasValue());
        const auto revision = cache.Revision();
        inputs[0].expected.revision.value = 2;
        ErrorIs(run(), SceneCellPayloadErrors::Stale);
        inputs[0].expected = Identity();
        auto invalidEntities = entities;
        invalidEntities[0].object = {};
        inputs[0].source.entities = invalidEntities;
        REQUIRE(run().HasError());
        inputs[0].source.entities = entities;
        const std::array missing{Asset(9)};
        const std::array missingGraph{SceneCellCookDependency{Asset(1), Hash(), 1, missing}};
        ErrorIs(run(missingGraph), SceneCellPayloadErrors::Invalid);
        const std::array back{Asset(1)};
        const std::array cycle{SceneCellCookDependency{Asset(1), Hash(), 1, back}};
        ErrorIs(run(cycle), SceneCellPayloadErrors::Invalid);
        const std::array duplicates{SceneCellCookDependency{Asset(1), Hash(), 1, {}}, SceneCellCookDependency{Asset(1), Hash(), 1, {}}};
        ErrorIs(run(duplicates), SceneCellPayloadErrors::Invalid);
        const std::array zeroRevision{SceneCellCookDependency{Asset(1), Hash(), 0, {}}};
        ErrorIs(run(zeroRevision), SceneCellPayloadErrors::Invalid);
        CHECK(cache.Revision() == revision);
        auto recovered = run();
        REQUIRE(recovered.HasValue());
        CHECK(recovered.Value().reused == 1);
        CHECK(recovered.Value().cells[0].payload == original.Value().cells[0].payload);
        ErrorIs(cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), revision, PayloadLimits()), SceneCellPayloadErrors::Stale);
        const std::array duplicateCells{inputs[0], inputs[0]};
        ErrorIs(cache.Cook(manifest.Descriptor(), duplicateCells, {}, Hash(), cache.Revision(), PayloadLimits()),
                SceneCellPayloadErrors::Invalid);
    }

    TEST_CASE("Incremental hits recheck ceilings and unsupported source changes preserve output", "[scene][incremental_cell_cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        IncrementalSceneCellCook cache{CacheLimits()};
        std::array entities{Entity(), Entity(2)};
        std::array inputs{Input(entities)};
        auto run = [&](SceneCellPayloadLimits limits) {
            return cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), cache.Revision(), limits);
        };
        auto original = run(PayloadLimits());
        REQUIRE(original.HasValue());
        const auto revision = cache.Revision();
        auto limits = PayloadLimits();
        limits.maximumEntities = 1;
        ErrorIs(run(limits), SceneCellPayloadErrors::CapacityExceeded);
        limits = PayloadLimits();
        limits.maximumRetainedBytes = original.Value().cells[0].payload->RetainedBytes() - 1;
        ErrorIs(run(limits), SceneCellPayloadErrors::CapacityExceeded);
        limits.maximumRetainedBytes += 1;
        REQUIRE(run(limits).Value().reused == 1);
        entities[0].components.camera = CameraComponent{.projection = static_cast<CameraProjection>(99)};
        ErrorIs(run(PayloadLimits()), SceneCellPayloadErrors::Unsupported);
        entities[0].components.camera.reset();
        REQUIRE(run(PayloadLimits()).Value().reused == 1);
        CHECK(cache.Revision() == revision + 2);
        auto tiny = CacheLimits();
        tiny.maximumKeyBytes = 1;
        IncrementalSceneCellCook tinyCache{tiny};
        ErrorIs(tinyCache.Cook(manifest.Descriptor(), inputs, {}, Hash(), 0, PayloadLimits()), SceneCellPayloadErrors::CapacityExceeded);
        tiny = CacheLimits();
        tiny.maximumRetainedBytes = 1;
        IncrementalSceneCellCook noStorage{tiny};
        ErrorIs(noStorage.Cook(manifest.Descriptor(), inputs, {}, Hash(), 0, PayloadLimits()), SceneCellPayloadErrors::CapacityExceeded);
        CHECK(noStorage.Revision() == 0);
    }

    TEST_CASE("Replacement removal cancellation and shutdown preserve leased cell ownership", "[scene][incremental_cell_cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        IncrementalSceneCellCook cache{CacheLimits()};
        const std::array entities{Entity()};
        const std::array inputs{Input(entities), Input(entities, 1)};
        auto first = cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), 0, PayloadLimits());
        REQUIRE(first.HasValue());
        const auto lease = first.Value().cells[0].payload;
        CancellationSource cancel;
        cancel.RequestCancellation();
        ErrorIs(cache.Cook(manifest.Descriptor(), {}, {}, Hash(), 1, PayloadLimits(), cancel.Token()), SceneCellPayloadErrors::Cancelled);
        CHECK(cache.Revision() == 1);
        REQUIRE(cache.Cook(manifest.Descriptor(), std::span{inputs}.subspan(1), {}, Hash(), 1, PayloadLimits()).Value().reused == 1);
        auto readd = cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), 2, PayloadLimits());
        REQUIRE(readd.HasValue());
        CHECK(readd.Value().cooked == 1);
        CHECK(readd.Value().reused == 1);
        REQUIRE(cache.Cook(manifest.Descriptor(), {}, {}, Hash(), 3, PayloadLimits()).Value().cells.empty());
        cache.Shutdown();
        cache.Shutdown();
        ErrorIs(cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), 4, PayloadLimits()), SceneCellPayloadErrors::Invalid);
        CHECK(lease->Definition().Entities()[0].object == SceneObjectId{1});
        CHECK(lease->Identity() == Identity());
    }

    TEST_CASE("Opaque component payloads schemas and authored revisions participate in actual cell keys",
              "[scene][incremental_cell_cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        IncrementalSceneCellCook cache{CacheLimits()};
        std::array entities{Entity()};
        const auto type = Gameplay::ComponentTypeId::Parse("game.test.inventory").Value();
        entities[0].components.gameplayComponents.push_back(
            {type, 1, Gameplay::ComponentPayloadEncoding::CanonicalJson, {std::byte{'{'}, std::byte{'}'}}});
        std::array schemas{SceneCellComponentSchema{type, 1}};
        std::array inputs{Input(entities)};
        inputs[0].source.componentSchemas = schemas;
        auto run = [&] {
            return cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), cache.Revision(), PayloadLimits());
        };
        REQUIRE(run().Value().cooked == 1);
        REQUIRE(run().Value().reused == 1);
        entities[0].components.gameplayComponents[0].payload = {std::byte{'['}, std::byte{']'}};
        auto changed = run();
        REQUIRE(changed.HasValue());
        CHECK(changed.Value().cooked == 1);
        auto fresh = CookRuntimeSceneCellPayload(manifest.Descriptor(), inputs[0].source, inputs[0].expected, PayloadLimits());
        REQUIRE(fresh.HasValue());
        Equivalent(*changed.Value().cells[0].payload, fresh.Value());
        schemas[0].version = 2;
        ErrorIs(run(), SceneCellPayloadErrors::Unsupported);
        entities[0].components.gameplayComponents[0].schemaVersion = 2;
        REQUIRE(run().Value().cooked == 1);
        inputs[0].source.identity.revision.value = 2;
        inputs[0].expected = inputs[0].source.identity;
        REQUIRE(run().Value().cooked == 1);
    }

    TEST_CASE("Later cell failure discards already cooked candidates and retains previous cache", "[scene][incremental_cell_cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        IncrementalSceneCellCook cache{CacheLimits()};
        std::array first{Entity()};
        std::array second{Entity(2)};
        const std::array inputs{Input(first), Input(second, 1)};
        REQUIRE(cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), 0, PayloadLimits()).HasValue());
        first[0].localTransform.translation.x = 42;
        second[0].object = {};
        REQUIRE(cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), 1, PayloadLimits()).HasError());
        CHECK(cache.Revision() == 1);
        second[0].object = {2};
        auto recovered = cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), 1, PayloadLimits());
        REQUIRE(recovered.HasValue());
        CHECK(recovered.Value().cooked == 1);
        CHECK(recovered.Value().reused == 1);
    }

    TEST_CASE("Concurrent cancellation during substantial key work preserves cache revision", "[scene][incremental_cell_cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        auto limits = CacheLimits();
        limits.maximumKeyBytes = 64 * 1024 * 1024;
        IncrementalSceneCellCook cache{limits};
        const std::array baseline{Entity()};
        const std::array original{Input(baseline)};
        REQUIRE(cache.Cook(manifest.Descriptor(), original, {}, Hash(), 0, PayloadLimits()).HasValue());
        std::vector<RuntimeEntityDefinition> entities(10000);
        for (std::size_t i = 0; i < entities.size(); ++i)
            entities[i].object = {i + 1};
        const std::array inputs{Input(entities)};
        auto payloadLimits = PayloadLimits();
        payloadLimits.maximumEntities = entities.size();
        payloadLimits.maximumRetainedBytes = 64 * 1024 * 1024;
        CancellationSource cancel;
        std::jthread cancelling([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            cancel.RequestCancellation();
        });
        ErrorIs(cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), 1, payloadLimits, cancel.Token()), SceneCellPayloadErrors::Cancelled);
        CHECK(cache.Revision() == 1);
        REQUIRE(cache.Cook(manifest.Descriptor(), original, {}, Hash(), 1, PayloadLimits()).Value().reused == 1);
    }

    TEST_CASE("Cached baseline lease uses real deferred RuntimeScene publication after cache shutdown",
              "[scene][incremental_cell_cook][integration]") {
        class Authority final : public SceneCellPayloadAuthority {
        public:
            Result<void> ValidatePublication(const SceneCellPayloadIdentity &, const W::StreamingFence &) const override {
                return Result<void>::Success();
            }
        };

        const auto manifest = W::CandidateTestSupport::Manifest();
        IncrementalSceneCellCook cache{CacheLimits()};
        const std::array entities{Entity()};
        const std::array inputs{Input(entities)};
        REQUIRE(cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), 0, PayloadLimits()).HasValue());
        auto hit = cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), 1, PayloadLimits());
        REQUIRE(hit.HasValue());
        CHECK(hit.Value().reused == 1);
        RuntimeSceneService service;
        REQUIRE(service.Startup({}).HasValue());
        const W::StreamingFence fence{World(), W::TestSupport::IdentityFrom<W::PartitionEpoch>(1), Cell(),
                                      W::TestSupport::IdentityFrom<W::StreamingGeneration>(1)};
        REQUIRE(QueueRuntimeSceneCellPayload(service, *hit.Value().cells[0].payload, fence, std::make_shared<Authority>()).HasValue());
        hit = Result<SceneCellCookReport>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
        cache.Shutdown();
        REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, FrameContext{1, {}, 0.0, 0, {}, false, {}}).HasValue());
        REQUIRE(service.ActiveScene().has_value());
        CHECK_FALSE(service.TakeOperationError().has_value());
        service.Shutdown();
    }

    TEST_CASE("Projected navigation artifacts invalidate cells and cached dependency ceilings remain strict",
              "[scene][incremental_cell_cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        IncrementalSceneCellCook cache{CacheLimits()};
        std::array entities{Entity()};
        entities[0].components.navigationSurface =
            NavigationSurfaceComponent{.id = Navigation::SurfaceId::Create(1).Value(),
                                       .definition = Asset(10),
                                       .profiles = {Navigation::NavigationAgentProfileId::Create(7).Value()}};
        const std::array declared{SceneAssetDependency{Asset(1), Assets::AssetTypeId::Parse("core.mesh").Value()}};
        const std::array inputs{Input(entities, 0, declared)};
        std::array catalog{SceneCellCookDependency{Asset(10), Hash(10), 1, {}}, SceneCellCookDependency{Asset(1), Hash(1), 1, {}}};
        auto run = [&](SceneCellPayloadLimits limits) {
            return cache.Cook(manifest.Descriptor(), inputs, catalog, Hash(), cache.Revision(), limits);
        };
        auto first = run(PayloadLimits());
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().cells[0].payload->Definition().AssetDependencies().size() == 2);
        auto tight = PayloadLimits();
        tight.maximumDependencies = 1;
        ErrorIs(run(tight), SceneCellPayloadErrors::CapacityExceeded);
        CHECK(cache.Revision() == 1);
        REQUIRE(run(PayloadLimits()).Value().reused == 1);
        catalog[0].content = Hash(99);
        auto changed = run(PayloadLimits());
        REQUIRE(changed.HasValue());
        CHECK(changed.Value().cooked == 1);
        auto fresh = CookRuntimeSceneCellPayload(manifest.Descriptor(), inputs[0].source, inputs[0].expected, PayloadLimits());
        REQUIRE(fresh.HasValue());
        Equivalent(*changed.Value().cells[0].payload, fresh.Value());
    }

    TEST_CASE("Empty cell canonical content key has a portable independent SHA256 fixture", "[scene][incremental_cell_cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        IncrementalSceneCellCook cache{CacheLimits()};
        const std::array inputs{Input({})};
        const auto result = cache.Cook(manifest.Descriptor(), inputs, {}, Hash(), 0, PayloadLimits());
        REQUIRE(result.HasValue());
        CHECK(FormatSha256(result.Value().cells[0].key) == "sha256:ca70360beeb7e1f40bdcec50c45a7566cd6aeb71184072186d5f51340ae07bf5");
        REQUIRE(result.Value().cells[0].payload->Definition().Entities().empty());
    }

    TEST_CASE("Aggregate input graph and schema ceilings reject without publication", "[scene][incremental_cell_cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        const std::array entities{Entity()};
        const std::array inputs{Input(entities), Input(entities, 1)};
        auto limits = CacheLimits();
        limits.maximumCells = 1;
        IncrementalSceneCellCook cellBound{limits};
        ErrorIs(cellBound.Cook(manifest.Descriptor(), inputs, {}, Hash(), 0, PayloadLimits()), SceneCellPayloadErrors::CapacityExceeded);
        limits = CacheLimits();
        limits.maximumDependencies = 1;
        IncrementalSceneCellCook nodeBound{limits};
        const std::array catalog{SceneCellCookDependency{Asset(1), Hash(), 1, {}}, SceneCellCookDependency{Asset(2), Hash(), 1, {}}};
        ErrorIs(nodeBound.Cook(manifest.Descriptor(), inputs, catalog, Hash(), 0, PayloadLimits()),
                SceneCellPayloadErrors::CapacityExceeded);
        limits = CacheLimits();
        limits.maximumDependencyEdges = 1;
        IncrementalSceneCellCook edgeBound{limits};
        const std::array edges{Asset(1), Asset(2)};
        const std::array edgeCatalog{SceneCellCookDependency{Asset(3), Hash(), 1, edges}};
        ErrorIs(edgeBound.Cook(manifest.Descriptor(), inputs, edgeCatalog, Hash(), 0, PayloadLimits()),
                SceneCellPayloadErrors::CapacityExceeded);
        limits = CacheLimits();
        limits.maximumSchemas = 1;
        IncrementalSceneCellCook schemaBound{limits};
        const auto type = Gameplay::ComponentTypeId::Parse("game.test.inventory").Value();
        const std::array schemas{SceneCellComponentSchema{type, 1}, SceneCellComponentSchema{type, 2}};
        auto schemaInput = inputs;
        schemaInput[0].source.componentSchemas = schemas;
        ErrorIs(schemaBound.Cook(manifest.Descriptor(), schemaInput, {}, Hash(), 0, PayloadLimits()),
                SceneCellPayloadErrors::CapacityExceeded);
        CHECK(schemaBound.Revision() == 0);
        IncrementalSceneCellCook invalid{{}};
        ErrorIs(invalid.Cook(manifest.Descriptor(), inputs, {}, Hash(), 0, PayloadLimits()), SceneCellPayloadErrors::Invalid);
    }

}  // namespace Horo::Runtime
