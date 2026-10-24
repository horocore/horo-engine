#include "GameModuleHostTestHelpers.h"
#include "GameplayRetirementTestSupport.h"

using namespace Horo;
using namespace Horo::Gameplay;
using namespace Horo::Runtime;
using namespace Horo::Gameplay::HostTest;

namespace {
    /** @brief Copies canonical requirements from a real successfully frozen fixture generation. */
    GameplayReplicationAdmissionPolicy NativeAdmission() {
        auto loaded = GameModuleHost{}.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
        REQUIRE(loaded.HasValue());
        REQUIRE_FALSE(loaded.Value().diagnostic);
        auto acquired = loaded.Value().owner->Replication().Acquire();
        REQUIRE(acquired.HasValue());
        GameplayReplicationAdmissionPolicy policy;
        policy.inventory.completeness = Network::NetworkReplicationInventoryCompleteness::Complete;
        policy.inventory.declarations.push_back({acquired.Value().Descriptors()->Schemas().front()});
        policy.use = Network::ReplicationDeclarationUse::ServerActivation;
        return policy;
    }
}  // namespace

TEST_CASE("Native replication admission denies activation before service factories and module Start", "[gameplay][replication][host]") {
    auto policy = NativeAdmission();
    SECTION("incompatible schema") {
        policy.inventory.declarations[0].schema.version.major = 2;
        policy.inventory.declarations[0].schema.compatibility = {{2, 0}, {2, 0}};
    }
    SECTION("missing module") {
        policy.inventory.declarations[0].schema.owner.value = "game.absent";
    }
    SECTION("unknown inventory") {
        policy.inventory.completeness = Network::NetworkReplicationInventoryCompleteness::Unknown;
    }
    SECTION("editor inspection is not activation") {
        policy.use = Network::ReplicationDeclarationUse::EditorInspection;
    }
    Tests::NativeGameplayFailureControl control{0};
    GameModuleHostOptions options{{}, {}};
    options.replicationAdmission = std::move(policy);
    auto candidate = GameModuleHost{std::move(options)}.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    REQUIRE(candidate.HasValue());
    REQUIRE(candidate.Value().diagnostic);
    CHECK(control.control.registered == 1);
    CHECK(control.control.serviceCreated == 0);
    CHECK(control.control.serviceStarted == 0);
    CHECK(control.control.started == 0);
    CHECK(candidate.Value().owner->ActiveServices().empty());
    auto owner = std::move(candidate).Value().owner;
    REQUIRE(owner->TryRetire().HasValue());
    owner.reset();
    CHECK(control.control.destroyed == 1);
}

TEST_CASE("Native admitted descriptors permit actual services and Start with optional unavailable declarations",
          "[gameplay][replication][host]") {
    auto policy = NativeAdmission();
    auto optional = policy.inventory.declarations.front();
    optional.schema.id = Network::ReplicationSchemaId::Create(1002).Value();
    optional.schema.owner.value = "game.absent";
    optional.requirement = Network::ReplicationDeclarationRequirement::Optional;
    policy.inventory.declarations.push_back(std::move(optional));
    policy.use = Network::ReplicationDeclarationUse::ClientActivation;
    Tests::NativeGameplayFailureControl control{0};
    GameModuleHostOptions options{{}, {}};
    options.replicationAdmission = std::move(policy);
    auto candidate = GameModuleHost{std::move(options)}.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    REQUIRE(candidate.HasValue());
    REQUIRE_FALSE(candidate.Value().diagnostic);
    CHECK(control.control.serviceCreated == 1);
    CHECK(control.control.serviceStarted == 1);
    CHECK(control.control.started == 1);
    auto owner = std::move(candidate).Value().owner;
    {
        auto lease = owner->Replication().Acquire();
        REQUIRE(lease.HasValue());
        CHECK(owner->TryRetire().HasError());
        CHECK(lease.Value().Descriptors()->Schemas().size() == 1);
        CHECK(control.control.destroyed == 0);
    }
    REQUIRE(owner->TryRetire().HasValue());
    CHECK(control.control.destroyed == 1);
}

