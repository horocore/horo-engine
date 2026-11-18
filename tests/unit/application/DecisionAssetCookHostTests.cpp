#include "DecisionGraphCompilerTestSupport.h"
#include "Horo/Application/BlackboardUsageReview.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Scene/SceneSource.h"
#include "assets/AssetCookServiceFixture.h"

#include <algorithm>
#include <catch2/generators/catch_generators.hpp>
#include <format>
#include <ranges>

namespace {
    using namespace Horo;
    using namespace Horo::AI;
    using namespace Horo::AI::DecisionGraphTestSupport;
    using namespace Horo::Application;
    using namespace Horo::Assets;
    namespace Files = Horo::Assets::ServiceTestSupport;

    struct Fixture final {
        DecisionGraphTestSupport::Fixture ai;
        Files::TempDir source, cache, cooked;
        JobSystem jobs;
        AssetRegistry registry;
        DecisionAssetCookRequest request;
        AssetId root{Files::Id("00000000-0000-0000-0000-0000000000a1")};
        AssetId child{Files::Id("00000000-0000-0000-0000-0000000000b1")};
        std::vector<AssetRecord> records;

        explicit Fixture(const bool utility = false) {
            std::filesystem::create_directories(source.path / "assets");
            if (utility)
                PrepareUtilitySchema();
            Add(root, "root.horo_sm", DecisionStateMachineSourceType,
                CallingMachine(1, 2, utility ? DecisionPlanKind::Utility : DecisionPlanKind::BehaviorTree));
            if (utility)
                Add(child, "task.horo_utility", DecisionUtilitySourceType, UtilityGraph());
            else
                Add(child, "task.horo_bt", DecisionBehaviorTreeSourceType, TaskTree(2));
            REQUIRE(registry.Publish(records).status == AssetRegistryBuildStatus::Complete);
            request.assets.sourceRoot = source.path;
            request.assets.cacheFiles = Horo::TestSupport::NativeCacheFiles(cache.path);
            request.assets.cookedRoot = cooked.path;
            request.assets.registry = registry.Snapshot();
            request.assets.target = Files::Target("headless-null");
            request.assets.limits.maximumConcurrentCooks = 1;
            CookPublicationTestSupport::ConfigureNativeCookPublication(request.assets);
            request.environment = ai.Environment();
            const auto capturedSchema = ai.schema;
            const auto capturedProviders = ai.providers;
            request.assets.validateHostInputs = [this, capturedSchema, capturedProviders]() -> Result<void> {
                if (ai.schema != capturedSchema || ai.providers != capturedProviders || !capturedProviders->CanUse())
                    return Result<void>::Failure(MakeError(DecisionCookErrors::Stale));
                return Result<void>::Success();
            };
            Capture();
        }

        void PrepareUtilitySchema() {
            const auto key = MakeIdentity<BlackboardKeyId>(1);
            const std::array keys{BlackboardKeyDescriptor{.key = key,
                                                          .kind = BlackboardValueKind::Scalar,
                                                          .defaultValue = BlackboardValue{BlackboardScalarValue{0.5}}}};
            auto captured = BlackboardSchema::Capture({SchemaId(), 1, BlackboardUnknownValuePolicy::Reject, keys});
            REQUIRE(captured.HasValue());
            ai.schema = std::make_shared<const BlackboardSchema>(std::move(captured).Value());
            auto instance = BlackboardInstance::Create(ai.binding, ai.schema);
            REQUIRE(instance.HasValue());
            ai.blackboard = std::move(instance).Value();
        }

        DecisionGraphSource UtilityGraph() const {
            UtilityDecisionSource body;
            body.root = Node(1);
            body.requirements = {{.key = MakeIdentity<BlackboardKeyId>(1), .kind = BlackboardValueKind::Scalar, .source = Location()}};
            body.options = {{.option = {Node(2), MakeIdentity<TaskId>(5002), 1, 0, {{.key = MakeIdentity<BlackboardKeyId>(1)}}},
                             .provider = {.id = Node(2), .type = Type(501), .descriptorVersion = {1, 1}, .source = Location()}}};
            return {Graph(2), 1, SchemaId(), {1, 1}, Location(), {}, std::move(body)};
        }

