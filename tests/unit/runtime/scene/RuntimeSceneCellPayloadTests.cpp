#include "../world_streaming/StreamingCellCandidateTestSupport.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Runtime/Scene/AudioSceneExtraction.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <thread>

namespace Horo::Runtime {
    namespace {
        namespace W = WorldStreaming;
        using W::CandidateTestSupport::Cell;
        using W::TestSupport::Asset;
        using W::TestSupport::IdentityFrom;
        using W::TestSupport::World;

        SceneCellPayloadIdentity Identity(const std::uint64_t revision = 1) {
            return {World(), Cell(), {77}, {revision}};
        }

        SceneCellPayloadLimits Limits() {
            return {16, 16, 1024 * 1024};
        }

        W::StreamingFence Fence() {
            return {World(), IdentityFrom<W::PartitionEpoch>(1), Cell(), IdentityFrom<W::StreamingGeneration>(1)};
        }

        RuntimeEntityDefinition Entity(const std::uint64_t object, const std::optional<SceneObjectId> parent = {}) {
            return {.object = {object}, .parent = parent};
        }

        RuntimeSceneCellPayload Payload(const std::uint64_t revision = 1) {
            const auto manifest = W::CandidateTestSupport::Manifest();
            const std::array entities{Entity(1), Entity(2, SceneObjectId{1})};
            auto result = CookRuntimeSceneCellPayload(manifest.Descriptor(), {Identity(revision), entities}, Identity(revision), Limits());
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        void Commit(RuntimeSceneService &service) {
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, FrameContext{1, {}, 0.0, 0, {}, false, {}}).HasValue());
        }

