#include "../world_streaming/WorldStreamingTestUtils.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellLayers.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <thread>

namespace Horo::Runtime {
    namespace {
        namespace W = WorldStreaming;
        using W::TestSupport::Asset;
        using W::TestSupport::IdentityFrom;
        using W::TestSupport::Layer;
        using W::TestSupport::World;
        using W::TestSupport::WorldOwner;

        W::StreamingCellId Cell() {
            return {0, 0, 0, 0, Layer(0)};
        }

        SceneCellPayloadIdentity Identity() {
            return {World(), Cell(), {77}, {1}};
        }

        W::StreamingFence Fence() {
            return {World(), WorldOwner().epoch, Cell(), IdentityFrom<W::StreamingGeneration>(1)};
        }

        SceneCellLayerLimits Limits() {
            return {8, 16, 1024 * 1024};
        }

        W::WorldLayerFilterPolicy Policy() {
            return {IdentityFrom<W::WorldLayerFilterPolicyId>(1), IdentityFrom<W::WorldLayerFilterPolicyRevision>(1),
                    W::WorldLayerExecutionTarget::ClientRuntime, W::WorldLayerOptionalPolicy::Include};
        }

        W::WorldLayerFilterContext Context() {
            return {WorldOwner(), Policy().id, Policy().revision, 8, W::WorldLayerFilterAuthorityState::Active};
        }

        W::WorldPartitionDescriptor Partition() {
            const auto grid = W::WorldCellQuantizationPolicy::Create({}, 100, {-1, 1, -1, 1, -1, 1}, 1).Value();
            const std::array layers{W::WorldLayerDescriptor{Layer(0), "base", W::WorldLayerOwnership::WorldStreaming,
                                                            W::WorldLayerFlags::Persistent, 1},
                                    W::WorldLayerDescriptor{Layer(1), "server", W::WorldLayerOwnership::WorldStreaming,
                                                            W::WorldLayerFlags::ServerOnly, 1},
                                    W::WorldLayerDescriptor{Layer(2), "optional", W::WorldLayerOwnership::WorldStreaming,
                                                            W::WorldLayerFlags::Optional, 1}};
            const std::array cells{W::WorldPartitionCellDescriptor{Cell(), {Asset(4)}}};
            return W::WorldPartitionDescriptor::Create({}, World(),
                                                       {Math::WorldCoordinate64::FromMillimeters(-100, -100, -100),
                                                        Math::WorldCoordinate64::FromMillimeters(199, 199, 199)},
                                                       grid, layers, cells, {8, 8, 128})
                .Value();
        }

        std::array<SceneCellDataLayer, 3> Layers() {
            return {{{Layer(0), IdentityFrom<W::WorldLayerRevision>(1), W::WorldLayerFlags::Persistent},
                     {Layer(1), IdentityFrom<W::WorldLayerRevision>(1), W::WorldLayerFlags::ServerOnly},
                     {Layer(2), IdentityFrom<W::WorldLayerRevision>(1), W::WorldLayerFlags::Optional}}};
        }

        std::array<W::WorldLayerFilterCandidate, 3> Candidates() {
            std::array<W::WorldLayerFilterCandidate, 3> result{};
            const auto layers = Layers();
            for (std::size_t index = 0; index < result.size(); ++index) {
                result[index] = {{layers[index].layer, layers[index].ownershipRevision, W::WorldLayerPlacement::Spatial,
                                  index == 0 ? W::WorldLayerResidencyPolicy::Persistent : W::WorldLayerResidencyPolicy::Streamed,
                                  W::WorldLayerAudience::Runtime, W::TestSupport::StreamingLayerOwner()},
                                 layers[index].flags};
            }
            return result;
        }

        std::array<W::WorldLayerStateRecord, 3> States() {
            std::array<W::WorldLayerStateRecord, 3> result{};
            const auto candidates = Candidates();
            for (std::size_t index = 0; index < result.size(); ++index)
                result[index] = {candidates[index].ownership, IdentityFrom<W::WorldLayerStateRevision>(5), W::WorldLayerState::Activated};
            return result;
        }

        RuntimeSceneCellPayload Baseline(const std::optional<SceneObjectId> parent = {},
                                         const std::span<const SceneAssetDependency> dependencies = {}) {
            const auto partition = Partition();
            const std::array entities{RuntimeEntityDefinition{.object = {3}}, RuntimeEntityDefinition{.object = {1}},
                                      RuntimeEntityDefinition{.object = {2}, .parent = parent}};
            auto cooked = CookRuntimeSceneCellPayload(partition, {Identity(), entities, dependencies}, Identity(), {8, 8, 1024 * 1024});
            REQUIRE(cooked.HasValue());
            return std::move(cooked).Value();
        }

