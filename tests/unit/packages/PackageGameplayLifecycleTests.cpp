#include "../runtime/ai/BehaviorTreeRuntimeTestSupport.h"
#include "GameplayModuleTestSupport.h"
#include "Horo/AI/AIScenePerceptionSource.h"
#include "Horo/AI/PerceptionSenseScene.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameEventRegistry.h"
#include "Horo/Packages/GameplayPackageModuleHost.h"
#include "PackageLifecycleTestSupport.h"
#include "PerceptionSenseTestSupport.h"

#include <cstdint>
#include <thread>
using namespace Horo::Packages::Tests::Support;

namespace {
    std::shared_ptr<PackageRestoreGraph> GameplayGraph(const bool withBackend = false) {
        const auto entry = "module" + std::filesystem::path{HORO_TEST_GAME_MODULE_PATH}.extension().string();
        nlohmann::json descriptor{{"id", "fixture.gameplay"},
                                  {"version", "1.0.0"},
                                  {"modules",
                                   {{{"id", "game.tests"},
                                     {"version", "1.0.0"},
                                     {"kind", "native"},
                                     {"roles", {"runtime-participant"}},
                                     {"entry", entry},
                                     {"requiredCapabilities", {"runtime.participate"}}}}}};
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("extensions/native/Node.horo_script", "return {role='condition',check=function() return true end}");
        files.emplace_back("extensions/native/Sense.horo_script", "return function(x,y,z,lx,ly,lz) return x-lx < 500 end");
        if (withBackend) {
            const auto backend = "backend" + std::filesystem::path{HORO_PACKAGE_NATIVE_FIXTURE}.extension().string();
            descriptor["modules"].push_back({{"id", "fixture.backend"},
                                             {"version", "1.0.0"},
                                             {"kind", "native"},
                                             {"roles", {"backend-capability"}},
                                             {"entry", backend}});
            descriptor["contributions"] = {{{"type", "asset.importer"}, {"id", "fixture.importer"}, {"module", "fixture.backend"}}};
            files.emplace_back("extensions/native/" + backend, FileBytes(HORO_PACKAGE_NATIVE_FIXTURE));
        }
        return Graph(HORO_TEST_GAME_MODULE_PATH, true, true, "fixture.gameplay", std::move(descriptor), std::move(files));
    }

    struct GameplayFixture final {
        Fixture base;
        std::shared_ptr<PackageModuleRetirementQueue> retirement{std::make_shared<PackageModuleRetirementQueue>(4)};
        std::shared_ptr<GameplayPackageModuleHost> gameplay;
        std::shared_ptr<NativePackageModuleHost> backend;
        bool withBackend{};
        EnabledPackageModule enabled{Parse<HoroPackageId>("fixture.gameplay"), Parse<PackagePath>("extensions/native/extension.json"),
                                     "game.tests", Extensions::ExtensionModuleRole::RuntimeParticipant};
        unsigned worldFactories{};

        explicit GameplayFixture(const bool approved = true, const bool correctFingerprint = true, const bool composeHost = true,
                                 const bool mixed = false)
            : withBackend(mixed) {
            base.service.reset();
            base.trust.capabilities = {{"runtime.participate"}};
            base.trust.permissions = {{"runtime.participate"}};
            if (approved)
                base.trust.approvals = base.trust.permissions;
            GameplayPackageModuleBinding binding{enabled.package,
                                                 enabled.module,
                                                 correctFingerprint ? std::string{Gameplay::CurrentGameplayBuildFingerprint()}
                                                                    : "wrong-sdk",
                                                 Horo::Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                                 {},
                                                 [this](const VerifiedPackageModule &) {
                ++worldFactories;
                return Result<std::shared_ptr<Gameplay::GameplayPhysicsContext>>::Success({});
            }};
            gameplay =
                std::make_shared<GameplayPackageModuleHost>(GameplayPackageHostConfiguration{std::filesystem::canonical(
                                                                                                 base.temporary.Path()),
                                                                                             ::Horo::Tests::CreateAcceptingArtifactGate()},
                                                            std::vector<GameplayPackageModuleBinding>{std::move(binding)});
            PackageLifecycleConfiguration config{.temporaryRoot = std::filesystem::canonical(base.temporary.Path()),
                                                 .profile = Extensions::ExtensionHostProfile::Headless,
                                                 .capabilities = {"runtime.participate"},
                                                 .artifactGate = ::Horo::Tests::CreateAcceptingArtifactGate()};
            if (composeHost)
                config.moduleHosts = {gameplay};
            if (mixed) {
                backend = std::make_shared<NativePackageModuleHost>(config);
                config.moduleHosts.push_back(backend);
            }
            config.moduleRetirements = retirement;
            auto created = PackageLifecycleService::Create(base.install, base.trust, std::move(config));
            REQUIRE(created.HasValue());
            base.service = std::move(created).Value();
            base.Install(GameplayGraph(mixed));
        }

