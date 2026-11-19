#include "CommittedSceneAssets.h"
#include "DecisionGraphCompilerTestSupport.h"
#include "HeadlessCookedDecisionAdmission.h"
#include "HeadlessNetworkServices.h"
#include "HeadlessSceneLoadBoundary.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "OwnedTestDirectory.h"
#include "PhysicsSceneActivationTestSupport.h"
#include "assets/AssetCookOutputFixture.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <thread>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Application::Internal;
    using namespace std::chrono_literals;

    /** @brief Gates a real deterministic provider to observe owned pending/cancellation paths.
     * @details Only atomics cross the provider worker/owner/releaser lanes; all lanes join before fixture destruction.
     */
    class FencedAssetProvider final : public Assets::IAssetProvider {
    public:
        Result<bool> Exists(Assets::AssetId id, const CancellationToken &cancellation) const override {
            return memory.Exists(id, cancellation);
        }

        Result<std::vector<std::uint8_t>> Load(Assets::AssetId id, const CancellationToken &cancellation) const override {
            active.store(true, std::memory_order_release);
            entered.store(true, std::memory_order_release);
            while (!released.load(std::memory_order_acquire) && !cancellation.IsCancellationRequested())
                std::this_thread::sleep_for(1ms);
            cancelled.store(cancellation.IsCancellationRequested(), std::memory_order_release);
            auto result = memory.Load(id, cancellation);
            active.store(false, std::memory_order_release);
            return result;
        }

        Assets::MemoryAssetProvider memory;
        mutable std::atomic<bool> entered{};
        mutable std::atomic<bool> active{};
        mutable std::atomic<bool> cancelled{};
        std::atomic<bool> released{};
    };

    /** @brief Starts actual workers before asset services and keeps them alive until all private Scene handles join. */
    struct LoadFixture final {
        Assets::AssetId asset{Physics::Asset(79)};
        Assets::AssetTypeId type{Physics::AssetType()};
        Assets::AssetRegistry registry;
        FencedAssetProvider provider;
        JobSystem jobs{JobSystemConfig{1, 16}};
        Assets::AssetLoadService loads{jobs, provider};
        Runtime::RuntimeSceneService scene{registry, loads, {}, Runtime::SceneRuntimeId{71}};

        LoadFixture() {
            REQUIRE(jobs.WorkerCount() > 0);
            REQUIRE(registry.Publish({Physics::Record(asset, type)}).status == Assets::AssetRegistryBuildStatus::Complete);
            provider.memory.Insert(asset, {1, 3, 7});
        }

        ~LoadFixture() {
            scene.Shutdown();
            loads.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Drain);
        }

        LoadFixture(const LoadFixture &) = delete;
        LoadFixture &operator=(const LoadFixture &) = delete;
        LoadFixture(LoadFixture &&) = delete;
        LoadFixture &operator=(LoadFixture &&) = delete;

        Runtime::RuntimeSceneDefinition Definition() const {
            Runtime::SceneDefinitionBuilder builder{{7}, {1}};
            builder.Add({.object = {9}});
            REQUIRE(builder.RequireAsset({asset, type}).HasValue());
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            return std::move(definition).Value();
        }
    };

    bool AwaitProvider(const FencedAssetProvider &provider) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!provider.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        return provider.entered.load(std::memory_order_acquire);
    }

    TEST_CASE("Headless load boundary waits for actual required asset readiness on an already running executor", "[scene][host][load]") {
        LoadFixture fixture;
        bool observedPending{};
        std::jthread release{[&] {
            observedPending = AwaitProvider(fixture.provider);
            fixture.provider.released.store(true, std::memory_order_release);
        }};
        const auto loaded =
            PrepareHeadlessSceneAtLoadBoundary(fixture.scene, fixture.Definition(), {{}, std::chrono::steady_clock::now() + 3s});
        release.join();
        REQUIRE(observedPending);
        REQUIRE(loaded.HasValue());
        CHECK(loaded.Value().RuntimeId() == Runtime::SceneRuntimeId{71});
        const auto resident = loaded.Value().FindAsset(fixture.asset);
        REQUIRE(resident.has_value());
        REQUIRE(resident->bytes.size() == 3);
        CHECK(resident->bytes[2] == 7);
        CHECK_FALSE(fixture.provider.active.load());
        CHECK_FALSE(fixture.provider.cancelled.load());
    }

    TEST_CASE("Headless load cancellation joins only the private candidate's actual pending asset work", "[scene][host][load]") {
        LoadFixture fixture;
        CancellationSource cancellation;
        bool observedPending{};
        std::jthread cancel{[&] {
            observedPending = AwaitProvider(fixture.provider);
            cancellation.RequestCancellation();
        }};
        const auto loaded = PrepareHeadlessSceneAtLoadBoundary(fixture.scene, fixture.Definition(),
                                                               {cancellation.Token(), std::chrono::steady_clock::now() + 3s});
        cancel.join();
        REQUIRE(observedPending);
        REQUIRE(loaded.HasError());
        CHECK(loaded.ErrorValue().code.Value() == HeadlessSceneLoadErrors::Cancelled.code.Value());
        CHECK_FALSE(fixture.scene.ActiveScene().has_value());
        CHECK_FALSE(fixture.provider.active.load());
        CHECK(fixture.provider.cancelled.load());
    }

    TEST_CASE("Headless load deadline preserves its primary classification through owned asset cancellation", "[scene][host][load]") {
        LoadFixture fixture;
        const auto loaded =
            PrepareHeadlessSceneAtLoadBoundary(fixture.scene, fixture.Definition(), {{}, std::chrono::steady_clock::now() + 100ms});
        REQUIRE(loaded.HasError());
        CHECK(loaded.ErrorValue().code.Value() == HeadlessSceneLoadErrors::Deadline.code.Value());
        CHECK_FALSE(fixture.scene.ActiveScene().has_value());
        CHECK_FALSE(fixture.provider.active.load());
        if (fixture.provider.entered.load())
            CHECK(fixture.provider.cancelled.load());
    }

    TEST_CASE("Headless load preserves the actual provider error and leaves an independently active Scene intact", "[scene][host][load]") {
        LoadFixture fixture;
        Runtime::RuntimeSceneService old{Runtime::SceneRuntimeId{69}};
        REQUIRE(old.Startup({}).HasValue());
        REQUIRE(old.QueuePreparation(Physics::Definition()).HasValue());
        const CancellationToken token;
        const Runtime::FrameContext frame{.frameNumber = 1, .cancellation = token};
        REQUIRE(old.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        REQUIRE(old.ActiveScene().has_value());
        const auto previous = *old.ActiveScene();
        fixture.provider.memory.Remove(fixture.asset);
        fixture.provider.released.store(true);
        const auto expected = fixture.provider.memory.Load(fixture.asset, {});
        REQUIRE(expected.HasError());
        const auto loaded =
            PrepareHeadlessSceneAtLoadBoundary(fixture.scene, fixture.Definition(), {{}, std::chrono::steady_clock::now() + 3s});
        REQUIRE(loaded.HasError());
        CHECK(loaded.ErrorValue().code.Value() == expected.ErrorValue().code.Value());
        CHECK(loaded.ErrorValue().domain.Value() == expected.ErrorValue().domain.Value());
        CHECK(loaded.ErrorValue().message == expected.ErrorValue().message);
        CHECK_FALSE(fixture.scene.ActiveScene().has_value());
        CHECK_FALSE(fixture.provider.active.load());
        REQUIRE(old.ActiveScene().has_value());
        CHECK(old.ActiveScene()->RuntimeId() == previous.RuntimeId());
        CHECK(previous.Find({1}).has_value());
        old.Shutdown();
    }

    TEST_CASE("Headless expired and cancelled load budgets reject before starting required asset work", "[scene][host][load]") {
        for (const bool cancelled : {false, true}) {
            LoadFixture fixture;
            CancellationSource cancellation;
            if (cancelled)
                cancellation.RequestCancellation();
            const auto deadline = std::chrono::steady_clock::now() + (cancelled ? 3s : -1s);
            const auto loaded = PrepareHeadlessSceneAtLoadBoundary(fixture.scene, fixture.Definition(), {cancellation.Token(), deadline});
            REQUIRE(loaded.HasError());
            CHECK(loaded.ErrorValue().code.Value() ==
                  (cancelled ? HeadlessSceneLoadErrors::Cancelled.code.Value() : HeadlessSceneLoadErrors::Deadline.code.Value()));
            CHECK_FALSE(fixture.provider.entered.load());
            CHECK_FALSE(fixture.scene.ActiveScene().has_value());
        }
    }

    TEST_CASE("Headless cooked controller admission verifies query outputs against its committed generation", "[gameplay][decision][eqs]") {
        using namespace AI;
        namespace Decisions = AI::DecisionGraphTestSupport;
        namespace Output = Assets::OutputTestSupport;
        Decisions::Fixture fixture;
        const auto key = Decisions::MakeIdentity<BlackboardKeyId>(43);
        const auto queryId = Decisions::MakeIdentity<QueryId>(44);
        const auto resultId = Decisions::MakeIdentity<QueryResultSchemaId>(45);
        const auto registry = CaptureBuiltinQuerySchema();
        REQUIRE(registry.HasValue());
        const auto query = CreateBuiltinLatticeQuery(queryId, Decisions::MakeIdentity<QueryStageId>(46), resultId, "Test positions",
                                                     {.stepMillimeters = 500, .outerSteps = 1}, 8);
        REQUIRE(query.HasValue());
        const std::array keys{
            BlackboardKeyDescriptor{.key = key, .kind = BlackboardValueKind::WorldCoordinate, .presence = BlackboardKeyPresence::Optional}};
        auto schema = BlackboardSchema::Capture({.identity = Decisions::SchemaId(), .keys = keys});
        REQUIRE(schema.HasValue());
        fixture.schema = std::make_shared<const BlackboardSchema>(std::move(schema).Value());
        const auto &result = query.Value().ResultSchema();
        const BehaviorTreeQueryOutput output{queryId, result.id, result.version, result.itemType, key, 10};
        auto registration = Decisions::Registration(fixture.probe);
        registration.contract.properties.emplace_back(Decisions::MakeIdentity<BehaviorTreePropertyId>(47), "output",
                                                      BehaviorTreePropertyKind::QueryOutput, output);
        auto providers = DecisionNodeGeneration::Capture({&registration, 1}, fixture.admission);
        REQUIRE(providers.HasValue());
        fixture.providers = std::move(providers).Value();
        auto task = Decisions::TreeNode(2, DecisionControlKind::Task);
        task.properties = registration.contract.properties;
        task.requirements = {{.key = key,
                              .kind = BlackboardValueKind::WorldCoordinate,
                              .access = BlackboardKeyAccess::ReadWrite,
                              .source = Decisions::Location()}};
        auto graph = Decisions::Tree(1, {Decisions::TreeNode(1, DecisionControlKind::Root), task}, {Decisions::Edge(1, 1, 2)});
        graph.executionTask = Decisions::MakeIdentity<TaskId>(48);
        const auto compiled = DecisionGraphCompiler::Compile(graph.asset, std::span{&graph, 1}, fixture.Environment());
        REQUIRE(compiled.HasValue());
        for (const auto &diagnostic : compiled.Value().validation.Diagnostics())
            UNSCOPED_INFO(diagnostic.message);
        REQUIRE(compiled.Value().plan);
        const AiControllerDescriptor controller{.controller = Decisions::MakeIdentity<ControllerTypeId>(49),
                                                .decisionAsset = graph.asset,
                                                .decisionKind = DecisionPlanKind::BehaviorTree,
                                                .blackboardSchema = fixture.schema};
        const std::array catalog{controller};
        const auto decision = DecisionPlanCodec::EncodeCooked(*compiled.Value().plan);
        REQUIRE(decision.HasValue());
        const auto payload = EnvironmentQueryCookedCodec::Encode(query.Value(), registry.Value());
        REQUIRE(payload.HasValue());

        const bool available = GENERATE(true, false);
        const auto decisionId = Assets::AssetId::FromBytes({103});
        const auto queryAsset = Assets::AssetId::FromBytes({104});
        const auto decisionType = Assets::AssetTypeId::Parse(DecisionAssetType(DecisionPlanKind::BehaviorTree)).Value();
        const auto queryType = Assets::AssetTypeId::Parse(EnvironmentQuerySourceAssetType).Value();
        const auto target = Output::Target("headless-null");
        std::vector<Assets::AssetCookManifestEntry> manifest;
        std::vector<std::vector<std::uint8_t>> artifacts;
        std::vector<Assets::AssetRecord> records;
        const auto add = [&](Assets::AssetId id, const auto &type, const auto &bytes, std::string_view path) {
            auto encoded = Assets::EncodeCookedArtifact(
                {.id = id, .type = type, .target = target, .payloadDigest = Output::DigestOf(bytes), .payload = bytes});
            REQUIRE(encoded.HasValue());
            manifest.push_back({id, type, id.ToString() + ".cooked", Output::DigestOf(encoded.Value())});
            artifacts.push_back(std::move(encoded).Value());
            records.push_back({id, type, ProjectPath::Parse(path).Value(), ProjectPath::Parse(std::string{path} + ".horo").Value()});
        };
        add(decisionId, decisionType, decision.Value(), "assets/decision.horo_bt");
        if (available)
            add(queryAsset, queryType, payload.Value(), "assets/tactics.horo_eqs");
        Horo::Tests::OwnedTestDirectory directory{"headless-query-admission"};
        REQUIRE(Output::PublishFixture(directory.Path(), target, manifest, artifacts).HasValue());
        Assets::AssetRegistry assets;
        REQUIRE(assets.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
        const auto committed = CommittedSceneAssets::Capture(NativeDurableFileSystem{}, {directory.Path(), target}, assets.Snapshot());
        REQUIRE(committed.HasValue());
        Runtime::SceneDefinitionBuilder builder{{7}, {1}};
        builder.Add({.object = {8}, .localTransform = {.translation = {2, 0, 0}}});
        Runtime::RuntimeEntityDefinition querier{.object = {9},
                                                 .parent = Runtime::SceneObjectId{8},
                                                 .localTransform = {.translation = {0.5F, 0, 0}}};
        const auto identity = Decisions::MakeIdentity<AgentId>(50);
        querier.components.aiAgent = AiAgentComponent{.agent = identity};
        querier.components.aiController = AiControllerComponent{.controller = controller.controller,
                                                                .decisionAsset = graph.asset,
                                                                .blackboardSchema = fixture.schema->Identity()};
        builder.Add(querier);
        auto scene = std::move(builder).Build();
        REQUIRE(scene.HasValue());
        const auto limits = Prefab::PrefabLimitProfile::Create({});
        REQUIRE(limits.HasValue());
        const HeadlessCookedDecisionInput input{.committed = *committed.Value(),
                                                .environment = fixture.Environment(),
                                                .bindings = {{controller.controller, decisionId}},
                                                .querySchema = std::make_shared<const QuerySchemaRegistry>(registry.Value())};
        const auto admitted = AdmitHeadlessCookedControllers(catalog, scene.Value(), input, limits.Value(), {});
        if (available) {
            REQUIRE(admitted.HasValue());
            REQUIRE(admitted.Value().size() == 1);
            CHECK(admitted.Value().front().cookedPlan->Root() == graph.asset);
        } else {
            REQUIRE(admitted.HasError());
            CHECK(admitted.ErrorValue().code.Value() == AIErrors::EnvironmentQueryDescriptorUnavailable.code.Value());
        }
        CHECK(fixture.probe.factories == 0);
#if HORO_TEST_PHYSICS_NATIVE
        if (available) {
            JobSystem jobs{{1, 8}};
            Assets::AssetLoadService loads{jobs, committed.Value()->SceneProvider()};
            const HeadlessSceneLoadBoundary load{{}, std::chrono::steady_clock::now() + std::chrono::seconds{5}};
            auto execution = input;
            execution.execution.emplace(AiSceneCookedDecisionExecution{jobs, {}, {}});
            auto composed =
                ComposeHeadlessSceneNetworkServices(std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(scene).Value()),
                                                    {.mode = Physics::PhysicsRuntimeMode::Canonical,
                                                     .settings = Physics::Settings(),
                                                     .registry = assets,
                                                     .loads = loads,
                                                     .load = load,
                                                     .aiControllers = catalog,
                                                     .cookedDecisions =
                                                         std::make_shared<const HeadlessCookedDecisionInput>(std::move(execution))});
            REQUIRE(composed.HasValue());
            using namespace Network;
            const auto service = [&](NetworkModeServiceKind kind) {
                return composed.Value().factories.services[static_cast<std::size_t>(kind)](
                    {kind, NetworkProjectRole::Standalone, {}, NetworkModeWorldKind::Standalone, {7}});
            };
            auto sceneService = service(NetworkModeServiceKind::Scene);
            REQUIRE(sceneService.HasValue());
            REQUIRE(sceneService.Value()->Prepare().HasValue());
            auto physics = service(NetworkModeServiceKind::Physics);
            REQUIRE(physics.HasValue());
            REQUIRE(physics.Value()->Prepare().HasValue());
            REQUIRE(physics.Value()->Activate().HasValue());
            const auto before = InspectHeadlessDecisionValue(*physics.Value(), identity, key);
            REQUIRE(before.HasValue());
            CHECK_FALSE(before.Value());
            for (std::uint64_t tick = 1; tick <= 5; ++tick) {
                const auto frame = physics.Value()->RunFixedTick({tick, Duration::FromNanoseconds(16'666'667), {}});
                if (frame.HasError())
                    UNSCOPED_INFO(frame.ErrorValue().message);
                REQUIRE(frame.HasValue());
            }
            const auto after = InspectHeadlessDecisionValue(*physics.Value(), identity, key);
            REQUIRE(after.HasValue());
            REQUIRE(after.Value());
            const auto &winner = std::get<Math::WorldCoordinate64>(std::get<BlackboardScalarValue>(*after.Value()));
            CHECK(winner == Math::WorldCoordinate64::FromMillimeters(2000, 0, -500));
            CHECK(fixture.probe.factories == 0);
            physics.Value()->Shutdown();
            sceneService.Value()->Shutdown();
            CHECK(InspectHeadlessDecisionValue(*physics.Value(), identity, key).HasError());
        }
#endif
    }
}  // namespace
