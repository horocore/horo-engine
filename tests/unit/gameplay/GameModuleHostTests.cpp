#include "GameplayModuleTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameAssetTypeRegistry.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/GameplayRegistrationRuntime.h"
#include "Horo/Gameplay/ReplicationRegistration.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Runtime;

    GameModuleLoadExpectation Expectation() {
        return {
            .moduleId = "game.tests",
            .buildFingerprint = CurrentGameplayBuildFingerprint(),
            .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
        };
    }

    IGameModule *CreateTestModule() noexcept {
        return nullptr;
    }

    void DestroyTestModule(IGameModule *) noexcept {}

    IBehaviorInstance *CreateTestBehavior(void *) {
        return nullptr;
    }

    void DestroyTestBehavior(void *, IBehaviorInstance *) noexcept {}

    class RestartRequiredModule final : public IGameModule {
    public:
        Result<void> Register(GameRegistrationContext &) override {
            return Result<void>::Success();
        }

        Result<void> Start(GameRuntimeContext &) override {
            return Result<void>::Success();
        }

        void Stop(GameRuntimeContext &) noexcept override {}
    };

    struct ValidBundleStorage {
        BehaviorDescriptor behavior;
        GeneratedBehaviorFactoryBinding binding;
        GeneratedGameplayDescriptorBundle bundle;

        ValidBundleStorage() {
            behavior.typeId = BehaviorTypeId::Parse("game.tests.valid").Value();
            binding = {
                .typeId = behavior.typeId,
                .factory = {.userData = nullptr, .create = &CreateTestBehavior, .destroy = &DestroyTestBehavior},
            };
            bundle = {
                .structSize = sizeof(GeneratedGameplayDescriptorBundle),
                .schemaVersion = GameplayDescriptorBundleSchemaVersion,
                .sdkBoundaryVersion = GameplaySdkBoundaryVersion,
                .moduleId = "game.tests",
                .buildFingerprint = CurrentGameplayBuildFingerprint().data(),
                .descriptorRevision = 7,
                .behaviors = &behavior,
                .behaviorCount = 1,
                .nativeFactoryBindings = &binding,
                .nativeFactoryBindingCount = 1,
                .diagnostics = nullptr,
                .diagnosticCount = 0,
                .lifecycle = {.create = &CreateTestModule, .destroy = &DestroyTestModule},
            };
        }
    };

    RuntimeSceneDefinition Definition() {
        return Tests::SingleBehaviorSceneDefinition(SceneDefinitionId{3}, SceneDefinitionRevision{1}, SceneObjectId{1},
                                                    BehaviorInstanceId{1}, BehaviorTypeId::Parse("game.tests.dynamic_mover").Value());
    }

    template <typename Value> void RequireRestartRequired(const Result<Value> &result) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == GameplayErrors::GameplayReloadRestartRequired.code.Value());
    }

    template <typename CreateRuntime, typename ExerciseRuntime>
    void CheckGenerationLease(CreateRuntime createRuntime, ExerciseRuntime exerciseRuntime) {
        GameModuleHost host;
        auto moduleResult = host.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
        REQUIRE(moduleResult.HasValue());
        std::unique_ptr<LoadedGameModule> loaded = std::move(moduleResult).Value();
        auto created = createRuntime(*loaded);
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();

        RequireRestartRequired(loaded->PrepareReload());
        const auto rejected = createRuntime(*loaded);
        RequireRestartRequired(rejected);

        loaded.reset();
        CHECK(exerciseRuntime(*runtime).HasValue());
        runtime->Shutdown();
    }

    void RequireFrozenRegistries(const LoadedGameModule &loaded) {
        REQUIRE(loaded.Registry().IsFrozen());
        REQUIRE(loaded.Components().IsFrozen());
        REQUIRE(loaded.Components().Descriptors().size() == 1);
        REQUIRE(loaded.Components().Descriptors().front().typeId.Value() == "game.tests.movement_settings");
        REQUIRE(loaded.AssetTypes().IsFrozen());
        REQUIRE(loaded.AssetTypes().Registrations().size() == 1);
        REQUIRE(loaded.AssetTypes().Registrations().front().descriptor.typeId.Value() == "game.tests.quest_definition");
        REQUIRE(loaded.Services().IsFrozen());
        REQUIRE(loaded.Services().Registrations().size() == 1);
        REQUIRE(loaded.Systems().IsFrozen());
        REQUIRE(loaded.Systems().Registrations().size() == 1);
    }
}  // namespace

