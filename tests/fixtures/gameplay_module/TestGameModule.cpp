#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameAssetTypeRegistry.h"
#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/GameServiceRegistry.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/NativeBehavior.h"
#include "Horo/Gameplay/ReplicationRegistration.h"
#include "Horo/Gameplay/SystemRegistry.h"
#include "gameplay/GameAssetTestSupport.h"

#include <algorithm>

using namespace Horo;
using namespace Horo::Gameplay;

class MoveBehavior final : public IBehaviorInstance {
public:
    static BehaviorDescriptor DescribeBehavior() {
        BehaviorDescriptor descriptor;
        descriptor.displayName = "Dynamic Mover";
        descriptor.phases.push_back({BehaviorPhase::Gameplay, "game.tests.dynamic_mover", {}, {}, {}});
        return descriptor;
    }

    void OnFixedUpdate(BehaviorContext &context, FixedDeltaTime) override {
        const auto actions = context.InputActions();
        if (actions.empty() || !actions.front().down)
            return;
        auto transform = context.LocalTransform();
        if (transform.HasError())
            return;
        Math::Transform moved = transform.Value();
        moved.translation.x += actions.front().x;
        static_cast<void>(context.SetLocalTransform(moved));
    }
};

HORO_BEHAVIOR(MoveBehavior, "game.tests.dynamic_mover")

namespace {
    class TestProjectService final : public IGameplayService {
    public:
        Result<void> Start(const GameplayServiceContext &context) override {
            return context.cancellation.IsCancellationRequested() ? Result<void>::Failure(MakeError(GameplayErrors::GameplayCancelled))
                                                                  : Result<void>::Success();
        }

        void Stop(const GameplayServiceContext &) noexcept override {}
    };

    class TestGameplaySystem final : public IGameplaySystem {
    public:
        Result<void> Start(const GameplaySystemContext &) override {
            return Result<void>::Success();
        }

        Result<void> Execute(const GameplaySystemContext &context) override {
            return context.cancellation.IsCancellationRequested() ? Result<void>::Failure(MakeError(GameplayErrors::GameplayCancelled))
                                                                  : Result<void>::Success();
        }

        void Stop(const GameplaySystemContext &) noexcept override {}
    };

    IGameplayService *CreateTestProjectService(void *) {
        return new TestProjectService{};
    }

    void DestroyTestProjectService(void *, IGameplayService *service) noexcept {
        delete service;
    }

    IGameplaySystem *CreateTestGameplaySystem(void *) {
        return new TestGameplaySystem{};
    }

    void DestroyTestGameplaySystem(void *, IGameplaySystem *system) noexcept {
        delete system;
    }

    Result<SerializedGameAsset> ImportTestAsset(void *, const GameAssetImportInput &input, const CancellationToken &) {
        return Result<SerializedGameAsset>::Success({
            .typeId = GameAssetTypeId::Parse("game.tests.quest_definition").Value(),
            .schemaVersion = 1,
            .encoding = GameAssetPayloadEncoding::CanonicalJson,
            .payload = {input.sourceBytes.begin(), input.sourceBytes.end()},
        });
    }

    Result<SerializedGameAsset> SerializeTestAsset(void *, const GameAssetSerializationInput &input, const CancellationToken &) {
        return Result<SerializedGameAsset>::Success({
            .typeId = GameAssetTypeId::Parse("game.tests.quest_definition").Value(),
            .schemaVersion = 1,
            .encoding = input.encoding,
            .payload = {input.editorPayload.begin(), input.editorPayload.end()},
        });
    }

    Result<std::vector<std::byte>> CookTestAsset(void *, const GameAssetCookInput &input, const CancellationToken &) {
        return Result<std::vector<std::byte>>::Success(input.asset.payload);
    }