        void Add(AssetId id, std::string_view name, std::string_view type, const DecisionGraphSource &graph) {
            const std::string relative = "assets/" + std::string{name};
            WriteGraph(relative, graph);
            const auto sidecar = Files::SidecarJson(id.ToString(), type);
            Files::WriteFile(source.path / (relative + ".horo"), {reinterpret_cast<const std::uint8_t *>(sidecar.data()), sidecar.size()});
            records.push_back(
                {id, Files::Type(type), ProjectPath::Parse(relative).Value(), ProjectPath::Parse(relative + ".horo").Value()});
        }

        void WriteGraph(const std::string &relative, const DecisionGraphSource &graph) {
            auto encoded = DecisionPlanCodec::EncodeSource(std::span{&graph, 1}, graph.asset);
            REQUIRE(encoded.HasValue());
            Files::WriteFile(source.path / relative, encoded.Value());
        }

        void Capture() {
            auto captured =
                AssetCookInputSnapshot::Capture(source.path, request.assets.registry, request.assets.limits, 16U * 1024U * 1024U);
            REQUIRE(captured.HasValue());
            request.assets.pinnedInputs = std::make_shared<const AssetCookInputSnapshot>(std::move(captured).Value());
        }

        Result<AssetCookReport> Cook(CancellationToken cancellation = {}) {
            return DecisionAssetCookHost::Cook(jobs, request, cancellation);
        }

        AssetCookGeneration Current() const {
            auto current = ResolveCurrentCookGeneration(NativeDurableFileSystem{}, cooked.path);
            REQUIRE(current.HasValue());
            return current.Value();
        }

        std::vector<AssetCookArtifact> Read() const {
            auto contents = ReadCookGenerationContents(NativeDurableFileSystem{}, Current(), 16U * 1024U * 1024U);
            REQUIRE(contents.HasValue());
            std::vector<AssetCookArtifact> artifacts;
            for (const auto &bytes : contents.Value().artifacts) {
                auto artifact = DecodeCookedArtifact(bytes);
                REQUIRE(artifact.HasValue());
                artifacts.push_back(std::move(artifact).Value());
            }
            return artifacts;
        }
    };