TEST_CASE("loaded gameplay declarations participate in durable capture and aggregate restore", "[unit][gameplay][save]") {
    using namespace Horo::Runtime;
    GameModuleHost moduleHost;
    auto loadedResult = moduleHost.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    REQUIRE(loadedResult.HasValue());
    REQUIRE_FALSE(loadedResult.Value().diagnostic);
    auto loaded = std::move(loadedResult).Value().owner;
    auto event = loaded->Events().Acquire(90, 1, 91, 1, loaded->Cancellation());
    REQUIRE(event.HasValue());
    CanonicalStateParticipantRegistry registry;
    NoSaveOperations operations;
    auto participationResult =
        SaveParticipationHost::Create(1436, {.captureParticipants = true, .restoreParticipants = true}, registry, operations);
    REQUIRE(participationResult.HasValue());
    auto participation = std::move(participationResult).Value();
    const auto adapters = RegisterDurableParticipants(*loaded, participation.Client());
    auto installed = RequireNativeInstallation(*loaded, adapters.front());
    auto participants = registry.Snapshot().Value();
    auto snapshot = CaptureDurableParticipants(participants);
    REQUIRE(snapshot.Records().size() == 4);
    auto receipts = StageDurableParticipants(adapters, snapshot);
    RequireRestartRequired(loaded->PrepareReload());
    CHECK_FALSE(installed.Value().CanUse());
    CHECK(installed.Value().AcquireAdapter() == nullptr);
    CHECK(loaded->AcquireInstalledPersistence(adapters.front()->Descriptor().participant.participant).HasError());
    RequireRestartRequired(loaded->AcquirePersistence(adapters.front()->Descriptor().participant.participant));
    CHECK(loaded->Events().Acquire(90, 1, 91, 1, loaded->Cancellation()).HasError());
    CHECK(event.Value()->Invoke({}) == GameplayEventOutcome::CapabilityUnavailable);
    REQUIRE(participation.Close().HasValue());
    loaded.reset();
    auto operation = CreateSaveOperation({.operation = 1436, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4}).Value();
    auto created = StagedRestoreTransaction::Create({.operation = 1436,
                                                     .registryGeneration = participants.Generation(),
                                                     .sessionGeneration = 1,
                                                     .sceneIncarnation = 1,
                                                     .maximumParticipants = 4},
                                                    std::move(operation), participants, std::move(receipts));
    REQUIRE(created.HasValue());
    auto transaction = std::move(created).Value();
    REQUIRE(transaction.Prepare().HasValue());
    REQUIRE(
        transaction.Activate({.registryGeneration = participants.Generation(), .sessionGeneration = 1, .sceneIncarnation = 1}).HasValue());
    auto restoredCapture = RuntimeSaveCaptureBuilder::Create(snapshot.Provenance(), participants).Value();
    REQUIRE(restoredCapture.CaptureParticipants().HasValue());
    auto restored = restoredCapture.Seal().Value();
    for (const auto &record : restored.Records())
        CHECK(record.Segment(0).back() == std::byte{0x42});
    for (const auto &record : snapshot.Records())
        CHECK(record.Segment(0).back() == std::byte{0x31});
}

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
    REQUIRE_FALSE(loaded.Value().diagnostic);
    REQUIRE(loaded.Value().owner->ModuleId() == "game.tests");
    RequireFrozenRegistries(*loaded.Value().owner);
    REQUIRE(loaded.Value().owner->ActiveServices().size() == 1);
    REQUIRE(loaded.Value().owner->Capabilities().size() == 1);
    REQUIRE_FALSE(loaded.Value().owner->Cancellation().IsCancellationRequested());

    {
        auto systems = GameplaySystemRuntime::Create(loaded.Value().owner->Systems(), loaded.Value().owner->ActiveServices(),
                                                     loaded.Value().owner->Capabilities(), loaded.Value().owner->Cancellation());
        REQUIRE(systems.HasValue());
        REQUIRE(systems.Value()->Execute(GameplaySystemPhase::Gameplay, GameplayThreadAffinity::RuntimeOwner, 1.0 / 60.0).HasValue());
        systems.Value()->Shutdown();
    }

    {
        Tests::ActiveBehaviorRuntime active =
            Tests::ActivateBehaviorRuntime(Definition(), SceneRuntimeId{7}, loaded.Value().owner->Registry());
        const GameplayInputAction move{GameplayActionId{"game.tests.move"}, 3.0F, 0.0F, true, true, false};
        REQUIRE(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, SceneObjectId{1}, {&move, 1}).x == 3.0F);
        active.runtime->Shutdown();
    }

    std::unique_ptr<LoadedGameModule> active = std::move(loaded).Value().owner;
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
    REQUIRE_FALSE(replacement.Value().diagnostic);
    REQUIRE(replacement.Value().owner->RestoreReload(reloadSnapshot.Value()).HasValue());
}