        std::array<SceneCellLayerMembership, 3> Edges() {
            return {{{{1}, Layer(1)}, {{1}, Layer(2)}, {{2}, Layer(0)}}};
        }

        RuntimeSceneCellLayers Encoded(const std::optional<SceneObjectId> parent = {},
                                       const std::span<const SceneAssetDependency> dependencies = {}) {
            const auto partition = Partition();
            auto result = EncodeRuntimeSceneCellLayers(partition, Baseline(parent, dependencies), Identity(), Layers(), Edges(), Limits());
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        void Commit(RuntimeSceneService &service) {
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, FrameContext{1, {}, 0.0, 0, {}, false, {}}).HasValue());
        }

        class Authority final : public SceneCellLayerAuthority {
        public:
            SceneCellLayerSelectionEvidence current{};
            W::StreamingFence fence{Fence()};
            bool active{true};

            Result<void> ValidatePublication(const SceneCellLayerSelectionEvidence &evidence,
                                             const W::StreamingFence &attempt) const override {
                if (!active)
                    return Result<void>::Failure(MakeError(W::WorldStreamingErrors::LayerFilterLifecycleUnavailable));
                if (evidence.identity != current.identity || evidence.world != current.world || evidence.policy != current.policy ||
                    evidence.states != current.states || attempt != fence)
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Stale));
                return Result<void>::Success();
            }
        };
    }  // namespace

    TEST_CASE("Data layer encoding transfers one baseline and owns only compact canonical membership", "[scene][cell_layers][cook]") {
        auto baseline = Baseline();
        const auto *original = baseline.Definition().Entities().data();
        const auto bytes = baseline.RetainedBytes();
        const auto partition = Partition();
        auto layers = Layers();
        auto edges = Edges();
        auto result = EncodeRuntimeSceneCellLayers(partition, std::move(baseline), Identity(), layers, edges, Limits());
        REQUIRE(result.HasValue());
        auto encoded = std::move(result).Value();
        layers[0].ownershipRevision = {};
        edges[0].object = {};
        REQUIRE(encoded.Baseline().Definition().Entities().data() == original);
        REQUIRE(encoded.Baseline().Definition().Entities().size() == 3);
        REQUIRE(encoded.Layers()[0].layer == Layer(0));
        REQUIRE(encoded.Memberships()[0].object == SceneObjectId{1});
        REQUIRE(encoded.RetainedBytes() == bytes + 3 * sizeof(SceneCellDataLayer) + 3 * sizeof(SceneCellLayerMembership));
        auto moved = std::move(encoded);
        REQUIRE_FALSE(encoded.IsUsable());
        REQUIRE(FilterRuntimeSceneCellLayers(encoded, Policy(), Candidates(), States(), Context()).HasError());
        REQUIRE(moved.IsUsable());
    }

    TEST_CASE("Data layer encoding rejects invalid stale and overcapacity metadata without consuming baseline",
              "[scene][cell_layers][failure]") {
        const auto partition = Partition();
        auto baseline = Baseline();
        const auto *original = baseline.Definition().Entities().data();
        auto layers = Layers();
        auto edges = Edges();
        auto limits = Limits();
        auto expected = Identity();
        auto encode = [&] {
            return EncodeRuntimeSceneCellLayers(partition, std::move(baseline), expected, layers, edges, limits);
        };
        SECTION("stale source") {
            expected.revision.value = 2;
            REQUIRE(encode().ErrorValue().code.Value() == "scene.cell_payload.stale");
        }
        SECTION("stale flags") {
            layers[1].flags = W::WorldLayerFlags::None;
            REQUIRE(encode().HasError());
        }
        SECTION("missing layer") {
            layers[2].layer = Layer(9);
            REQUIRE(encode().HasError());
        }
        SECTION("invalid revision") {
            layers[1].ownershipRevision = {};
            REQUIRE(encode().HasError());
        }
        SECTION("duplicate layers") {
            layers[1] = layers[0];
            REQUIRE(encode().HasError());
        }
        SECTION("duplicate edges") {
            edges[1] = edges[0];
            REQUIRE(encode().HasError());
        }
        SECTION("unknown object") {
            edges[2].object = {99};
            REQUIRE(encode().HasError());
        }
        SECTION("unknown membership layer") {
            edges[2].layer = Layer(9);
            REQUIRE(encode().HasError());
        }
        SECTION("unsorted edges") {
            std::swap(edges[0], edges[2]);
            REQUIRE(encode().HasError());
        }
        SECTION("zero ceiling") {
            limits.maximumLayers = 0;
            REQUIRE(encode().HasError());
        }
        SECTION("layer ceiling") {
            limits.maximumLayers = 2;
            REQUIRE(encode().HasError());
        }
        SECTION("edge ceiling") {
            limits.maximumMemberships = 2;
            REQUIRE(encode().HasError());
        }
        SECTION("byte ceiling") {
            limits.maximumRetainedBytes = baseline.RetainedBytes();
            REQUIRE(encode().HasError());
        }
        SECTION("extreme ceiling") {
            limits.maximumRetainedBytes = std::numeric_limits<std::size_t>::max();
            REQUIRE(encode().HasValue());
            return;
        }
        REQUIRE(baseline.Definition().Entities().data() == original);
        REQUIRE(baseline.Definition().Entities().size() == 3);
    }

    TEST_CASE("Layer selection uses union membership activated state and target policy in authored order", "[scene][cell_layers][filter]") {
        const auto encoded = Encoded();
        auto states = States();
        auto policy = Policy();
        auto select = [&] {
            return FilterRuntimeSceneCellLayers(encoded, policy, Candidates(), states, Context());
        };
        REQUIRE(select().Value().Definition().Entities().size() == 3);
        states[2].state = W::WorldLayerState::Loaded;
        auto selected = select();
        REQUIRE(selected.HasValue());
        REQUIRE(selected.Value().Definition().Entities().size() == 2);
        REQUIRE(selected.Value().Definition().Entities()[0].object == SceneObjectId{3});
        REQUIRE(selected.Value().Definition().Entities()[1].object == SceneObjectId{2});
        REQUIRE(selected.Value().Evidence().states.size() == 3);
        policy.target = W::WorldLayerExecutionTarget::DedicatedServerRuntime;
        REQUIRE(select().Value().Definition().Entities().size() == 3);
        policy.target = W::WorldLayerExecutionTarget::Editor;
        REQUIRE(select().Value().Definition().Entities().size() == 3);
        policy = Policy();
        states = States();
        policy.optional = W::WorldLayerOptionalPolicy::Exclude;
        REQUIRE(select().Value().Definition().Entities().size() == 2);
        states[0].state = W::WorldLayerState::Failed;
        REQUIRE(select().Value().Definition().Entities().size() == 1);
        states[0].state = W::WorldLayerState::Unloaded;
        REQUIRE(select().Value().Definition().Entities().size() == 1);
    }

    TEST_CASE("Layer filtering rejects incomplete unsupported stale cancelled and closed snapshots", "[scene][cell_layers][failure]") {
        const auto encoded = Encoded();
        auto candidates = Candidates();
        auto states = States();
        auto policy = Policy();
        auto context = Context();
        auto filter = [&] {
            return FilterRuntimeSceneCellLayers(encoded, policy, candidates, states, context);
        };
        SECTION("ownership replacement") {
            candidates[0].ownership.revision = IdentityFrom<W::WorldLayerRevision>(2);
            REQUIRE(filter().HasError());
        }
        SECTION("state ownership mismatch") {
            states[0].ownership.owner.world.epoch = IdentityFrom<W::PartitionEpoch>(2);
            REQUIRE(filter().HasError());
        }
        SECTION("world replacement") {
            context.expectedWorld.epoch = IdentityFrom<W::PartitionEpoch>(2);
            REQUIRE(filter().HasError());
        }
        SECTION("policy replacement") {
            context.expectedPolicyRevision = IdentityFrom<W::WorldLayerFilterPolicyRevision>(2);
            REQUIRE(filter().HasError());
        }
        SECTION("unsupported target") {
            policy.target = static_cast<W::WorldLayerExecutionTarget>(99);
            REQUIRE(filter().HasError());
        }
        SECTION("unknown state") {
            states[0].state = static_cast<W::WorldLayerState>(99);
            REQUIRE(filter().HasError());
        }
        SECTION("capacity") {
            context.maximumCandidates = 2;
            REQUIRE(filter().HasError());
        }
        SECTION("cancelling") {
            context.authorityState = W::WorldLayerFilterAuthorityState::Cancelling;
            REQUIRE(filter().HasError());
        }
        SECTION("closed") {
            context.authorityState = W::WorldLayerFilterAuthorityState::Closed;
            REQUIRE(filter().HasError());
        }
        SECTION("missing state") {
            REQUIRE(FilterRuntimeSceneCellLayers(encoded, policy, candidates, {}, context).HasError());
        }
        SECTION("cancelled") {
            CancellationSource cancel;
            cancel.RequestCancellation();
            REQUIRE(FilterRuntimeSceneCellLayers(encoded, policy, candidates, states, context, cancel.Token()).ErrorValue().code.Value() ==
                    "scene.cell_payload.cancelled");
        }
        REQUIRE(encoded.Baseline().Definition().Entities().size() == 3);
    }

    TEST_CASE("Layer selection rejects an excluded required parent instead of silently changing transforms",
              "[scene][cell_layers][hierarchy]") {
        const auto encoded = Encoded(SceneObjectId{1});
        auto states = States();
        states[2].state = W::WorldLayerState::Loaded;
        const auto result = FilterRuntimeSceneCellLayers(encoded, Policy(), Candidates(), states, Context());
        REQUIRE(result.ErrorValue().code.Value() == "scene.hierarchy.parent_not_found");
        REQUIRE(encoded.Baseline().Definition().Entities()[2].parent == SceneObjectId{1});
    }

    TEST_CASE("Layer encoding handles cancellation and an entity-empty cell without fabricated content", "[scene][cell_layers][boundary]") {
        const auto partition = Partition();
        auto baseline = Baseline();
        CancellationSource cancel;
        cancel.RequestCancellation();
        REQUIRE(EncodeRuntimeSceneCellLayers(partition, std::move(baseline), Identity(), Layers(), Edges(), Limits(), cancel.Token())
                    .HasError());
        REQUIRE(baseline.Definition().Entities().size() == 3);
        auto empty = CookRuntimeSceneCellPayload(partition, {Identity(), {}}, Identity(), {8, 8, 1024});
        REQUIRE(empty.HasValue());
        auto encoded = EncodeRuntimeSceneCellLayers(partition, std::move(empty).Value(), Identity(), {}, {}, Limits());
        REQUIRE(encoded.HasValue());
        const auto selected = FilterRuntimeSceneCellLayers(encoded.Value(), Policy(), {}, {}, Context());
        REQUIRE(selected.HasValue());
        REQUIRE(selected.Value().Definition().Entities().empty());
    }

    TEST_CASE("Layer-selected payload enters actual Scene runtime and stale replacement preserves active entities",
              "[scene][cell_layers][integration]") {
        RuntimeSceneService service;
        REQUIRE(service.Startup({}).HasValue());
        auto authority = std::make_shared<Authority>();
        {
            const auto encoded = Encoded();
            auto states = States();
            states[2].state = W::WorldLayerState::Loaded;
            const auto selected = FilterRuntimeSceneCellLayers(encoded, Policy(), Candidates(), states, Context());
            REQUIRE(selected.HasValue());
            authority->current = selected.Value().Evidence();
            REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), authority).HasValue());
        }
        Commit(service);
        REQUIRE_FALSE(service.TakeOperationError().has_value());
        REQUIRE(service.ActiveScene()->Find(SceneObjectId{3}).has_value());
        REQUIRE_FALSE(service.ActiveScene()->Find(SceneObjectId{1}).has_value());
        const auto original = service.ActiveScene()->Find(SceneObjectId{2}).value();
        const auto encoded = Encoded();
        const auto selected = FilterRuntimeSceneCellLayers(encoded, Policy(), Candidates(), States(), Context());
        REQUIRE(selected.HasValue());
        authority->current = selected.Value().Evidence();
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), authority).HasValue());
        authority->current.states[2].stateRevision = IdentityFrom<W::WorldLayerStateRevision>(6);
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.stale");
        REQUIRE(service.ActiveScene()->Get(original).HasValue());
        authority->current = selected.Value().Evidence();
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), authority).HasValue());
        Commit(service);
        REQUIRE(service.ActiveScene()->Find(SceneObjectId{1}).has_value());
        REQUIRE(service.ActiveScene()->Get(original).HasError());
    }

    TEST_CASE("Layer publication owns its lease and observes policy replacement cancellation unload and shutdown",
              "[scene][cell_layers][lifecycle]") {
        const auto encoded = Encoded();
        const auto selected = FilterRuntimeSceneCellLayers(encoded, Policy(), Candidates(), States(), Context());
        REQUIRE(selected.HasValue());
        RuntimeSceneService service;
        REQUIRE(service.Startup({}).HasValue());
        auto authority = std::make_shared<Authority>();
        authority->current = selected.Value().Evidence();
        std::weak_ptr<Authority> lease = authority;
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), {}, authority).HasError());
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), {}).HasError());
        auto fence = Fence();
        fence.epoch = IdentityFrom<W::PartitionEpoch>(2);
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), fence, authority).HasError());
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), authority).HasValue());
        authority->current.policy.revision = IdentityFrom<W::WorldLayerFilterPolicyRevision>(2);
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.stale");
        REQUIRE_FALSE(service.ActiveScene().has_value());
        authority->current = selected.Value().Evidence();
        CancellationSource cancel;
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), authority, cancel.Token()).HasValue());
        cancel.RequestCancellation();
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.cancelled");
        authority->active = false;
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), authority).HasError());
        authority->active = true;
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), authority).HasValue());
        authority.reset();
        REQUIRE_FALSE(lease.expired());
        REQUIRE(service.QueueUnload().HasValue());
        REQUIRE(lease.expired());
        Commit(service);
        authority = std::make_shared<Authority>();
        authority->current = selected.Value().Evidence();
        lease = authority;
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), authority).HasValue());
        authority.reset();
        service.Shutdown();
        REQUIRE(lease.expired());
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), std::make_shared<Authority>()).HasError());
    }

    TEST_CASE("Moved layer selections cannot publish a fabricated empty Scene", "[scene][cell_layers][ownership]") {
        const auto encoded = Encoded();
        auto result = FilterRuntimeSceneCellLayers(encoded, Policy(), Candidates(), States(), Context());
        REQUIRE(result.HasValue());
        auto moved = std::move(result).Value();
        REQUIRE_FALSE(result.Value().IsUsable());
        RuntimeSceneService service;
        REQUIRE(service.Startup({}).HasValue());
        auto authority = std::make_shared<Authority>();
        authority->current = moved.Evidence();
        REQUIRE(QueueRuntimeSceneCellLayers(service, result.Value(), Fence(), authority).HasError());
        REQUIRE(QueueRuntimeSceneCellLayers(service, moved, Fence(), authority).HasValue());
        Commit(service);
        REQUIRE(service.ActiveScene()->Find(SceneObjectId{1}).has_value());
    }

    TEST_CASE("Layer-selected Scene uses actual asynchronous assets and preserves active Scene after provider failure",
              "[scene][cell_layers][assets]") {
        const SceneAssetDependency dependency{Asset(9), Assets::AssetTypeId::Parse("core.mesh").Value()};
        const std::array dependencies{dependency};
        const auto encoded = Encoded({}, dependencies);
        const auto selected = FilterRuntimeSceneCellLayers(encoded, Policy(), Candidates(), States(), Context());
        REQUIRE(selected.HasValue());
        REQUIRE(selected.Value().Definition().AssetDependencies().size() == 1);
        Assets::AssetRegistry registry;
        REQUIRE(registry
                    .Publish(std::vector<Assets::AssetRecord>{{dependency.id, dependency.expectedType,
                                                               ProjectPath::Parse("assets/mesh.bin").Value(),
                                                               ProjectPath::Parse("assets/mesh.bin.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
        Assets::MemoryAssetProvider provider;
        provider.Insert(dependency.id, {1, 2, 3});
        JobSystem jobs{JobSystemConfig{1, 8}};
        Assets::AssetLoadService loads{jobs, provider};
        RuntimeSceneService service{registry, loads};
        REQUIRE(service.Startup({}).HasValue());
        auto authority = std::make_shared<Authority>();
        authority->current = selected.Value().Evidence();
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), authority).HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!service.ActiveScene() && std::chrono::steady_clock::now() < deadline) {
            Commit(service);
            std::this_thread::yield();
        }
        REQUIRE(service.ActiveScene().has_value());
        REQUIRE(service.ActiveScene()->FindAsset(dependency.id)->bytes.size() == 3);
        const auto original = service.ActiveScene()->Find(SceneObjectId{1}).value();
        provider.Remove(dependency.id);
        REQUIRE(registry
                    .Publish(std::vector<Assets::AssetRecord>{{dependency.id, dependency.expectedType,
                                                               ProjectPath::Parse("assets/mesh.bin").Value(),
                                                               ProjectPath::Parse("assets/mesh.bin.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
        REQUIRE(QueueRuntimeSceneCellLayers(service, selected.Value(), Fence(), authority).HasValue());
        std::optional<Error> failure;
        const auto failDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!failure && std::chrono::steady_clock::now() < failDeadline) {
            Commit(service);
            failure = service.TakeOperationError();
            std::this_thread::yield();
        }
        REQUIRE(failure.has_value());
        REQUIRE(failure->code.Value() == "asset.provider.not_found");
        REQUIRE(service.ActiveScene()->Get(original).HasValue());
        service.Shutdown();
        loads.Shutdown();
        jobs.Shutdown(ShutdownPolicy::Drain);
    }
}  // namespace Horo::Runtime