        ~GameplayFixture() {
            base.service.reset();
            CHECK(retirement->Close().HasValue());
        }

        Result<void> Activate() {
            std::vector<EnabledPackageModule> selected;
            if (withBackend)
                selected.emplace_back(enabled.package, enabled.descriptor, "fixture.backend",
                                      Extensions::ExtensionModuleRole::BackendCapability);
            selected.push_back(enabled);
            return base.service->ActivateModules(selected, PackageActivationBoundary::Quiescent, base.cancellation.Token());
        }
    };
}  // namespace

TEST_CASE("Package runtime role invokes actual SDK owner and retains native callbacks until drain", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture;
    REQUIRE(fixture.Activate().HasValue());
    const auto generation = fixture.base.service->Active()->generation;
    auto loaded = fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", generation);
    REQUIRE(loaded);
    CHECK(loaded->Components().IsFrozen());
    CHECK(loaded->ActiveServices().size() == 1);
    auto acquired = loaded->Events().Acquire(90, 1, 91, 1, loaded->Cancellation());
    REQUIRE(acquired.HasValue());
    auto callback = std::move(acquired).Value();
    const auto artifact = loaded->LoadedArtifactPath();
    fixture.base.service->Shutdown();
    CHECK_FALSE(fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", generation));
    CHECK(fixture.retirement->Drain() == 1);
    CHECK(std::filesystem::exists(artifact));
    loaded.reset();
    CHECK(fixture.retirement->Close().HasError());
    std::jthread releaseCallback{[owned = std::move(callback)]() mutable {
        owned.reset();
    }};
    releaseCallback.join();
    CHECK(fixture.retirement->Close().HasValue());
    CHECK_FALSE(std::filesystem::exists(artifact));
}

TEST_CASE("Package runtime permission is denied before explicit world and SDK factories", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture(false);
    CHECK(fixture.Activate().HasError());
    CHECK(fixture.worldFactories == 0);
    CHECK_FALSE(fixture.base.service->Active());
}

TEST_CASE("Package runtime fingerprint mismatch is a failed required role rather than activation", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture(true, false);
    CHECK(fixture.Activate().HasError());
    CHECK(fixture.worldFactories == 0);
    CHECK_FALSE(fixture.base.service->Active());
    CHECK(fixture.retirement->Close().HasValue());
}

TEST_CASE("Required gameplay role cannot be satisfied by native extension metadata", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture(true, true, false);
    CHECK(fixture.Activate().HasError());
    CHECK(fixture.worldFactories == 0);
}

TEST_CASE("Package gameplay replacement requires exact previous generation retirement", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture;
    REQUIRE(fixture.Activate().HasValue());
    const auto first = fixture.base.service->Active()->generation;
    auto previous = fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", first);
    REQUIRE(previous);
    CHECK(fixture.Activate().HasError());
    CHECK(fixture.base.service->Active()->generation == first);
    fixture.base.service->Shutdown();
    previous.reset();
    REQUIRE(fixture.retirement->Close().HasValue());
}

TEST_CASE("Actual gameplay generations drain before repeated update and uninstall", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture;
    std::uint64_t previousGeneration{};
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        fixture.base.Install(GameplayGraph());
        REQUIRE(fixture.Activate().HasValue());
        const auto generation = fixture.base.service->Active()->generation;
        CHECK(generation > previousGeneration);
        previousGeneration = generation;
        auto loaded = fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", generation);
        REQUIRE(loaded);
        REQUIRE(
            fixture.base.service->ActivateModules({}, PackageActivationBoundary::Quiescent, fixture.base.cancellation.Token()).HasValue());
        CHECK_FALSE(fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", generation));
        CHECK(fixture.retirement->Close().HasError());
        loaded.reset();
        REQUIRE(fixture.retirement->Close().HasValue());
    }
    auto removed = std::make_shared<PackageRestoreGraph>(*fixture.base.install.ActiveGraph());
    removed->packages.clear();
    REQUIRE(fixture.base.install.Install(removed, fixture.base.cancellation.Token()).HasValue());
    REQUIRE(fixture.base.service->ActivateModules({}, PackageActivationBoundary::Quiescent, fixture.base.cancellation.Token()).HasValue());
    CHECK(fixture.base.service->Active()->install->Graph()->packages.empty());
    REQUIRE(fixture.retirement->Close().HasValue());
}

TEST_CASE("Optional SDK failure preserves an actual native backend in the same verified archive", "[packages][gameplay][hybrid]") {
    GameplayFixture fixture(true, false, true, true);
    fixture.enabled.requirement = PackageModuleRequirement::Optional;
    REQUIRE(fixture.Activate().HasValue());
    auto active = fixture.base.service->Active();
    REQUIRE(active->modules.size() == 2);
    CHECK(fixture.backend->Importers(fixture.enabled.package, "fixture.backend", active->generation));
    CHECK_FALSE(fixture.gameplay->Acquire(fixture.enabled.package, "game.tests", active->generation));
    const auto runtime = std::ranges::find_if(active->modules, [](const auto &report) {
        return report.module == "game.tests";
    });
    REQUIRE(runtime != active->modules.end());
    CHECK(runtime->diagnostic.has_value());
}

TEST_CASE("Verified package decision nodes retain exact locked version and real generated factories", "[packages][decision-node]") {
    GameplayFixture fixture;
    REQUIRE(fixture.Activate().HasValue());
    const auto generation = fixture.base.service->Active()->generation;
    auto acquired = fixture.gameplay->AcquireDecisionNodes(fixture.enabled.package, "game.tests", generation);
    REQUIRE(acquired.HasValue());
    auto contribution = std::move(acquired).Value();
    CHECK(contribution.package == fixture.enabled.package);
    CHECK(contribution.version == fixture.base.install.ActiveGraph()->packages[0].lock.version);
    CHECK(contribution.artifact == fixture.base.install.ActiveGraph()->packages[0].lock.artifactDigest);
    CHECK(contribution.activationGeneration == generation);
    REQUIRE(contribution.nodes->Contracts().size() == 1);
    CHECK(contribution.nodes->Contracts()[0].descriptor.type.Value() == 201);
    CHECK(contribution.nodes->Contracts()[0].descriptor.origin.kind == AI::DecisionDescriptorSourceKind::Package);
    CHECK(fixture.gameplay->AcquireDecisionNodes(fixture.enabled.package, "game.tests", generation + 1).HasError());
    fixture.base.service->Shutdown();
    CHECK_FALSE(contribution.nodes->CanAdmit());
    CHECK(fixture.retirement->Close().HasError());
    contribution.nodes.reset();
    CHECK(fixture.retirement->Close().HasValue());
}

TEST_CASE("Verified package custom senses preserve locked identity and revoke before native cleanup", "[packages][sense-provider]") {
    GameplayFixture fixture;
    REQUIRE(fixture.Activate().HasValue());
    const auto generation = fixture.base.service->Active()->generation;
    auto acquired = fixture.gameplay->AcquirePerceptionSenses(fixture.enabled.package, "game.tests", generation);
    REQUIRE(acquired.HasValue());
    auto contribution = std::move(acquired).Value();
    CHECK(contribution.package == fixture.enabled.package);
    CHECK(contribution.version == fixture.base.install.ActiveGraph()->packages[0].lock.version);
    CHECK(contribution.artifact == fixture.base.install.ActiveGraph()->packages[0].lock.artifactDigest);
    CHECK(contribution.activationGeneration == generation);
    REQUIRE(contribution.senses->Contracts().size() == 1);
    CHECK(contribution.senses->Contracts()[0].sense.identity.Value() == 202);
    CHECK(contribution.senses->Contracts()[0].sense.origin.kind == AI::PerceptionDescriptorSourceKind::Package);
    CHECK(fixture.gameplay->AcquirePerceptionSenses(fixture.enabled.package, "game.tests", generation + 1).HasError());
    bool offOwnerRejected{};
    std::thread offOwner([&] {
        offOwnerRejected = fixture.gameplay->AcquirePerceptionSenses(fixture.enabled.package, "game.tests", generation).HasError();
    });
    offOwner.join();
    CHECK(offOwnerRejected);
    AI::PerceptionSenseTestSupport::InputFixture inputs;
    auto instance = inputs.Instance(contribution.senses);
    AI::AiWorkBudget budget{6};
    REQUIRE(instance->Evaluate(inputs.Input(), budget, budget).HasValue());
    fixture.base.service->Shutdown();
    CHECK_FALSE(contribution.senses->CanAdmit());
    CHECK(fixture.gameplay->AcquirePerceptionSenses(fixture.enabled.package, "game.tests", generation).HasError());
    AI::AiWorkBudget revoked{6};
    AI::TestSupport::ExpectError(instance->Evaluate(inputs.Input(2), revoked, revoked), AI::AIErrors::RuntimeUnavailable);
    CHECK(fixture.retirement->Close().HasError());
    instance.reset();
    CHECK(fixture.retirement->Close().HasError());
    contribution.senses.reset();
    CHECK(fixture.retirement->Close().HasValue());
}

TEST_CASE("Verified package script nodes use actual declared bytes and inherit native activation retirement", "[packages][decision-node]") {
    GameplayFixture fixture;
    REQUIRE(fixture.Activate().HasValue());
    const auto generation = fixture.base.service->Active()->generation;
    auto compiled =
        fixture.gameplay->CompileDecisionNode(fixture.enabled.package, "game.tests", generation,
                                              Parse<PackagePath>("extensions/native/Node.horo_script"),
                                              AI::DecisionNodeTypeId::Create(202).Value(), AI::DecisionProviderId::Create(702).Value(), 1);
    REQUIRE(compiled.HasValue());
    auto program = std::move(compiled).Value();
    auto acquired = program->Acquire();
    REQUIRE(acquired.HasValue());
    auto nodes = std::move(acquired).Value();
    CHECK(nodes->Contracts()[0].descriptor.source.source == "extensions/native/Node.horo_script");
    CHECK(nodes->Contracts()[0].descriptor.source.line == 0);
    AI::BehaviorTreeTestSupport::Fixture world({AI::BehaviorTreeTestSupport::Node(1, AI::BehaviorTreeOperation::Task)});
    AI::DecisionAssetDescriptor asset{.asset = AI::DecisionGraphAssetId::Create(100).Value(),
                                      .blackboardSchema = world.schema->Identity(),
                                      .nodes = {{.id = AI::DecisionNodeId::Create(1).Value(),
                                                 .type = AI::DecisionNodeTypeId::Create(202).Value(),
                                                 .source = {__FILE__, __LINE__, 1}}},
                                      .source = {__FILE__, __LINE__, 1}};
    const std::array descriptors{nodes->Contracts()[0].descriptor};
    auto bound = AI::DecisionAssetCompiler::Compile(asset, {}, descriptors, std::array{world.schema});
    REQUIRE(bound.HasValue());
    for (const auto &diagnostic : bound.Value().validation.Diagnostics())
        UNSCOPED_INFO(diagnostic.code.Value() << " " << diagnostic.path << " " << diagnostic.source.source);
    REQUIRE(bound.Value().IsValid());
    auto created = nodes->CreateExecutor(nodes, *bound.Value().plan, {});
    REQUIRE(created.HasValue());
    auto executor = std::move(created).Value();
    auto snapshot = world.blackboard->Snapshot();
    REQUIRE(snapshot.HasValue());
    const AI::BehaviorTreeEvaluationContext context{AI::DecisionNodeId::Create(1).Value(), {}, snapshot.Value(), 1};
    REQUIRE(executor->Check(context).HasValue());
    CHECK(executor->Check(context).Value());
    fixture.base.service->Shutdown();
    CHECK_FALSE(nodes->CanAdmit());
    CHECK(executor->Check(context).HasError());
    CHECK(program->Acquire().HasError());
    CHECK(fixture.retirement->Close().HasError());
    executor.reset();
    nodes.reset();
    program.reset();
    CHECK(fixture.retirement->Close().HasValue());
}

TEST_CASE("Verified package script senses use declared bytes and retain their actual native parent", "[packages][sense-provider]") {
    GameplayFixture fixture;
    REQUIRE(fixture.Activate().HasValue());
    const auto activation = fixture.base.service->Active()->generation;
    AI::PerceptionSenseTestSupport::InputFixture inputs;
    const AI::PerceptionDescriptorOrigin origin{AI::PerceptionDescriptorSourceKind::Script,
                                                AI::TestSupport::MakeIdentity<AI::PerceptionProviderId>(702), 1};
    AI::PerceptionSenseContract contract{{AI::TestSupport::MakeIdentity<AI::SenseTypeId>(302), origin, "package script sense",
                                          inputs.capabilities},
                                         {AI::TestSupport::MakeIdentity<AI::StimulusTypeId>(303), origin, "package script result", 1, {}},
                                         0,
                                         4,
                                         2};
    const auto source = Parse<PackagePath>("extensions/native/Sense.horo_script");
    CHECK(fixture.gameplay->CompilePerceptionSense(fixture.enabled.package, "game.tests", activation + 1, source, contract).HasError());
    CHECK(fixture.gameplay
              ->CompilePerceptionSense(fixture.enabled.package, "game.tests", activation,
                                       Parse<PackagePath>("extensions/native/Missing.horo_script"), contract)
              .HasError());
    auto compiled = fixture.gameplay->CompilePerceptionSense(fixture.enabled.package, "game.tests", activation, source, contract);
    REQUIRE(compiled.HasValue());
    auto program = std::move(compiled).Value();
    auto acquired = program->Acquire();
    REQUIRE(acquired.HasValue());
    auto senses = std::move(acquired).Value();
    CHECK(senses->Contracts()[0].source.source == source.Value());
    auto instance = inputs.Instance(senses);
    AI::AiWorkBudget budget{6};
    const auto evaluated = instance->Evaluate(inputs.Input(), budget, budget);
    REQUIRE(evaluated.HasValue());
    REQUIRE(evaluated.Value().count == 2);
    CHECK(evaluated.Value().samples[0].sensed);
    CHECK_FALSE(evaluated.Value().samples[1].sensed);

    Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
    for (std::uint64_t index = 1; index <= 3; ++index) {
        Runtime::RuntimeEntityDefinition entity;
        entity.object = Runtime::SceneObjectId{index};
        builder.Add(entity);
    }
    const auto definition = std::move(builder).Build();
    REQUIRE(definition.HasValue());
    auto created = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{77});
    REQUIRE(created.HasValue());
    auto scene = std::move(created).Value();
    const auto view = scene->View();
    AI::PerceptionSenseTestSupport::InputFixture sceneInputs;
    const auto listener = *view.Find(Runtime::SceneObjectId{1});
    sceneInputs.binding.listener = AI::ProjectPerceptionSource(listener);
    auto sceneInstance = sceneInputs.Instance(senses);
    AI::PerceptionSpatialListener spatialListener;
    spatialListener.entity = listener;
    spatialListener.position = Math::WorldCoordinate64::FromMillimeters(1000, 0, 0);
    spatialListener.senses[0] = contract.sense.identity;
    spatialListener.senseCount = 1;
    std::array<AI::PerceptionSpatialSource, 2> sources;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        auto &candidate = sources[index];
        candidate.entity = *view.Find(Runtime::SceneObjectId{index + 2});
        candidate.position = sceneInputs.candidates[index].position;
        candidate.layers = 1;
        candidate.senses[0] = contract.sense.identity;
        candidate.senseCount = 1;
    }
    AI::PerceptionSpatialBroadphase spatial;
    REQUIRE(spatial.Publish(view, 1, std::span{&spatialListener, 1}, sources).HasValue());
    auto admittedMemory = AI::AIPerceptionMemory::Create(77, sceneInputs.binding.agent);
    REQUIRE(admittedMemory.HasValue());
    auto memory = std::move(admittedMemory).Value();
    const AI::PerceptionSpatialQuery query{.listener = listener,
                                           .sense = contract.sense.identity,
                                           .radiusMillimeters = 10'000,
                                           .visibleLayers = 1,
                                           .maximumCandidates = 2};
    AI::AiWorkBudget sceneBudget{100};
    const auto staged = AI::RunPerceptionSenseAtSensePoll(view, spatial, *sceneInstance, query, 1, memory, sceneBudget, sceneBudget);
    REQUIRE(staged.HasValue());
    CHECK(staged.Value().candidates == 2);
    CHECK(staged.Value().samples == 2);
    CHECK(sceneBudget.Remaining() == 93);
    const auto before = memory.Snapshot(1, AI::PerceptionSceneLiveness(*scene));
    REQUIRE(before.HasValue());
    REQUIRE(before.Value().count == 1);
    CHECK(before.Value().entries[0].lastKnownPosition == sources[0].position);

    fixture.base.service->Shutdown();
    CHECK_FALSE(senses->CanAdmit());
    CHECK(program->Acquire().HasError());
    CHECK(fixture.retirement->Close().HasError());
    AI::AiWorkBudget revoked{6};
    AI::TestSupport::ExpectError(instance->Evaluate(inputs.Input(2), revoked, revoked), AI::AIErrors::RuntimeUnavailable);
    AI::TestSupport::ExpectError(AI::RunPerceptionSenseAtSensePoll(view, spatial, *sceneInstance, query, 2, memory, sceneBudget,
                                                                   sceneBudget),
                                 AI::AIErrors::RuntimeUnavailable);
    CHECK(sceneBudget.Remaining() == 93);
    const auto after = memory.Snapshot(2, AI::PerceptionSceneLiveness(*scene));
    REQUIRE(after.HasValue());
    REQUIRE(after.Value().count == 1);
    CHECK(after.Value().entries[0].lastSensedTick == 1);
    instance.reset();
    senses.reset();
    program.reset();
    CHECK(fixture.retirement->Close().HasError());
    sceneInstance.reset();
    CHECK(fixture.retirement->Close().HasValue());
}

TEST_CASE("Package node admission rejects denied retired foreign and undeclared source selections", "[packages][decision-node]") {
    GameplayFixture fixture;
    CHECK(fixture.gameplay->AcquireDecisionNodes(fixture.enabled.package, "game.tests", 1).HasError());
    REQUIRE(fixture.Activate().HasValue());
    const auto generation = fixture.base.service->Active()->generation;
    CHECK(fixture.gameplay
              ->CompileDecisionNode(fixture.enabled.package, "game.tests", generation,
                                    Parse<PackagePath>("extensions/native/Missing.horo_script"),
                                    AI::DecisionNodeTypeId::Create(202).Value(), AI::DecisionProviderId::Create(702).Value(), 1)
              .HasError());
    bool foreignRejected{};
    std::jthread worker([&] {
        foreignRejected = fixture.gameplay->AcquireDecisionNodes(fixture.enabled.package, "game.tests", generation).HasError();
    });
    worker.join();
    CHECK(foreignRejected);
    fixture.base.service->Shutdown();
    CHECK(fixture.gameplay->AcquireDecisionNodes(fixture.enabled.package, "game.tests", generation).HasError());
}
