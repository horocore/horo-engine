#include "../runtime/ai/BehaviorTreeRuntimeTestSupport.h"
#include "ApprovedGameplayPackageHost.h"
#include "DecisionGraphCompilerTestSupport.h"
#include "GameplayModuleTestSupport.h"
#include "Horo/AI/AIScenePerceptionSource.h"
#include "Horo/AI/PerceptionSenseScene.h"
#include "Horo/Editor/PackageModalSession.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameEventRegistry.h"
#include "Horo/Packages/GameplayPackageModuleHost.h"
#include "Horo/Packages/PackageCache.h"
#include "Horo/Packages/PackageRequest.h"
#include "HostSignatureDocument.h"
#include "OpenXRProductSigningTestSupport.h"
#include "PackageApplicationServiceTestSupport.h"
#include "PackageHostPolicy.h"
#include "PackageLifecycleTestSupport.h"
#include "PackageProjectHost.h"
#include "PerceptionSenseTestSupport.h"

#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <fstream>
#include <thread>
using namespace Horo::Packages::Tests::Support;

namespace {
    std::shared_ptr<PackageRestoreGraph> GameplayGraph(const bool withBackend = false,
                                                       std::optional<std::string> moduleSignature = std::nullopt) {
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
        if (moduleSignature)
            files.emplace_back("extensions/native/" + entry + ".signature.json", std::move(*moduleSignature));
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

    /** @brief Runs the same authored closure with one admitted package condition. */
    std::shared_ptr<const AI::CookedDecisionPlan> RunPackageCondition(const AI::DecisionNodeTypeId type,
                                                                      const AI::DecisionCompilationEnvironment &environment,
                                                                      const AI::DecisionGraphTestSupport::Fixture &world,
                                                                      AI::DecisionStepTrace &trace) {
        using namespace AI::DecisionGraphTestSupport;
        auto condition = TreeNode(2, AI::DecisionControlKind::Condition);
        condition.kind = AI::BehaviorTreeNodeKind::Decorator;
        condition.minimumChildren = 1;
        condition.maximumChildren = 1;
        condition.type = AI::BehaviorTreeNodeTypeId::Create(type.Value()).Value();
        auto call = TreeNode(3, AI::DecisionControlKind::Subplan);
        call.call = AI::DecisionSubplanCall{Graph(2), AI::DecisionPlanKind::StateMachine, {1, 1}};
        const auto root = Tree(1, {TreeNode(1, AI::DecisionControlKind::Root), condition, call}, {Edge(1, 1, 2), Edge(2, 2, 3)});
        const std::array sources{root, TerminalMachine(2)};
        auto compiled = AI::DecisionGraphCompiler::Compile(Graph(1), sources, environment);
        REQUIRE(compiled.HasValue());
        for (const auto &diagnostic : compiled.Value().validation.Diagnostics())
            UNSCOPED_INFO(diagnostic.message);
        REQUIRE(compiled.Value().plan);
        auto plan = std::move(compiled).Value().plan;
        auto graph = AI::DecisionGraphInstance::Create(plan, environment.nodes, world.RuntimeBinding());
        REQUIRE(graph.HasValue());
        const auto units = plan->MaximumDecisionWorkUnits();
        REQUIRE(units.has_value());
        const auto snapshot = world.blackboard->Snapshot().Value();
        AI::AiWorkBudget budget{*units};
        const auto stepped = graph.Value()->StepBudgetedWithTrace({1, world.binding, snapshot, {}}, budget, trace);
        REQUIRE(stepped.HasValue());
        CHECK(stepped.Value() == std::optional{AI::AiTaskState::Succeeded});
        CHECK(budget.Remaining() == 0);
        return plan;
    }
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
    const auto decisionNodes = fixture.gameplay->AcquireActiveDecisionNodes(*fixture.base.service);
    REQUIRE(decisionNodes.HasValue());
    CHECK(decisionNodes.Value().empty());
}

TEST_CASE("Verified package decision nodes retain exact locked version and real generated factories", "[packages][decision-node]") {
    GameplayFixture fixture;
    const auto installed = fixture.gameplay->AcquireActiveDecisionNodes(*fixture.base.service);
    REQUIRE(installed.HasValue());
    CHECK(installed.Value().empty());
    REQUIRE(fixture.Activate().HasValue());
    const auto generation = fixture.base.service->Active()->generation;
    auto acquired = fixture.gameplay->AcquireActiveDecisionNodes(*fixture.base.service);
    REQUIRE(acquired.HasValue());
    REQUIRE(acquired.Value().size() == 1);
    auto contribution = std::move(std::move(acquired).Value().front());
    CHECK(contribution.package == fixture.enabled.package);
    CHECK(contribution.version == fixture.base.install.ActiveGraph()->packages[0].lock.version);
    CHECK(contribution.artifact == fixture.base.install.ActiveGraph()->packages[0].lock.artifactDigest);
    CHECK(contribution.activationGeneration == generation);
    REQUIRE(contribution.nodes->Contracts().size() == 1);
    CHECK(contribution.nodes->Contracts()[0].descriptor.type.Value() == 201);
    CHECK(contribution.nodes->Contracts()[0].descriptor.origin.kind == AI::DecisionDescriptorSourceKind::Package);
    CHECK(contribution.nodes->HasImplementation(contribution.nodes->Contracts()[0].descriptor.type));
    CHECK(fixture.gameplay->AcquireDecisionNodes(fixture.enabled.package, "game.tests", generation + 1).HasError());
    GameplayFixture foreign;
    REQUIRE(foreign.Activate().HasValue());
    CHECK(foreign.base.service->Active()->generation == generation);
    CHECK(fixture.gameplay->AcquireActiveDecisionNodes(*foreign.base.service).HasError());

    REQUIRE(fixture.base.service->ActivateModules({}, PackageActivationBoundary::Quiescent, fixture.base.cancellation.Token()).HasValue());
    const auto withdrawn = fixture.gameplay->AcquireActiveDecisionNodes(*fixture.base.service);
    REQUIRE(withdrawn.HasValue());
    CHECK(withdrawn.Value().empty());
    CHECK_FALSE(contribution.nodes->CanAdmit());
    CHECK(fixture.retirement->Close().HasError());
    contribution.nodes.reset();
    CHECK(fixture.retirement->Close().HasValue());
}

TEST_CASE("Signed offline Editor package selection publishes only exactly approved Decision nodes", "[packages][decision-node][editor]") {
    using namespace Horo::Application::Internal;
    enum class Admission {
        Approved,
        ArchiveMismatch,
        NativeSignatureMismatch
    };
    const auto admission = GENERATE(Admission::Approved, Admission::ArchiveMismatch, Admission::NativeSignatureMismatch);
    GameplayFixture fixture;
    Horo::Tests::Packages::TemporaryDirectory project{"editor package decisions"};
    const auto root = std::filesystem::canonical(project.Path());
    const auto content = std::filesystem::canonical(fixture.base.temporary.Path());
    const auto relativeMetadata = root / ".horo";
    REQUIRE(std::filesystem::create_directories(relativeMetadata));
    Horo::Tests::OpenXRProduct::SigningKey key;
    const auto moduleBytes = FileBytes(HORO_TEST_GAME_MODULE_PATH);
    const auto signedModule = key.Sign(std::as_bytes(
        std::span{admission == Admission::NativeSignatureMismatch ? std::string_view{"different bytes"} : std::string_view{moduleBytes}}));
    const auto locked = GameplayGraph(false, EncodeDetachedSignatureDocument(signedModule));
    const auto &entry = locked->packages.front();
    const auto intent = nlohmann::json{{"sources", {{"fixture.source", {{"kind", "public-registry"}, {"registry", "official"}}}}},
                                       {"dependencies", {{"fixture.gameplay", {{"source", "fixture.source"}, {"version", "1.0.0"}}}}}}
                            .dump();
    const auto request = ValidatedPackageRequest::Parse(intent);
    REQUIRE(request.HasValue());
    const PackageResolutionPlan resolution{
        .packages = {{entry.lock.package, entry.lock.version, entry.lock.source, entry.lock.artifactDigest, {}}}};
    const PackageLockArtifact artifact{.package = entry.lock.package,
                                       .version = entry.lock.version,
                                       .source = entry.lock.source,
                                       .artifactDigest = entry.lock.artifactDigest,
                                       .manifestDigest = entry.lock.manifestDigest,
                                       .fileManifestDigest = entry.lock.fileManifestDigest,
                                       .packageFormatVersion = 1,
                                       .contributions = {"native"}};
    const auto lock = ValidatedPackageLockfileV1::Generate(resolution, std::span{&entry.lock.package, 1}, request.Value().Digest(),
                                                           std::span{&artifact, 1});
    REQUIRE(lock.HasValue());
    const auto write = [](const std::filesystem::path &path, std::string_view bytes) {
        std::ofstream file{path, std::ios::binary};
        file << bytes;
        REQUIRE(file.good());
    };
    write(relativeMetadata / "packages.json", intent);
    write(relativeMetadata / "packages.lock", lock.Value().SerializeCanonical());
    const auto cacheRoot = content / "decision-host-cache";
    auto cache = PackageCacheStore::Create(fixture.base.files, cacheRoot);
    REQUIRE(cache.HasValue());
    auto cacheStore = std::move(cache).Value();
    REQUIRE(cacheStore.Publish(*entry.archive, key.Sign(entry.archive->Bytes())).HasValue());
    const auto trustDocument = content / "approved-publishers.json";
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string publicKey;
    for (const auto byte : key.root.publicKey) {
        const auto value = std::to_integer<unsigned>(byte);
        publicKey.push_back(digits[value >> 4U]);
        publicKey.push_back(digits[value & 15U]);
    }
    const auto trustJson = nlohmann::json{{"schemaVersion", 1},
                                          {"allowUnsigned", false},
                                          {"publishers",
                                           {{{"publisherId", key.root.publisherId},
                                             {"keyId", key.root.keyId},
                                             {"algorithm", "ecdsa-p256-sha256"},
                                             {"publicKeyHex", publicKey},
                                             {"expiresAtUnixMilliseconds", 0},
                                             {"revoked", false}}}}}
                               .dump();
    write(trustDocument, trustJson);
    const auto projectTrust = root / "unapproved-publishers.json";
    write(projectTrust, trustJson);
    JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 8, .ioWorkerCount = 1}};
    const SteadyClock clock;
    const SystemWallClock wall;
    const auto platform = NativePackagePlatform();
    REQUIRE(platform.HasValue());
    const PackageProjectPolicy base{.projectRoot = root,
                                    .cacheRoot = cacheRoot,
                                    .platform = platform.Value(),
                                    .lifecycle = {.temporaryRoot = cacheRoot / "native",
                                                  .profile = Extensions::ExtensionHostProfile::Interactive}};
    const ApprovedGameplayPackage approved{entry.lock.package,
                                           "fixture.gameplay",
                                           fixture.enabled.module,
                                           admission == Admission::ArchiveMismatch ? ComputeSha256({}) : entry.lock.artifactDigest,
                                           entry.lock.manifestDigest,
                                           Horo::Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)};
    CHECK(PrepareApprovedGameplayPackageHost(fixture.base.files, wall, base, {projectTrust, {approved}}).HasError());
    auto prepared = PrepareApprovedGameplayPackageHost(fixture.base.files, wall, base, {trustDocument, {approved}});
    REQUIRE(prepared.HasValue());
    auto configuration = std::move(prepared).Value();
    auto created =
        PackageProjectHost::Create({jobs, fixture.base.files, clock, wall, *configuration.execution}, std::move(configuration.policy),
                                   Duration::FromMilliseconds(60'000), nullptr, configuration.publishers.get());
    REQUIRE(created.HasValue());
    const auto host = std::move(created).Value();
    CHECK(host->HasGameplayModuleOwner());
    CHECK(host->Acquire(content).HasError());
    std::shared_ptr<Horo::Application::PackageApplication> application;
    for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
         !application && std::chrono::steady_clock::now() < deadline;) {
        auto acquired = host->Acquire(root);
        if (acquired.HasValue())
            application = std::move(acquired).Value();
        else
            REQUIRE(acquired.ErrorValue().code.Value() == "packages.host.preparing");
        std::this_thread::yield();
    }
    REQUIRE(application);
    CHECK_FALSE(host->DecisionCatalog(root));
    Horo::Editor::PackageModalSession modal{application};
    const auto generation = modal.Open();
    REQUIRE(generation.HasValue());
    CHECK(modal.ActivateModules({generation.Value().value + 1}, std::span{&fixture.enabled, 1}).HasError());
    const auto activated = modal.ActivateModules(generation.Value(), std::span{&fixture.enabled, 1});
    REQUIRE(activated.HasValue());
    const auto pump = [&](Horo::Application::PackageOperationId operation) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            REQUIRE(host->PumpProject(root, PackageActivationBoundary::Quiescent).HasValue());
            auto progress = application->Query(operation);
            REQUIRE(progress.HasValue());
            if (progress.Value().outcome != Horo::Application::PackageOperationOutcome::Running)
                return std::move(progress).Value();
            std::this_thread::yield();
        }
        FAIL("Configured project package request did not finish.");
        return Horo::Application::PackageOperationSnapshot{};
    };
    const auto publication = pump(activated.Value());
    INFO((publication.diagnostic ? publication.diagnostic->message : ""));
    if (admission != Admission::Approved) {
        CHECK(publication.outcome == Horo::Application::PackageOperationOutcome::Failed);
        CHECK_FALSE(host->DecisionCatalog(root));
        REQUIRE(modal.Close(generation.Value()).HasValue());
        REQUIRE(host->Close().HasValue());
        return;
    }
    REQUIRE(publication.outcome == Horo::Application::PackageOperationOutcome::Succeeded);
    const auto stamp = host->DecisionCatalog(root);
    REQUIRE(stamp);
    REQUIRE(publication.activeGeneration);
    CHECK(stamp->activation == *publication.activeGeneration);
    CHECK_FALSE(host->DecisionCatalog(content));
    CHECK(host->AcquireDecisionNodes(root, {stamp->project + 1, stamp->activation}).HasError());
    {
        const auto leaves = host->AcquireDecisionNodes(root, *stamp);
        REQUIRE(leaves.HasValue());
        REQUIRE(leaves.Value().size() == 1);
        REQUIRE(leaves.Value().front()->Contracts().size() == 1);
        CHECK(leaves.Value().front()->Contracts().front().descriptor.type.Value() == 201);
    }
    const auto disable = modal.Submit(generation.Value(), Horo::Application::PackageOperationKind::Disable);
    REQUIRE(disable.HasValue());
    REQUIRE(pump(disable.Value()).outcome == Horo::Application::PackageOperationOutcome::Succeeded);
    CHECK_FALSE(host->DecisionCatalog(root));
    CHECK(host->AcquireDecisionNodes(root, *stamp).HasError());
    REQUIRE(modal.Close(generation.Value()).HasValue());
    REQUIRE(host->Close().HasValue());
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