    TEST_CASE("Project Blackboard review pins typed Decision uses and preserves incomplete owners",
              "[native][decision-cook][blackboard][usage]") {
        Fixture fixture{true};
        const auto schemaAsset = Files::Id("00000000-0000-0000-0000-0000000000ca");
        const auto schemaBytes = BlackboardSchemaCodec::Encode(*fixture.ai.schema);
        REQUIRE(schemaBytes.HasValue());
        const auto installSchema = [&](const AssetId asset, const std::string &name) {
            const auto path = fixture.source.path / "assets" / name;
            Files::WriteFile(path, schemaBytes.Value());
            const auto sidecar = Files::SidecarJson(asset.ToString(), BlackboardSchemaAssetType);
            Files::WriteFile(path.string() + ".horo", {reinterpret_cast<const std::uint8_t *>(sidecar.data()), sidecar.size()});
        };
        installSchema(schemaAsset, "guard.horo_bb");
        const auto rebuild = [&] {
            const auto result = RebuildAssetRegistry(fixture.registry, fixture.source.path, AssetRegistryOpenMode::ReadOnly);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().status == AssetRegistryBuildStatus::Complete);
            return result.Value().publishedRevision;
        };
        const auto original = rebuild();
        NativeDurableFileSystem files;
        Editor::ProjectMutationCoordinator mutations{files};
        const BlackboardUsageSelection selection{schemaAsset, fixture.ai.schema->Identity(), fixture.ai.schema->Version(),
                                                 fixture.ai.schema->Keys().front().key,
                                                 ComputeSha256(std::as_bytes(std::span{schemaBytes.Value()}))};
        const auto metadata = DecisionBlackboardProviderMetadata::Capture(*fixture.ai.providers);
        REQUIRE(metadata.HasValue());
        const auto review =
            CaptureBlackboardUsageReview(fixture.source.path, fixture.registry, original, mutations, selection, metadata.Value());
        REQUIRE(review.HasValue());
        CHECK(review.Value().registry == original);
        CHECK(review.Value().providers.lock() == metadata.Value());
        CHECK(review.Value().sources.size() == 3);
        REQUIRE(review.Value().uses.size() == 1);
        CHECK(review.Value().uses.front().asset == fixture.child);
        CHECK(review.Value().uses.front().use.requirement.key == selection.key);
        CHECK(review.Value().uses.front().use.requirement.source.source == "assets/task.horo_utility");
        CHECK_FALSE((std::ranges::find(review.Value().gaps, BlackboardUsageGapKind::DecisionProvider, &BlackboardUsageGap::kind) !=
                     review.Value().gaps.end()));
        CHECK((std::ranges::find(review.Value().gaps, BlackboardUsageGapKind::EnvironmentQueryBindings, &BlackboardUsageGap::kind) !=
               review.Value().gaps.end()));
        CHECK((std::ranges::find(review.Value().gaps, BlackboardUsageGapKind::PerceptionPolicy, &BlackboardUsageGap::kind) !=
               review.Value().gaps.end()));

        const auto partial = CaptureBlackboardUsageReview(fixture.source.path, fixture.registry, {}, mutations, selection);
        REQUIRE(partial.HasValue());
        CHECK(partial.Value().uses.size() == 1);
        CHECK((std::ranges::find(partial.Value().gaps, BlackboardUsageGapKind::DecisionProvider, &BlackboardUsageGap::kind) !=
               partial.Value().gaps.end()));
        CHECK((std::ranges::find(partial.Value().gaps, BlackboardUsageGapKind::RegistryInventory, &BlackboardUsageGap::kind) !=
               partial.Value().gaps.end()));