TEST_CASE("game module host validates fingerprint and keeps factories alive through behavior shutdown") {
    GameModuleHost host;
    auto relative = host.Load(std::filesystem::path{HORO_TEST_GAME_MODULE_PATH}.filename(), Expectation());
    REQUIRE(relative.HasError());
    GameModuleLoadExpectation mismatchExpectation = Expectation();
    mismatchExpectation.buildFingerprint = "wrong-fingerprint";
    auto mismatch = host.Load(HORO_TEST_GAME_MODULE_PATH, mismatchExpectation);
    REQUIRE(mismatch.HasError());
    REQUIRE(mismatch.ErrorValue().code.Value() == GameplayErrors::IncompatibleGameModule.code.Value());

    auto loaded = host.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value()->ModuleId() == "game.tests");
    RequireFrozenRegistries(*loaded.Value());
    REQUIRE(loaded.Value()->ActiveServices().size() == 1);
    REQUIRE(loaded.Value()->Capabilities().size() == 1);
    REQUIRE_FALSE(loaded.Value()->Cancellation().IsCancellationRequested());

    {
        auto systems = GameplaySystemRuntime::Create(loaded.Value()->Systems(), loaded.Value()->ActiveServices(),
                                                     loaded.Value()->Capabilities(), loaded.Value()->Cancellation());
        REQUIRE(systems.HasValue());
        REQUIRE(systems.Value()->Execute(GameplaySystemPhase::Gameplay, GameplayThreadAffinity::RuntimeOwner, 1.0 / 60.0).HasValue());
        systems.Value()->Shutdown();
    }

    {
        Tests::ActiveBehaviorRuntime active = Tests::ActivateBehaviorRuntime(Definition(), SceneRuntimeId{7}, loaded.Value()->Registry());
        const GameplayInputAction move{GameplayActionId{"game.tests.move"}, 3.0F, 0.0F, true, true, false};
        REQUIRE(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, SceneObjectId{1}, {&move, 1}).x == 3.0F);
        active.runtime->Shutdown();
    }

    std::unique_ptr<LoadedGameModule> active = std::move(loaded).Value();
    auto reloadSnapshot = active->PrepareReload();
    REQUIRE(reloadSnapshot.HasValue());
    REQUIRE(active->Cancellation().IsCancellationRequested());
    REQUIRE(active->ActiveServices().empty());
    REQUIRE(active->Capabilities().empty());
    REQUIRE(reloadSnapshot.Value().schemaVersion == 1);
    REQUIRE(reloadSnapshot.Value().payload.empty());
    active.reset();

    auto replacement = host.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    REQUIRE(replacement.HasValue());
    REQUIRE(replacement.Value()->RestoreReload(reloadSnapshot.Value()).HasValue());
}

TEST_CASE("game module host publishes native replication registrations") {
    GameModuleHost host;
    auto loaded = host.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value()->Replication().IsFrozen());

    auto replication = loaded.Value()->Replication().Acquire();
    REQUIRE(replication.HasValue());
    REQUIRE(replication.Value().Registrations().size() == 1);
    REQUIRE(replication.Value().Descriptors()->Schemas().size() == 1);
}

TEST_CASE("default game module reload contract requires a restart") {
    RestartRequiredModule module;
    GameRuntimeContext context;
    const auto prepared = module.PrepareReload(context);
    REQUIRE(prepared.HasError());
    CHECK(prepared.ErrorValue().code.Value() == GameplayErrors::GameplayReloadRestartRequired.code.Value());
}