TEST_CASE("Verified native and Lua package nodes use the same cooked decision execution contract", "[packages][decision-node]") {
    GameplayFixture fixture;
    REQUIRE(fixture.Activate().HasValue());
    const auto generation = fixture.base.service->Active()->generation;
    auto acquired = fixture.gameplay->AcquireActiveDecisionNodes(*fixture.base.service);
    REQUIRE(acquired.HasValue());
    REQUIRE(acquired.Value().size() == 1);
    const auto native = acquired.Value().front().nodes;
    REQUIRE(native);
    REQUIRE(native->Contracts().size() == 1);
    const auto nativeType = native->Contracts().front().descriptor.type;
    const auto scriptType = AI::DecisionNodeTypeId::Create(202).Value();
    auto script = fixture.gameplay->CompileDecisionNode(fixture.enabled.package, "game.tests", generation,
                                                        Parse<PackagePath>("extensions/native/Node.horo_script"), scriptType,
                                                        AI::DecisionProviderId::Create(702).Value(), 1);
    REQUIRE(script.HasValue());
    auto acquiredScript = script.Value()->Acquire();
    REQUIRE(acquiredScript.HasValue());
    const std::array providers{native, acquiredScript.Value()};
    auto composed = AI::DecisionNodeGeneration::Compose(providers);
    REQUIRE(composed.HasValue());

    AI::DecisionGraphTestSupport::Fixture world;
    auto environment = world.Environment();
    environment.nodes = std::move(composed).Value();
    AI::DecisionStepTrace nativeTrace;
    AI::DecisionStepTrace scriptTrace;
    const auto nativePlan = RunPackageCondition(nativeType, environment, world, nativeTrace);
    const auto scriptPlan = RunPackageCondition(scriptType, environment, world, scriptTrace);
    CHECK_FALSE(nativeTrace.Records().empty());
    CHECK(std::ranges::equal(nativeTrace.Records(), scriptTrace.Records()));
    CHECK(nativeTrace.Header().planDigest != scriptTrace.Header().planDigest);

    fixture.base.service->Shutdown();
    for (const auto &plan : {nativePlan, scriptPlan}) {
        CHECK_FALSE(plan->IsCurrent());
        CHECK(AI::DecisionGraphInstance::Create(plan, environment.nodes, world.RuntimeBinding()).HasError());
    }
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