TEST_CASE("game module host publishes native replication registrations") {
    GameModuleHost host;
    auto loaded = host.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    REQUIRE(loaded.HasValue());
    REQUIRE_FALSE(loaded.Value().diagnostic);
    REQUIRE(loaded.Value().owner->Replication().IsFrozen());

    auto replication = loaded.Value().owner->Replication().Acquire();
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
        REQUIRE_FALSE(loadedResult.Value().diagnostic);
        auto replication = loadedResult.Value().owner->Replication().Acquire();
        REQUIRE(replication.HasValue());
        std::unique_ptr<LoadedGameModule> loaded = std::move(loadedResult).Value().owner;

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
    REQUIRE_FALSE(loaded.Value().diagnostic);
    std::unique_ptr<LoadedGameModule> module = std::move(loaded).Value().owner;
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

TEST_CASE("combined SDK22 boundary rejects previous native capability layouts before activation", "[unit][gameplay][sdk]") {
    ValidBundleStorage storage;
    const GameModuleLoadExpectation expected{
        .moduleId = "game.tests",
        .buildFingerprint = CurrentGameplayBuildFingerprint(),
        .descriptorRevision = 7,
    };
    GameModuleDescriptor descriptor{
        .moduleId = "game.tests",
        .buildFingerprint = CurrentGameplayBuildFingerprint().data(),
    };
    REQUIRE(ValidateGameModuleDescriptor(descriptor, expected).HasValue());
    REQUIRE(ValidateGeneratedGameplayDescriptorBundle(storage.bundle, expected).HasValue());
    for (const std::uint32_t previousLayout : {6U, 7U, 10U, 11U, 15U, 16U, 17U, 18U, 19U, 20U, 21U}) {
        descriptor.sdkBoundaryVersion = previousLayout;
        storage.bundle.sdkBoundaryVersion = previousLayout;
        const auto oldModule = ValidateGameModuleDescriptor(descriptor, expected);
        const auto oldBundle = ValidateGeneratedGameplayDescriptorBundle(storage.bundle, expected);
        REQUIRE(oldModule.HasError());
        REQUIRE(oldBundle.HasError());
        CHECK(oldModule.ErrorValue().code.Value() == GameplayErrors::IncompatibleGameModule.code.Value());
        CHECK(oldBundle.ErrorValue().code.Value() == GameplayErrors::InvalidGeneratedDescriptorBundle.code.Value());
    }
    // A stale SDK21 build fingerprint remains incompatible even if a descriptor reports the current boundary.
    auto oldFingerprint = std::string{CurrentGameplayBuildFingerprint()};
    const auto generation = oldFingerprint.find("gameplay-sdk22");
    REQUIRE(generation != std::string::npos);
    oldFingerprint.replace(generation, std::string_view{"gameplay-sdk22"}.size(), "gameplay-sdk21");
    descriptor.sdkBoundaryVersion = GameplaySdkBoundaryVersion;
    descriptor.buildFingerprint = oldFingerprint.c_str();
    const auto oldBuild = ValidateGameModuleDescriptor(descriptor, expected);
    REQUIRE(oldBuild.HasError());
    CHECK(oldBuild.ErrorValue().code.Value() == GameplayErrors::IncompatibleGameModule.code.Value());
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

TEST_CASE("event registration SDK rejects prior generation descriptors and bundles") {
    ValidBundleStorage storage;
    const GameModuleLoadExpectation expected{
        .moduleId = "game.tests",
        .buildFingerprint = CurrentGameplayBuildFingerprint(),
        .descriptorRevision = 7,
    };
    GameModuleDescriptor descriptor{
        .moduleId = "game.tests",
        .buildFingerprint = CurrentGameplayBuildFingerprint().data(),
    };
    REQUIRE(ValidateGameModuleDescriptor(descriptor, expected).HasValue());
    REQUIRE(ValidateGeneratedGameplayDescriptorBundle(storage.bundle, expected).HasValue());
    descriptor.sdkBoundaryVersion = GameplaySdkBoundaryVersion - 1;
    const auto module = ValidateGameModuleDescriptor(descriptor, expected);
    REQUIRE(module.HasError());
    CHECK(module.ErrorValue().code.Value() == GameplayErrors::IncompatibleGameModule.code.Value());
    storage.bundle.sdkBoundaryVersion = GameplaySdkBoundaryVersion - 1;
    const auto bundle = ValidateGeneratedGameplayDescriptorBundle(storage.bundle, expected);
    REQUIRE(bundle.HasError());
    CHECK(bundle.ErrorValue().code.Value() == GameplayErrors::InvalidGeneratedDescriptorBundle.code.Value());
}

TEST_CASE("native module restart after releasing generation leases preserves replication semantics") {
    GameModuleHost host;
    const auto schema = Network::ReplicationSchemaId::Create(1001).Value();
    const auto field = Network::FieldId::Create(1).Value();
    Sha256Digest fingerprint;
    Network::ReplicationEncodedValue encodedEvidence;
    GameplayReplicationOwner owner;
    {
        auto first = host.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
        REQUIRE(first.HasValue());
        REQUIRE_FALSE(first.Value().diagnostic);
        std::unique_ptr<LoadedGameModule> loaded = std::move(first).Value().owner;
        auto prior = loaded->Replication().Acquire();
        REQUIRE(prior.HasValue());
        fingerprint = prior.Value().Descriptors()->Fingerprint();
        const auto encoded = prior.Value().Serializers().Encode(schema, field, 3.5);
        REQUIRE(encoded.HasValue());
        encodedEvidence = encoded.Value();
        owner = prior.Value().Registrations().front().owner;
        RequireRestartRequired(loaded->PrepareReload());
        loaded.reset();
        CHECK(prior.Value().Serializers().Encode(schema, field, 3.5).Value() == encodedEvidence);
    }  // Every old native generation lease is released before restart; only copied inert evidence remains.
    auto replacement = host.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    REQUIRE(replacement.HasValue());
    REQUIRE_FALSE(replacement.Value().diagnostic);
    auto current = replacement.Value().owner->Replication().Acquire();
    REQUIRE(current.HasValue());
    CHECK(current.Value().Descriptors()->Fingerprint() == fingerprint);
    CHECK(current.Value().Serializers().Encode(schema, field, 3.5).Value() == encodedEvidence);
    CHECK(current.Value().Registrations().front().owner == owner);
}