        SECTION("an observed provider requirement carries its origin without keeping the provider alive") {
            auto contribution = Registration(fixture.ai.probe);
            const auto origin = contribution.contract.descriptor.origin;
            contribution.contract.descriptor.requirements.push_back(
                {.key = selection.key, .kind = BlackboardValueKind::Scalar, .source = Location()});
            auto captured = DecisionNodeGeneration::Capture({&contribution, 1}, fixture.ai.admission);
            REQUIRE(captured.HasValue());
            auto generation = std::move(captured).Value();
            std::weak_ptr<const DecisionNodeGeneration> native{generation};
            const auto copiedContracts = DecisionBlackboardProviderMetadata::Capture(*generation);
            REQUIRE(copiedContracts.HasValue());
            generation.reset();
            CHECK(native.expired());
            const auto supplied = CaptureBlackboardUsageReview(fixture.source.path, fixture.registry, original, mutations, selection,
                                                               copiedContracts.Value());
            REQUIRE(supplied.HasValue());
            REQUIRE(supplied.Value().uses.size() == 2);
            const auto usage = std::ranges::find_if(supplied.Value().uses, [](const BlackboardDecisionUsage &use) {
                return use.use.provider.has_value();
            });
            REQUIRE(usage != supplied.Value().uses.end());
            CHECK(usage->use.provider == origin);
            CHECK(supplied.Value().providers.lock() == copiedContracts.Value());
        }
        SECTION("a changed selected source cannot review an older document opening") {
            std::vector<BlackboardSchemaAuthoringRow> labels;
            for (const auto &key : fixture.ai.schema->Keys())
                labels.push_back({key.key, "External name"});
            const auto changed = BlackboardSchemaSourceCodec::Encode(*fixture.ai.schema, labels);
            REQUIRE(changed.HasValue());
            Files::WriteFile(fixture.source.path / "assets/guard.horo_bb", changed.Value());
            const auto stale = CaptureBlackboardUsageReview(fixture.source.path, fixture.registry, original, mutations, selection);
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == DecisionCookErrors::Stale.code.Value());
        }
        SECTION("invalid source remains explicitly unresolved") {
            const std::array<std::uint8_t, 1> malformed{0};
            Files::WriteFile(fixture.source.path / "assets/task.horo_utility", malformed);
            const auto invalid = CaptureBlackboardUsageReview(fixture.source.path, fixture.registry, original, mutations, selection);
            REQUIRE(invalid.HasValue());
            CHECK(invalid.Value().uses.empty());
            CHECK(invalid.Value().closure != review.Value().closure);
            CHECK(std::ranges::any_of(invalid.Value().gaps, [&](const BlackboardUsageGap &gap) {
                return gap.kind == BlackboardUsageGapKind::DecisionSource && gap.asset == fixture.child && gap.diagnostic;
            }));
        }
        SECTION("distinct registered sources claiming the same schema are ambiguous") {
            installSchema(Files::Id("00000000-0000-0000-0000-0000000000cb"), "duplicate.horo_bb");
            const auto duplicate = CaptureBlackboardUsageReview(fixture.source.path, fixture.registry, rebuild(), mutations, selection);
            REQUIRE(duplicate.HasError());
            CHECK(duplicate.ErrorValue().code.Value() == AIErrors::DecisionAssetSchemaAmbiguous.code.Value());
        }
    }

    TEST_CASE("Project review reports saved controller schema bindings with separate unresolved owners",
              "[native][decision-cook][blackboard][usage]") {
        Fixture fixture{true};
        const auto schemaAsset = AssetId::FromBytes({0xc1});
        const auto sceneAsset = AssetId::FromBytes({0xc2});
        const auto prefabAsset = AssetId::FromBytes({0xc3});
        const auto unrelated = AssetId::FromBytes({0xc4});
        const auto schemaBytes = BlackboardSchemaCodec::Encode(*fixture.ai.schema);
        REQUIRE(schemaBytes.HasValue());
        const auto writePair = [&](std::string_view name, AssetId id, std::string_view type, std::string_view bytes) {
            Files::WriteFile(fixture.source.path / name, {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
            const auto sidecar = Files::SidecarJson(id.ToString(), type);
            Files::WriteFile(fixture.source.path / (std::string{name} + ".horo"),
                             {reinterpret_cast<const std::uint8_t *>(sidecar.data()), sidecar.size()});
        };
        writePair("assets/shared.horo_bb", schemaAsset, BlackboardSchemaAssetType,
                  {reinterpret_cast<const char *>(schemaBytes.Value().data()), schemaBytes.Value().size()});

        const AiControllerComponent controller{.controller = MakeIdentity<ControllerTypeId>(51),
                                               .decisionAsset = Graph(1),
                                               .blackboardSchema = fixture.ai.schema->Identity(),
                                               .enabled = false};
        SceneSource::SceneObjectSnapshot sceneObject{.id = {9},
                                                     .name = "Admitted direct controller",
                                                     .components = {.aiController = controller}};
        const SceneSource::ScenePrefabInstance placement{Prefab::PrefabInstanceId::Create(13).Value(),
                                                         Prefab::PrefabAssetReference::Create(prefabAsset).Value(),
                                                         {},
                                                         {}};
        const std::array sceneObjects{sceneObject};
        const std::array placements{placement};
        const auto sceneText = SceneSource::EncodeSceneSource({sceneObjects, placements});
        writePair("assets/level.horo", sceneAsset, "core.scene", sceneText);

        sceneObject.components.aiController->blackboardSchema = MakeIdentity<BlackboardSchemaId>(901);
        const std::array unrelatedObjects{sceneObject};
        writePair("assets/other.horo", unrelated, "core.scene", SceneSource::EncodeSceneSource({unrelatedObjects, {}}));

        const auto payload = std::format(
            R"({{"controller":{},"decisionAsset":{},"blackboardSchema":{},"decisionKind":"behavior_tree","requiredCapabilities":0,"schemaVersion":1,"startupPolicy":"scene_activation","enabled":false}})",
            controller.controller.Value(), controller.decisionAsset.Value(), controller.blackboardSchema.Value());
        const auto bytes = std::as_bytes(std::span{payload});
        const Prefab::RawComponentPayload component{.instance = Prefab::PrefabComponentInstanceId::Create(1).Value(),
                                                    .component = {.typeId =
                                                                      Gameplay::ComponentTypeId::Parse("game.horo.ai_controller").Value(),
                                                                  .schemaVersion = 1,
                                                                  .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                                                  .payload = {bytes.begin(), bytes.end()}}};
        const auto limits = Prefab::PrefabLimitProfile::Create({});
        REQUIRE(limits.HasValue());
        const auto prefab = Prefab::PrefabDocument::Create({.projectVersion = {0, 1, 0},
                                                            .assetId = prefabAsset,
                                                            .objects = {{.name = "Direct root", .components = {component}}}},
                                                           limits.Value());
        REQUIRE(prefab.HasValue());
        const auto serialized = prefab.Value().SerializeCanonical();
        REQUIRE(serialized.HasValue());
        writePair("assets/unit.prefab", prefabAsset, "core.prefab", serialized.Value());

        const auto rebuilt = RebuildAssetRegistry(fixture.registry, fixture.source.path, AssetRegistryOpenMode::ReadOnly);
        REQUIRE(rebuilt.HasValue());
        REQUIRE(rebuilt.Value().status == AssetRegistryBuildStatus::Complete);
        NativeDurableFileSystem files;
        Editor::ProjectMutationCoordinator mutations{files};
        const BlackboardUsageSelection selection{schemaAsset, fixture.ai.schema->Identity(), fixture.ai.schema->Version(),
                                                 fixture.ai.schema->Keys().front().key,
                                                 ComputeSha256(std::as_bytes(std::span{schemaBytes.Value()}))};
        const auto review = [&] {
            return CaptureBlackboardUsageReview(fixture.source.path, fixture.registry, rebuilt.Value().publishedRevision, mutations,
                                                selection);
        };
        const auto scanned = review();
        REQUIRE(scanned.HasValue());
        REQUIRE(scanned.Value().schemaBindings.size() == 2);
        CHECK(scanned.Value().sources.size() == 6);
        CHECK(scanned.Value().uses.size() == 1);  // Authored Decision key use remains a separate finding.
        const auto scene = std::ranges::find(scanned.Value().schemaBindings, sceneAsset, &BlackboardSchemaBinding::asset);
        REQUIRE(scene != scanned.Value().schemaBindings.end());
        CHECK(scene->sourcePath == "assets/level.horo");
        CHECK(std::get<SceneSource::SceneObjectId>(scene->object).value == 9);
        CHECK(scene->controller == controller.controller);
        CHECK(scene->decision == controller.decisionAsset);
        const auto concrete = std::ranges::find(scanned.Value().schemaBindings, prefabAsset, &BlackboardSchemaBinding::asset);
        REQUIRE(concrete != scanned.Value().schemaBindings.end());
        CHECK(std::get<Prefab::LocalObjectId>(concrete->object).IsRoot());
        CHECK(std::ranges::none_of(scanned.Value().gaps, [prefabAsset](const auto &gap) {
            return gap.asset == prefabAsset;
        }));
        CHECK(std::ranges::any_of(scanned.Value().gaps, [sceneAsset](const auto &gap) {
            return gap.kind == BlackboardUsageGapKind::SceneSource && gap.asset == sceneAsset;  // Unexpanded placement.
        }));

        SECTION("malformed serialized prefab reports a typed gap without losing unrelated evidence") {
            writePair("assets/unit.prefab", prefabAsset, "core.prefab", "{}");
            const auto rejected = review();
            REQUIRE(rejected.HasValue());
            CHECK(rejected.Value().schemaBindings.size() == 1);
            CHECK(rejected.Value().uses.size() == 1);
            CHECK(rejected.Value().closure != scanned.Value().closure);
            CHECK(std::ranges::any_of(rejected.Value().gaps, [prefabAsset](const auto &gap) {
                return gap.kind == BlackboardUsageGapKind::PrefabSource && gap.asset == prefabAsset && gap.diagnostic;
            }));
        }
        SECTION("failed hierarchy source capture preserves Decision evidence and records unknown owners") {
            REQUIRE(std::filesystem::remove(fixture.source.path / "assets/unit.prefab.horo"));
            const auto partial = review();
            REQUIRE(partial.HasValue());
            CHECK(partial.Value().schemaBindings.empty());
            CHECK(partial.Value().uses.size() == 1);
            CHECK(std::ranges::any_of(partial.Value().gaps, [prefabAsset](const auto &gap) {
                return gap.kind == BlackboardUsageGapKind::PrefabSource && gap.asset == prefabAsset && gap.diagnostic;
            }));
        }
    }

    TEST_CASE("Project registry discovers and cooks authored blackboard schemas for the exact decision environment",
              "[native][decision-cook][blackboard]") {
        Fixture fixture;
        const auto schemaId = Files::Id("00000000-0000-0000-0000-0000000000ca");
        const auto semantic = BlackboardSchemaCodec::Encode(*fixture.ai.schema);
        REQUIRE(semantic.HasValue());
        std::vector<BlackboardSchemaAuthoringRow> rows;
        for (const auto &key : fixture.ai.schema->Keys() | std::views::reverse)
            rows.push_back({key.key, "Authored Label"});
        const bool authoring = GENERATE(false, true);
        const auto encoded = authoring ? BlackboardSchemaSourceCodec::Encode(*fixture.ai.schema, rows) : semantic;
        REQUIRE(encoded.HasValue());
        const auto file = fixture.source.path / "assets/guard.horo_bb";
        Files::WriteFile(file, encoded.Value());
        const auto sidecar = Files::SidecarJson(schemaId.ToString(), BlackboardSchemaAssetType);
        Files::WriteFile(fixture.source.path / "assets/guard.horo_bb.horo",
                         {reinterpret_cast<const std::uint8_t *>(sidecar.data()), sidecar.size()});
        auto rebuilt = RebuildAssetRegistry(fixture.registry, fixture.source.path, AssetRegistryOpenMode::ReadOnly);
        REQUIRE(rebuilt.HasValue());
        REQUIRE(rebuilt.Value().status == AssetRegistryBuildStatus::Complete);
        fixture.request.assets.registry = fixture.registry.Snapshot();
        REQUIRE(fixture.request.assets.registry.Records().size() == 3);
        REQUIRE(fixture.request.assets.registry.Find(schemaId));
        fixture.Capture();
        SECTION("asset-only schemas produce and recover an executable plan") {
            fixture.request.environment.schemas.clear();
            auto cooked = fixture.Cook();
            REQUIRE(cooked.HasValue());
            CHECK(cooked.Value().cookedAssets == 3);
            const auto artifacts = fixture.Read();
            const auto schema = std::ranges::find(artifacts, schemaId, &AssetCookArtifact::id);
            const auto root = std::ranges::find(artifacts, fixture.root, &AssetCookArtifact::id);
            REQUIRE(schema != artifacts.end());
            REQUIRE(root != artifacts.end());
            CHECK(schema->type.Value() == BlackboardSchemaAssetType);
            CHECK(schema->payload == semantic.Value());
            auto recovered = BlackboardSchemaCodec::Decode(schema->payload);
            REQUIRE(recovered.HasValue());
            auto runtime = fixture.request.environment;
            runtime.schemas.push_back(std::make_shared<const BlackboardSchema>(std::move(recovered).Value()));
            const auto plan = DecisionPlanCodec::DecodeCooked(root->payload, runtime);
            REQUIRE(plan.HasValue());
            REQUIRE(plan.Value().plan);
            CHECK(plan.Value().plan->Root() == Graph(1));

            const auto reused = fixture.Cook();
            REQUIRE(reused.HasValue());
            CHECK(reused.Value().cacheHits == 3);
        }
        SECTION("duplicate exact schema versions from two authorities reject the cook") {
            const auto failed = fixture.Cook();
            REQUIRE(failed.HasError());
            CHECK(failed.ErrorValue().code.Value() == AIErrors::DecisionAssetSchemaAmbiguous.code.Value());
            CHECK(ResolveCurrentCookGeneration(NativeDurableFileSystem{}, fixture.cooked.path).HasError());
        }
    }

    TEST_CASE("Actual decision source pairs cook through AssetCookService and load into the real composed headless runner",
              "[native][decision-cook]") {
        Fixture fixture;
        auto first = fixture.Cook();
        REQUIRE(first.HasValue());
        CHECK(first.Value().cookedAssets == 2);
        CHECK(fixture.ai.probe.factories == 0);
        const auto artifacts = fixture.Read();
        REQUIRE(artifacts.size() == 2);
        const auto root = std::ranges::find(artifacts, fixture.root, &AssetCookArtifact::id);
        REQUIRE(root != artifacts.end());
        CHECK(root->type.Value() == DecisionStateMachineSourceType);
        auto loaded = DecisionPlanCodec::DecodeCooked(root->payload, fixture.ai.Environment());
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().plan);
        CHECK(loaded.Value().plan->Root() == Graph(1));
        CHECK(loaded.Value().plan->Graphs().size() == 2);
        auto instance = fixture.ai.Instance(loaded.Value().plan);
        CHECK(fixture.ai.Step(*instance, 1) == AiTaskState::Running);
        CHECK(fixture.ai.Step(*instance, 2) == AiTaskState::Succeeded);
        auto reused = fixture.Cook();
        REQUIRE(reused.HasValue());
        CHECK(reused.Value().cacheHits == 2);
        CHECK(reused.Value().cookedAssets == 0);
    }

    TEST_CASE("Actual Utility sources cook and load through a mixed HSM closure without provider side effects",
              "[native][decision-cook][utility]") {
        Fixture fixture{true};
        const auto cooked = fixture.Cook();
        REQUIRE(cooked.HasValue());
        CHECK(cooked.Value().cookedAssets == 2);
        CHECK(fixture.ai.probe.factories == 0);
        const auto artifacts = fixture.Read();
        const auto root = std::ranges::find(artifacts, fixture.root, &AssetCookArtifact::id);
        REQUIRE(root != artifacts.end());
        const auto loaded = DecisionPlanCodec::DecodeCooked(root->payload, fixture.ai.Environment());
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().plan);
        auto instance = fixture.ai.Instance(loaded.Value().plan);
        AiWorkBudget agent{8}, global{8};
        for (std::uint64_t tick = 1; tick <= 3; ++tick) {
            auto snapshot = fixture.ai.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            const auto step = instance->Step({tick, fixture.ai.binding, snapshot.Value(), {}, &agent, &global});
            REQUIRE(step.HasValue());
            if (tick == 3)
                CHECK(step.Value() == AiTaskState::Succeeded);
        }
        CHECK(fixture.ai.probe.starts == 1);
        CHECK(fixture.ai.probe.cleanups == 1);
        const auto reused = fixture.Cook();
        REQUIRE(reused.HasValue());
        CHECK(reused.Value().cacheHits == 2);
    }

    TEST_CASE("Invalid missing or future decision inputs preserve the actual prior published generation", "[native][decision-cook]") {
        Fixture fixture;
        REQUIRE(fixture.Cook().HasValue());
        const auto previous = fixture.Current();
        SECTION("missing actual dependency") {
            fixture.WriteGraph("assets/root.horo_sm", CallingMachine(1, 999, DecisionPlanKind::BehaviorTree));
            fixture.Capture();
        }
        SECTION("future actual source wire version") {
            auto encoded = DecisionPlanCodec::EncodeSource(std::array{TaskTree(2)}, Graph(2));
            REQUIRE(encoded.HasValue());
            auto future = encoded.Value();
            future[8] = 2;
            Files::WriteFile(fixture.source.path / "assets/task.horo_bt", future);
            fixture.Capture();
        }
        SECTION("bounded aggregate prepared output") {
            fixture.request.maximumPreparedBytes = 1;
        }
        CHECK(fixture.Cook().HasError());
        CHECK(fixture.Current().manifestDigest == previous.manifestDigest);
        CHECK(fixture.Current().generationRoot == previous.generationRoot);
        CHECK(fixture.ai.probe.factories == 0);
    }

    TEST_CASE("Decision provider revocation at the real publication fence cannot replace its prior generation", "[native][decision-cook]") {
        Fixture fixture;
        REQUIRE(fixture.Cook().HasValue());
        const auto previous = fixture.Current();
        const auto original = fixture.request.assets.validateHostInputs;
        std::size_t observations{};
        fixture.request.assets.validateHostInputs = [&]() -> Result<void> {
            if (++observations == 3)
                fixture.ai.admission->store(false, std::memory_order_release);
            return original();
        };
        CHECK(fixture.Cook().HasError());
        CHECK(observations >= 3);
        CHECK(fixture.Current().manifestDigest == previous.manifestDigest);
        CHECK(fixture.Current().generationRoot == previous.generationRoot);
    }

    TEST_CASE("Cancellation before actual decision current pointer replacement retains the complete prior generation",
              "[native][decision-cook]") {
        Fixture fixture;
        REQUIRE(fixture.Cook().HasValue());
        const auto previous = fixture.Current();
        CancellationSource cancellation;
        auto files = std::make_shared<Files::CookPublicationFiles>();
        files->cancellation = &cancellation;
        fixture.request.assets.publicationFiles = files;
        CHECK(fixture.Cook(cancellation.Token()).HasError());
        CHECK_FALSE(files->committed);
        CHECK(fixture.Current().manifestDigest == previous.manifestDigest);
        CHECK(fixture.Current().generationRoot == previous.generationRoot);
    }

    TEST_CASE("Decision cook detects real file drift and missing host authority before any output publication", "[native][decision-cook]") {
        Fixture fixture;
        REQUIRE(fixture.Cook().HasValue());
        const auto previous = fixture.Current();
        SECTION("source bytes changed after capture") {
            fixture.WriteGraph("assets/task.horo_bt", TaskTree(9));
        }
        SECTION("schema publication changed") {
            const std::array keys{BlackboardKeyDescriptor{.key = MakeIdentity<BlackboardKeyId>(2)}};
            auto schema = BlackboardSchema::Capture({SchemaId(), 2, BlackboardUnknownValuePolicy::Reject, keys});
            REQUIRE(schema.HasValue());
            fixture.ai.schema = std::make_shared<const BlackboardSchema>(std::move(schema).Value());
        }
        SECTION("required actual host fence absent") {
            fixture.request.assets.validateHostInputs = {};
        }
        CHECK(fixture.Cook().HasError());
        CHECK(fixture.Current().manifestDigest == previous.manifestDigest);
        CHECK(fixture.Current().generationRoot == previous.generationRoot);
    }
}  // namespace