TEST_CASE("game module generation remains pinned by external behavior and system runtimes") {
    SECTION("behavior runtime") {
        auto createdScene = RuntimeScene::Create(Definition(), SceneRuntimeId{18});
        REQUIRE(createdScene.HasValue());
        std::unique_ptr<RuntimeScene> scene = std::move(createdScene).Value();
        CheckGenerationLease([&scene](LoadedGameModule &loaded) {
            return BehaviorRuntime::Create(*scene, loaded.Registry());
        }, [](BehaviorRuntime &runtime) {
            return runtime.FixedUpdate({}, FixedDeltaTime{1.0 / 60.0});
        });
    }

    SECTION("system runtime") {
        CheckGenerationLease([](LoadedGameModule &loaded) {
            return GameplaySystemRuntime::Create(loaded.Systems(), loaded.ActiveServices(), loaded.Capabilities(), loaded.Cancellation());
        }, [](GameplaySystemRuntime &runtime) {
            return runtime.Execute(GameplaySystemPhase::Gameplay, GameplayThreadAffinity::RuntimeOwner, 1.0 / 60.0);
        });
    }

    SECTION("replication generation") {
        GameModuleHost host;
        auto loadedResult = host.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
        REQUIRE(loadedResult.HasValue());
        auto replication = loadedResult.Value()->Replication().Acquire();
        REQUIRE(replication.HasValue());
        std::unique_ptr<LoadedGameModule> loaded = std::move(loadedResult).Value();

        RequireRestartRequired(loaded->PrepareReload());
        RequireRestartRequired(loaded->Replication().Acquire());
        loaded.reset();

        const auto encoded = replication.Value().Serializers().Encode(Network::ReplicationSchemaId::Create(1001).Value(),
                                                                      Network::FieldId::Create(1).Value(), 3.5);
        REQUIRE(encoded.HasValue());
    }
}

TEST_CASE("game module host validates an independent shadow artifact and removes it after unload") {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "horo_game_module_host_shadow_copy_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    GameModuleHost host;
    auto loaded = host.LoadShadowCopy(std::filesystem::path{HORO_TEST_GAME_MODULE_PATH}, root, Expectation());
    REQUIRE(loaded.HasValue());
    std::unique_ptr<LoadedGameModule> module = std::move(loaded).Value();
    const std::filesystem::path shadowPath = module->LoadedArtifactPath();
    REQUIRE(std::filesystem::equivalent(shadowPath.parent_path(), root));
    REQUIRE(shadowPath != std::filesystem::path{HORO_TEST_GAME_MODULE_PATH});
    REQUIRE(std::filesystem::is_regular_file(shadowPath));

    module.reset();
    REQUIRE_FALSE(std::filesystem::exists(shadowPath));
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("generated gameplay bundle validation rejects incompatible identity before lifecycle activation") {
    ValidBundleStorage storage;
    const GameModuleLoadExpectation expected{
        .moduleId = "game.tests",
        .buildFingerprint = CurrentGameplayBuildFingerprint(),
        .descriptorRevision = 7,
    };
    REQUIRE(ValidateGeneratedGameplayDescriptorBundle(storage.bundle, expected).HasValue());

    storage.bundle.descriptorRevision = 8;
    const auto mismatch = ValidateGeneratedGameplayDescriptorBundle(storage.bundle, expected);
    REQUIRE(mismatch.HasError());
    REQUIRE(mismatch.ErrorValue().code.Value() == GameplayErrors::IncompatibleGameModule.code.Value());
}

TEST_CASE("generated gameplay bundle validation rejects incomplete bindings and bounded diagnostics") {
    ValidBundleStorage storage;
    const GameModuleLoadExpectation expected{
        .moduleId = "game.tests",
        .buildFingerprint = CurrentGameplayBuildFingerprint(),
        .descriptorRevision = 7,
    };

    storage.binding.factory.create = nullptr;
    const auto invalidBinding = ValidateGeneratedGameplayDescriptorBundle(storage.bundle, expected);
    REQUIRE(invalidBinding.HasError());
    REQUIRE(invalidBinding.ErrorValue().code.Value() == GameplayErrors::InvalidGeneratedDescriptorBundle.code.Value());

    storage.binding.factory.create = &CreateTestBehavior;
    const GeneratedDescriptorDiagnostic diagnostic{.code = "gameplay.codegen.failure", .message = "annotation rejected"};
    storage.bundle.diagnostics = &diagnostic;
    storage.bundle.diagnosticCount = 1;
    const auto diagnostics = ValidateGeneratedGameplayDescriptorBundle(storage.bundle, expected);
    REQUIRE(diagnostics.HasError());
    REQUIRE(diagnostics.ErrorValue().code.Value() == GameplayErrors::GeneratedDescriptorDiagnosticsPresent.code.Value());
}