        void PublishMesh(Assets::AssetRegistry &registry, const SceneAssetDependency &dependency) {
            REQUIRE(registry
                        .Publish(std::vector<Assets::AssetRecord>{{dependency.id, dependency.expectedType,
                                                                   ProjectPath::Parse("assets/mesh.bin").Value(),
                                                                   ProjectPath::Parse("assets/mesh.bin.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
        }

        void AwaitActiveScene(RuntimeSceneService &service) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!service.ActiveScene() && std::chrono::steady_clock::now() < deadline) {
                Commit(service);
                REQUIRE_FALSE(service.TakeOperationError().has_value());
                std::this_thread::yield();
            }
            REQUIRE(service.ActiveScene().has_value());
        }

        Error AwaitOperationFailure(RuntimeSceneService &service) {
            std::optional<Error> failure;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!failure && std::chrono::steady_clock::now() < deadline) {
                Commit(service);
                failure = service.TakeOperationError();
                std::this_thread::yield();
            }
            REQUIRE(failure.has_value());
            return *failure;
        }

        class Authority final : public SceneCellPayloadAuthority {
        public:
            SceneCellPayloadIdentity identity{Identity()};
            W::StreamingFence fence{Fence()};
            bool ready{true};
            mutable std::size_t checks{};

            Result<void> ValidatePublication(const SceneCellPayloadIdentity &candidate, const W::StreamingFence &attempt) const override {
                ++checks;
                if (!ready || candidate != identity || attempt != fence)
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Stale));
                return Result<void>::Success();
            }
        };
    }  // namespace

    TEST_CASE("Cell baseline owns source-free hierarchy and opaque gameplay data", "[scene][cell_payload][cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        std::vector entities{Entity(2, SceneObjectId{1}), Entity(1)};
        const auto componentType = Gameplay::ComponentTypeId::Parse("game.test.inventory").Value();
        entities[0].components.gameplayComponents.push_back(
            {componentType, 3, Gameplay::ComponentPayloadEncoding::CanonicalJson, {std::byte{'{'}, std::byte{'}'}}});
        entities[1].components.camera = CameraComponent{};
        const std::array schemas{SceneCellComponentSchema{componentType, 3}};
        const std::array dependencies{SceneAssetDependency{Asset(1), Assets::AssetTypeId::Parse("core.mesh").Value()}};
        auto cooked =
            CookRuntimeSceneCellPayload(manifest.Descriptor(), {Identity(), entities, dependencies, schemas}, Identity(), Limits());
        REQUIRE(cooked.HasValue());
        auto payload = std::move(cooked).Value();
        const auto original = payload.Definition().Entities()[0].components;
        entities.clear();
        REQUIRE(payload.Identity() == Identity());
        REQUIRE(payload.Definition().Entities().size() == 2);
        REQUIRE(payload.Definition().Entities()[0].object == SceneObjectId{2});
        REQUIRE(payload.Definition().Entities()[0].parent == SceneObjectId{1});
        REQUIRE(payload.Definition().Entities()[0].components == original);
        REQUIRE(payload.Definition().Entities()[1].components.camera.has_value());
        REQUIRE(payload.Definition().AssetDependencies()[0].id == Asset(1));
        REQUIRE(payload.RetainedBytes() > 2 * sizeof(RuntimeEntityDefinition));
    }

    TEST_CASE("Cell cook rejects malformed stale and unsupported required content", "[scene][cell_payload][cook][failure]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        std::vector entities{Entity(1)};
        SceneCellPayloadSource source{Identity(), entities};
        auto cook = [&] {
            return CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits());
        };
        source.identity.revision.value = 2;
        REQUIRE(cook().ErrorValue().code.Value() == "scene.cell_payload.stale");
        source.identity = Identity();
        source.identity.revision.value = 0;
        REQUIRE(cook().ErrorValue().code.Value() == "scene.cell_payload.invalid");
        source.identity = Identity();
        source.identity.cell.x = 999;
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, source.identity, Limits()).ErrorValue().code.Value() ==
                "scene.cell_payload.invalid");
        source.identity = Identity();
        entities[0].parent = SceneObjectId{999};
        REQUIRE(cook().ErrorValue().code.Value() == "scene.hierarchy.parent_not_found");
        entities[0].parent.reset();
        entities[0].localTransform.translation.x = std::numeric_limits<float>::quiet_NaN();
        REQUIRE(cook().HasError());
        entities[0] = Entity(1);
        entities[0].components.camera = CameraComponent{.projection = static_cast<CameraProjection>(255)};
        REQUIRE(cook().ErrorValue().code.Value() == "scene.cell_payload.unsupported");
        entities[0].components.camera.reset();
        entities[0].components.light = LightComponent{.kind = static_cast<LightKind>(255)};
        REQUIRE(cook().ErrorValue().code.Value() == "scene.cell_payload.unsupported");
        entities[0].components.light.reset();
        entities[0].components.gameplayComponents.push_back({Gameplay::ComponentTypeId::Parse("game.test.required").Value(),
                                                             7,
                                                             Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                                             {std::byte{'{'}, std::byte{'}'}}});
        REQUIRE(cook().ErrorValue().code.Value() == "scene.cell_payload.unsupported");
        const std::array schemas{SceneCellComponentSchema{entities[0].components.gameplayComponents[0].typeId, 6}};
        source.componentSchemas = schemas;
        REQUIRE(cook().ErrorValue().code.Value() == "scene.cell_payload.unsupported");
    }

    TEST_CASE("Cell cook is bounded at exact storage and dependency ceilings", "[scene][cell_payload][cook][capacity]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        const std::array entities{Entity(1), Entity(2)};
        SceneCellPayloadSource source{Identity(), entities};
        const auto success = CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits());
        REQUIRE(success.HasValue());
        auto limits = Limits();
        limits.maximumEntities = 1;
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), limits).ErrorValue().code.Value() ==
                "scene.cell_payload.capacity_exceeded");
        limits = Limits();
        limits.maximumRetainedBytes = success.Value().RetainedBytes();
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), limits).HasValue());
        --limits.maximumRetainedBytes;
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), limits).ErrorValue().code.Value() ==
                "scene.cell_payload.capacity_exceeded");
        limits = Limits();
        limits.maximumEntities = 0;
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), limits).ErrorValue().code.Value() ==
                "scene.cell_payload.invalid");
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits(), cancellation.Token())
                    .ErrorValue()
                    .code.Value() == "scene.cell_payload.cancelled");
        source.entities = {};
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits()).HasValue());
    }

    TEST_CASE("Cell baseline retains authored listeners through storage admission and runtime extraction", "[scene][cell_payload][audio]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        std::vector entities{Entity(1)};
        entities[0].localTransform.translation = {3, 4, 5};
        entities[0].components.audioListener = AudioListenerComponent{2, 7, 3};
        entities[0].components.audioSource = AudioSourceComponent{};
        auto cooked = CookRuntimeSceneCellPayload(manifest.Descriptor(), {Identity(), entities}, Identity(), Limits());
        REQUIRE(cooked.HasValue());
        entities.clear();
        const auto &payload = cooked.Value();
        auto limits = Limits();
        limits.maximumRetainedBytes = payload.RetainedBytes();
        SceneCellPayloadSource retained{Identity(), payload.Definition().Entities()};
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), retained, Identity(), limits).HasValue());
        --limits.maximumRetainedBytes;
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), retained, Identity(), limits).HasError());
        auto scene = RuntimeScene::Create(payload.Definition(), {12});
        REQUIRE(scene.HasValue());
        AudioSceneExtractor extractor;
        auto frame = extractor.Capture(scene.Value()->View(), {.context = {Audio::AudioRuntimeId::Create(4).Value(), 1, 1},
                                                               .sequence = 1,
                                                               .elapsedSeconds = 0.5F,
                                                               .policy = Audio::AudioListenerPolicy::PerView});
        REQUIRE(frame.HasValue());
        REQUIRE(frame.Value().listenerCount == 1);
        REQUIRE(frame.Value().Listeners()[0].view == 2);
        REQUIRE(frame.Value().Listeners()[0].priority == 7);
        REQUIRE(frame.Value().Listeners()[0].motion.current.position == Math::Vec3{3, 4, 5});
        REQUIRE(frame.Value().sourceCount == 1);
    }

    TEST_CASE("Cell baseline deduplicates identical dependencies and preserves conflicting-type errors", "[scene][cell_payload][cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        const std::array entities{Entity(1)};
        const auto mesh = Assets::AssetTypeId::Parse("core.mesh").Value();
        std::vector dependencies{SceneAssetDependency{Asset(3), mesh}};
        SceneCellPayloadSource source{Identity(), entities, dependencies};
        const auto original = CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits());
        REQUIRE(original.HasValue());
        dependencies.push_back(dependencies.front());
        source.dependencies = dependencies;
        const auto deduplicated = CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits());
        REQUIRE(deduplicated.HasValue());
        REQUIRE(deduplicated.Value().Definition().AssetDependencies().size() == 1);
        REQUIRE(deduplicated.Value().RetainedBytes() == original.Value().RetainedBytes());
        dependencies.back().expectedType = Assets::AssetTypeId::Parse("core.texture").Value();
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits()).ErrorValue().code.Value() ==
                "scene.asset.dependency_conflict");
    }

    TEST_CASE("Cell cook accounts for builder-projected dependencies and strict behavior identities", "[scene][cell_payload][cook]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        std::vector entities{Entity(1), Entity(2)};
        entities[0].components.navigationSurface =
            NavigationSurfaceComponent{.id = Navigation::SurfaceId::Create(1).Value(),
                                       .definition = Asset(10),
                                       .profiles = {Navigation::NavigationAgentProfileId::Create(7).Value()}};
        entities[1].components.navigationSurface =
            NavigationSurfaceComponent{.id = Navigation::SurfaceId::Create(2).Value(),
                                       .definition = Asset(11),
                                       .profiles = {Navigation::NavigationAgentProfileId::Create(7).Value()}};
        SceneCellPayloadSource source{Identity(), entities};
        auto result = CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits());
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().Definition().AssetDependencies().size() == 2);
        REQUIRE(result.Value().RetainedBytes() > 2 * sizeof(RuntimeEntityDefinition));
        auto limits = Limits();
        limits.maximumDependencies = 1;
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), limits).ErrorValue().code.Value() ==
                "scene.cell_payload.capacity_exceeded");
        limits = Limits();
        limits.maximumRetainedBytes = result.Value().RetainedBytes() - 1;
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), limits).ErrorValue().code.Value() ==
                "scene.cell_payload.capacity_exceeded");
        entities = {Entity(1), Entity(2)};
        source.entities = entities;
        const auto behaviorType = Gameplay::BehaviorTypeId::Parse("game.test.move").Value();
        entities[0].components.behaviors.push_back({{11}, behaviorType, 1, true, {{"speed", 2.0}}});
        entities[1].components.behaviors.push_back(entities[0].components.behaviors.front());
        const std::array schemas{SceneCellBehaviorSchema{behaviorType, 1}};
        source.behaviorSchemas = schemas;
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits()).ErrorValue().code.Value() ==
                "scene.cell_payload.invalid");
        entities[1].components.behaviors.front().instanceId.value = 12;
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits()).HasValue());
        source.behaviorSchemas = {};
        REQUIRE(CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits()).ErrorValue().code.Value() ==
                "scene.cell_payload.unsupported");
    }

    TEST_CASE("Cell admission rejects invalid fences missing authority and stale readiness without hidden state",
              "[scene][cell_payload][admission]") {
        RuntimeSceneService service;
        REQUIRE(service.Startup({}).HasValue());
        const auto payload = Payload();
        auto authority = std::make_shared<Authority>();
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, {}, authority).ErrorValue().code.Value() == "scene.cell_payload.invalid");
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, Fence(), {}).ErrorValue().code.Value() == "scene.cell_payload.invalid");
        auto fence = Fence();
        fence.cell.x = 1;
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, fence, authority).ErrorValue().code.Value() == "scene.cell_payload.stale");
        fence = Fence();
        fence.epoch = IdentityFrom<W::PartitionEpoch>(2);
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, fence, authority).ErrorValue().code.Value() == "scene.cell_payload.stale");
        authority->ready = false;
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, Fence(), authority).ErrorValue().code.Value() == "scene.cell_payload.stale");
        REQUIRE_FALSE(service.ActiveScene().has_value());
        authority->ready = true;
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, Fence(), authority).HasValue());
        Commit(service);
        REQUIRE(service.ActiveScene().has_value());
    }

    TEST_CASE("Pending cell owns its authority lease until commit cancellation and shutdown", "[scene][cell_payload][lifetime]") {
        RuntimeSceneService service;
        REQUIRE(service.Startup({}).HasValue());
        const auto payload = Payload();
        auto authority = std::make_shared<Authority>();
        std::weak_ptr<Authority> observer = authority;
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, Fence(), authority).HasValue());
        authority.reset();
        REQUIRE_FALSE(observer.expired());
        Commit(service);
        REQUIRE(observer.expired());
        REQUIRE(service.ActiveScene().has_value());
        authority = std::make_shared<Authority>();
        observer = authority;
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, Fence(), authority).HasValue());
        authority.reset();
        REQUIRE(service.QueueUnload().HasValue());
        REQUIRE(observer.expired());
        Commit(service);
        authority = std::make_shared<Authority>();
        observer = authority;
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, Fence(), authority).HasValue());
        authority.reset();
        service.Shutdown();
        REQUIRE(observer.expired());
        REQUIRE_FALSE(service.ActiveScene().has_value());
    }

    TEST_CASE("Cell payload uses production asynchronous asset preparation and cancels unpublished work",
              "[scene][cell_payload][assets][integration]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        const std::array entities{Entity(1)};
        const std::array dependencies{SceneAssetDependency{Asset(9), Assets::AssetTypeId::Parse("core.mesh").Value()}};
        auto cooked = CookRuntimeSceneCellPayload(manifest.Descriptor(), {Identity(), entities, dependencies}, Identity(), Limits());
        REQUIRE(cooked.HasValue());
        const auto &payload = cooked.Value();
        auto authority = std::make_shared<Authority>();
        RuntimeSceneService assetless;
        REQUIRE(QueueRuntimeSceneCellPayload(assetless, payload, Fence(), authority).ErrorValue().code.Value() ==
                "scene.asset.services_unavailable");
        Assets::AssetRegistry registry;
        PublishMesh(registry, dependencies[0]);
        Assets::MemoryAssetProvider provider;
        provider.Insert(Asset(9), {1, 2, 3});
        JobSystem jobs{JobSystemConfig{1, 8}};
        Assets::AssetLoadService loads{jobs, provider};
        RuntimeSceneService service{registry, loads};
        REQUIRE(service.Startup({}).HasValue());
        CancellationSource cancellation;
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, Fence(), authority, cancellation.Token()).HasValue());
        cancellation.RequestCancellation();
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.cancelled");
        REQUIRE_FALSE(service.ActiveScene().has_value());
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, Fence(), authority).HasValue());
        AwaitActiveScene(service);
        const auto bytes = service.ActiveScene()->FindAsset(Asset(9));
        REQUIRE(bytes.has_value());
        REQUIRE(bytes->bytes.size() == 3);
        const auto original = service.ActiveScene()->Find(SceneObjectId{1}).value();
        PublishMesh(registry, dependencies[0]);
        provider.Remove(Asset(9));
        auto failedAuthority = std::make_shared<Authority>();
        std::weak_ptr<Authority> failedObserver = failedAuthority;
        REQUIRE(QueueRuntimeSceneCellPayload(service, payload, Fence(), failedAuthority).HasValue());
        failedAuthority.reset();
        REQUIRE(AwaitOperationFailure(service).code.Value() == "asset.provider.not_found");
        REQUIRE(failedObserver.expired());
        REQUIRE(service.ActiveScene()->Get(original).HasValue());
        service.Shutdown();
        loads.Shutdown();
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Cooked cell feeds deferred RuntimeScene activation and replacement", "[scene][cell_payload][integration][replacement]") {
        auto authority = std::make_shared<Authority>();
        RuntimeSceneService service;
        REQUIRE(service.Startup({}).HasValue());
        {
            const auto payload = Payload();
            REQUIRE(QueueRuntimeSceneCellPayload(service, payload, Fence(), authority).HasValue());
        }  // All source and cooked payload storage can retire before commit.
        REQUIRE_FALSE(service.ActiveScene().has_value());
        Commit(service);
        REQUIRE_FALSE(service.TakeOperationError().has_value());
        REQUIRE(service.ActiveScene()->DefinitionRevision() == SceneDefinitionRevision{1});
        const auto old = service.ActiveScene()->Find(SceneObjectId{1}).value();
        authority->identity = Identity(2);
        REQUIRE(QueueRuntimeSceneCellPayload(service, Payload(2), Fence(), authority).HasValue());
        REQUIRE(service.ActiveScene()->DefinitionRevision() == SceneDefinitionRevision{1});
        Commit(service);
        REQUIRE(service.ActiveScene()->DefinitionRevision() == SceneDefinitionRevision{2});
        REQUIRE(service.ActiveScene()->Get(old).HasError());
        REQUIRE(authority->checks == 4);
        REQUIRE(service.QueueUnload().HasValue());
        Commit(service);
        REQUIRE_FALSE(service.ActiveScene().has_value());
        service.Shutdown();
    }

    TEST_CASE("Deferred cell fencing cancellation and failure preserve the active scene", "[scene][cell_payload][integration][failure]") {
        auto authority = std::make_shared<Authority>();
        RuntimeSceneService service;
        REQUIRE(service.Startup({}).HasValue());
        REQUIRE(QueueRuntimeSceneCellPayload(service, Payload(), Fence(), authority).HasValue());
        Commit(service);
        const auto original = service.ActiveScene()->Find(SceneObjectId{1}).value();
        authority->identity = Identity(2);
        CancellationSource cancellation;
        REQUIRE(QueueRuntimeSceneCellPayload(service, Payload(2), Fence(), authority, cancellation.Token()).HasValue());
        cancellation.RequestCancellation();
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.cancelled");
        REQUIRE(service.ActiveScene()->Get(original).HasValue());

        REQUIRE(QueueRuntimeSceneCellPayload(service, Payload(2), Fence(), authority).HasValue());
        authority->fence.generation = IdentityFrom<W::StreamingGeneration>(2);
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.stale");
        REQUIRE(service.ActiveScene()->Get(original).HasValue());
        authority->fence = Fence();
        REQUIRE(QueueRuntimeSceneCellPayload(service, Payload(2), Fence(), authority).HasValue());
        authority->identity = Identity(3);
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.stale");
        REQUIRE(service.ActiveScene()->Get(original).HasValue());
        authority->identity = Identity(2);
        REQUIRE(QueueRuntimeSceneCellPayload(service, Payload(2), Fence(), authority).HasValue());
        REQUIRE(QueueRuntimeSceneCellPayload(service, Payload(2), Fence(), authority).ErrorValue().code.Value() ==
                "scene.operation.in_progress");
        REQUIRE(service.QueueUnload().HasValue());
        Commit(service);
        REQUIRE_FALSE(service.ActiveScene().has_value());
        service.Shutdown();
        REQUIRE(QueueRuntimeSceneCellPayload(service, Payload(2), Fence(), authority).ErrorValue().code.Value() ==
                "scene.service.shutdown");
    }
}  // namespace Horo::Runtime