    class Module final : public IGameModule {
    public:
        Result<void> Register(GameRegistrationContext &context) override {
            const ComponentTypeId movementType = ComponentTypeId::Parse("game.tests.movement_settings").Value();
            ComponentDescriptor descriptor{
                .typeId = movementType,
                .schemaVersion = 2,
                .displayName = "Movement Settings",
                .category = "Gameplay/Movement",
                .properties = {{ComponentPropertyId::Parse("speed").Value(), "Speed", ComponentPropertyKind::Number, true}},
                .migrations = {{1, 2}},
            };
            if (Result<void> component = context.components.Register(std::move(descriptor)); component.HasError())
                return component;

            const auto schemaId = Network::ReplicationSchemaId::Create(1001).Value();
            const auto valueType = Network::ReplicationValueTypeId::Create(1).Value();
            const auto codec = Network::ReplicationCodecId::Create(1).Value();
            GameplayReplicationRegistration replication{
                .owner = movementType,
                .schema =
                    {
                        .id = schemaId,
                        .version = {1, 0},
                        .compatibility = {{1, 0}, {1, 0}},
                        .owner = {.value = "game.tests"},
                        .fields = {{.id = Network::FieldId::Create(1).Value(),
                                    .valueType = valueType,
                                    .codec = codec,
                                    .introducedVersion = {1, 0},
                                    .limits = {8, 1}}},
                    },
                .serializers = {Network::CanonicalScalarReplicationSerializer::Create(
                                    {.valueType = valueType,
                                     .codec = codec,
                                     .owner = {.value = "game.tests"},
                                     .valueKind = Network::ReplicationValueKind::FloatingPoint,
                                     .maximumEncodedBytes = 8,
                                     .maximumElementCount = 1})
                                    .Value()},
                .schedule =
                    {
                        .captureAccess = {.reads = {movementType}},
                        .applyAccess = {.writes = {movementType}},
                    },
            };
            if (Result<void> registered = context.replication.Register(std::move(replication)); registered.HasError())
                return registered;

            GameAssetTypeRegistration asset{
                .descriptor = Tests::QuestGameAssetDescriptor(),
                .handler =
                    {
                        .importAsset = &ImportTestAsset,
                        .serializeAsset = &SerializeTestAsset,
                        .cookAsset = &CookTestAsset,
                    },
            };
            if (Result<void> registered = context.assetTypes.Register(std::move(asset)); registered.HasError())
                return registered;

            GameplayServiceRegistration service{
                .descriptor =
                    {
                        .id = GameplayServiceId::Parse("game.tests.session_service").Value(),
                        .scope = GameplayServiceScope::Project,
                        .affinity = GameplayThreadAffinity::RuntimeOwner,
                        .sceneReplacement = GameplaySceneReplacementPolicy::Preserve,
                        .providedCapabilities = {GameplayCapabilityId::Parse("game.tests.session.read").Value()},
                        .observabilityCategory = "game.tests.gameplay",
                    },
                .factory = {.create = &CreateTestProjectService, .destroy = &DestroyTestProjectService},
            };
            if (Result<void> registered = context.services.Register(std::move(service)); registered.HasError())
                return registered;

            GameplaySystemRegistration system{
                .descriptor =
                    {
                        .id = GameplaySystemId::Parse("game.tests.session_system").Value(),
                        .phase = GameplaySystemPhase::Gameplay,
                        .affinity = GameplayThreadAffinity::RuntimeOwner,
                        .requiredServices = {GameplayServiceId::Parse("game.tests.session_service").Value()},
                        .requiredCapabilities = {GameplayCapabilityId::Parse("game.tests.session.read").Value()},
                    },
                .factory = {.create = &CreateTestGameplaySystem, .destroy = &DestroyTestGameplaySystem},
            };
            return context.systems.Register(std::move(system));
        }

        Result<void> Start(GameRuntimeContext &context) override {
            const GameplayServiceId service = GameplayServiceId::Parse("game.tests.session_service").Value();
            const GameplayCapabilityId capability = GameplayCapabilityId::Parse("game.tests.session.read").Value();
            if (context.cancellation.IsCancellationRequested() ||
                std::ranges::find(context.activeServices, service) == context.activeServices.end() ||
                std::ranges::find(context.capabilities, capability) == context.capabilities.end())
                return Result<void>::Failure(MakeError(GameplayErrors::CapabilityMissing));
            return Result<void>::Success();
        }

        void Stop(GameRuntimeContext &) noexcept override {}

        Result<GameModuleReloadSnapshot> PrepareReload(GameRuntimeContext &context) override {
            if (!context.cancellation.IsCancellationRequested())
                return Result<GameModuleReloadSnapshot>::Failure(MakeError(GameplayErrors::GameplayReloadRestartRequired));
            return Result<GameModuleReloadSnapshot>::Success({1, {}});
        }

        Result<void> RestoreReload(const GameModuleReloadSnapshot &snapshot, GameRuntimeContext &) override {
            if (snapshot.schemaVersion != 1 || !snapshot.payload.empty())
                return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadRestoreFailed));
            return Result<void>::Success();
        }
    };

}  // namespace

extern "C" HORO_GAME_EXPORT IGameModule *CreateGameModule() noexcept {
    return new Module{};
}

extern "C" HORO_GAME_EXPORT void DestroyGameModule(IGameModule *module) noexcept {
    delete module;
}
