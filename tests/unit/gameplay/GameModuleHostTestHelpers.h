#pragma once

#include "../runtime/scene/SaveContentWorldTestHelpers.h"
#include "GameplayModuleTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameAssetTypeRegistry.h"
#include "Horo/Gameplay/GameEventRegistry.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/GameplayRegistrationRuntime.h"
#include "Horo/Gameplay/ReplicationRegistration.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveParticipation.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <format>
#include <future>

namespace Horo::Gameplay::HostTest {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Runtime;

    class NoSaveOperations final : public ISaveParticipationOperationHost {
    public:
        Result<SaveOperationHandle> RequestSave(const SaveParticipationSaveRequest &) override {
            return Result<SaveOperationHandle>::Failure(MakeError(SaveErrors::LifecycleUnavailable));
        }

        Result<SaveOperationHandle> RequestLoad(const SaveParticipationLoadRequest &) override {
            return Result<SaveOperationHandle>::Failure(MakeError(SaveErrors::LifecycleUnavailable));
        }
    };

    inline GameModuleLoadExpectation Expectation() {
        return {
            .moduleId = "game.tests",
            .buildFingerprint = CurrentGameplayBuildFingerprint(),
            .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
        };
    }

    inline IGameModule *CreateTestModule() noexcept {
        return nullptr;
    }

    inline void DestroyTestModule(IGameModule *) noexcept {}

    inline IBehaviorInstance *CreateTestBehavior(void *) {
        return nullptr;
    }

    inline void DestroyTestBehavior(void *, IBehaviorInstance *) noexcept {}

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

    inline RuntimeSceneDefinition Definition() {
        return Tests::SingleBehaviorSceneDefinition(SceneDefinitionId{3}, SceneDefinitionRevision{1}, SceneObjectId{1},
                                                    BehaviorInstanceId{1}, BehaviorTypeId::Parse("game.tests.dynamic_mover").Value());
    }

    template <typename Value> void RequireRestartRequired(const Result<Value> &result) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == GameplayErrors::GameplayReloadRestartRequired.code.Value());
    }

    template <typename CreateRuntime, typename ExerciseRuntime>
    inline void CheckGenerationLease(CreateRuntime createRuntime, ExerciseRuntime exerciseRuntime) {
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

    inline void RequireFrozenRegistries(const LoadedGameModule &loaded) {
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

    /** @brief Explicitly composes all four module declarations with a save participation host. */
    [[nodiscard]] inline std::vector<std::shared_ptr<GameplayPersistenceAdapter>> RegisterDurableParticipants(
        const LoadedGameModule &loaded, const SaveParticipationClient &client) {
        std::vector<std::shared_ptr<GameplayPersistenceAdapter>> adapters;
        for (unsigned index = 0; index < 4; ++index) {
            auto adapter = loaded.AcquirePersistence(SaveParticipantId::Parse(std::format("game.tests.durable{}", index)).Value());
            REQUIRE(adapter.HasValue());
            REQUIRE(client.RegisterParticipant(adapter.Value()->Descriptor().participant, adapter.Value()).HasValue());
            adapters.push_back(std::move(adapter).Value());
        }
        return adapters;
    }

    /** @brief Stages detached archive bytes without altering the original capture snapshot. */
    [[nodiscard]] inline std::vector<std::unique_ptr<IStagedRestoreParticipant>> StageDurableParticipants(
        const std::span<const std::shared_ptr<GameplayPersistenceAdapter>> adapters, const RuntimeSaveSnapshot &snapshot) {
        std::vector<std::unique_ptr<IStagedRestoreParticipant>> receipts;
        for (std::size_t index = 0; index < adapters.size(); ++index) {
            const auto &record = snapshot.Records()[index];
            CHECK(record.Segment(0).back() == std::byte{0x31});
            std::vector<std::byte> detached{record.Segment(0).begin(), record.Segment(0).end()};
            detached.back() = std::byte{0x42};
            auto receipt = adapters[index]->StageRestore(adapters[index]->Descriptor().participant.schemaVersion,
                                                         adapters[index]->Descriptor().record, detached);
            REQUIRE(receipt.HasValue());
            receipts.push_back(std::move(receipt).Value());
        }
        return receipts;
    }

    /** @brief Captures registered module records with valid scene and generation provenance. */
    [[nodiscard]] inline RuntimeSaveSnapshot CaptureDurableParticipants(const SaveParticipantRegistrySnapshot &participants) {
        SaveIdentityDetail::Bytes captureId{};
        captureId.back() = 1;
        auto builder = RuntimeSaveCaptureBuilder::Create({.capturedState = CapturedStateId::FromBytes(captureId).Value(),
                                                          .epoch = CanonicalCaptureEpoch{1},
                                                          .sceneIncarnation = 1,
                                                          .sceneRevision = 1,
                                                          .registryGeneration = participants.Generation()},
                                                         participants)
                           .Value();
        REQUIRE(builder.CaptureParticipants().HasValue());
        return builder.Seal().Value();
    }

    /** @brief Qualifies private native installation authority against the actual registered persistence adapter. */
    inline Result<GameplayPersistenceInstallation> RequireNativeInstallation(const LoadedGameModule &loaded,
                                                                             const std::shared_ptr<GameplayPersistenceAdapter> &adapter) {
        auto installed = loaded.AcquireInstalledPersistence(adapter->Descriptor().participant.participant);
        REQUIRE(installed.HasValue());
        CHECK(installed.Value().CanUse());
        CHECK(installed.Value().Descriptor().moduleId == adapter->Descriptor().moduleId);
        CHECK(installed.Value().Descriptor().moduleVersion == adapter->Descriptor().moduleVersion);
        REQUIRE(installed.Value().AcquireAdapter() != nullptr);
        CHECK(loaded.AcquireInstalledPersistence(SaveParticipantId::Parse("game.tests.absent").Value()).HasError());
        CHECK(loaded.AcquirePersistence(SaveParticipantId::Parse("game.tests.absent").Value()).ErrorValue().code.Value() ==
              SaveErrors::ParticipantAdapterMissing.code.Value());
        return installed;
    }

}  // namespace Horo::Gameplay::HostTest
