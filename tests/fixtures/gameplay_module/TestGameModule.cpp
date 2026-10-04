#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameAssetTypeRegistry.h"
#include "Horo/Gameplay/GameEventRegistry.h"
#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/GameServiceRegistry.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/NativeBehavior.h"
#include "Horo/Gameplay/PersistenceRegistration.h"
#include "Horo/Gameplay/ReplicationRegistration.h"
#include "Horo/Gameplay/SystemRegistry.h"
#include "gameplay/GameAssetTestSupport.h"
#if defined(HORO_TEST_GAMEPLAY_PHYSICS) && HORO_TEST_GAMEPLAY_PHYSICS
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#endif

#include <algorithm>
#include <format>

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
    class DurableCandidate final : public IPreparedPersistenceState {
    public:
        DurableCandidate(std::vector<std::byte> &active, std::vector<std::byte> candidate)
            : active_(active), candidate_(std::move(candidate)) {}

        void Publish() noexcept override {
            active_.swap(candidate_);
        }

    private:
        std::vector<std::byte> &active_;
        std::vector<std::byte> candidate_;
    };

    class DurableSource final : public IPersistenceSource {
    public:
        Result<std::vector<std::byte>> CaptureRuntimeState(std::uint64_t) const override {
            return Result<std::vector<std::byte>>::Success(active_);
        }

        Result<std::unique_ptr<IPreparedPersistenceState>> PrepareRuntimeState(std::span<const std::byte> bytes) override {
            return Result<std::unique_ptr<IPreparedPersistenceState>>::Success(
                std::make_unique<DurableCandidate>(active_, std::vector<std::byte>{bytes.begin(), bytes.end()}));
        }

    private:
        std::vector<std::byte> active_{std::byte{0x31}};
    };

    Result<void> RegisterDurableState(GameRegistrationContext &context) {
        using namespace Horo::Runtime;
        for (std::uint8_t index = 0; index < 4; ++index) {
            GameplayPersistenceDescriptor descriptor;
            descriptor.participant.participant = SaveParticipantId::Parse(std::format("game.tests.durable{}", index)).Value();
            descriptor.participant.schemaVersion = ParticipantSchemaVersion::Create(1).Value();
            descriptor.participant.scope = index == 3 ? SaveParticipantScope::SlotPlayer : SaveParticipantScope::RuntimeScene;
            descriptor.participant.roles = SaveParticipantRole::Capture | SaveParticipantRole::Restore;
            descriptor.participant.limits = {128, 1, 4};
            SaveIdentityDetail::Bytes record{};
            record.back() = index + 1;
            descriptor.record = SaveRecordId::FromBytes(record).Value();
            descriptor.participant.ownedRecords = {descriptor.record};
            descriptor.owner = static_cast<GameplayPersistenceOwner>(index);
            descriptor.moduleId = SaveParticipantId::Parse("game.tests").Value();
            descriptor.moduleVersion = 1;
            PersistenceOwnerIdentity owner;
            if (index == 0)
                owner = BehaviorTypeId::Parse("game.tests.dynamic_mover").Value();
            else if (index == 2)
                owner = GameplayServiceId::Parse("game.tests.session_service").Value();
            if (auto registered = context.persistence.Register({descriptor, owner, std::make_shared<DurableSource>()});
                registered.HasError())
                return registered;
        }
        return Result<void>::Success();
    }

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
            if (auto registered = context.events.Register({90, 1, 91, 1, BorrowedCallbackContext{this}, &CompleteQuest});
                registered.HasError())
                return registered;
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
            if (auto registered = context.systems.Register(std::move(system)); registered.HasError())
                return registered;
            return RegisterDurableState(context);
        }

        Result<void> Start(GameRuntimeContext &context) override {
#if defined(HORO_TEST_GAMEPLAY_PHYSICS) && HORO_TEST_GAMEPLAY_PHYSICS
            if (!context.physics)
                return Result<void>::Failure(MakeError(GameplayErrors::PhysicsUnavailable));
            const auto &binding = context.physics->Binding();
            const auto physics = context.physics->Acquire("game.tests", binding.scene, binding.sceneGeneration);
            if (binding.permissionGranted && binding.moduleEnabled && physics.HasError() &&
                physics.ErrorValue().code.Value() != GameplayErrors::PhysicsUnavailable.code.Value())
                return Result<void>::Failure(physics.ErrorValue());
            if (!binding.permissionGranted && physics.HasValue())
                return Result<void>::Failure(MakeError(GameplayErrors::PhysicsPermissionDenied));
#endif
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

    private:
        static GameplayEventOutcome CompleteQuest(const BorrowedCallbackContext &context, const GameplayEventRequest &request) {
            using enum GameplayEventOutcome;
            auto *instance = context.Get<Module>();
            if (instance == nullptr)
                return InvalidTarget;
            auto &gameModule = *instance;
            if (request.payload.size() < sizeof(std::uint64_t) || request.committedTick == 0 || request.traversal == 0)
                return InvalidTarget;
            // The fixture's cooked contract is a one-argument canonical array of signed integers.
            // Inspect its little-endian scalar without requiring a VM or extension runtime in the gameplay SDK.
            std::uint64_t quest{};
            const auto scalar = request.payload.last(sizeof(quest));
            for (std::size_t byte = 0; byte < scalar.size(); ++byte)
                quest |= static_cast<std::uint64_t>(std::to_integer<unsigned>(scalar[byte])) << (byte * 8U);
            if (quest != 42)
                return InvalidTarget;
            ++gameModule.completedQuests_;
            return Accepted;
        }

        std::uint64_t completedQuests_{};
    };

}  // namespace

extern "C" HORO_GAME_EXPORT IGameModule *CreateGameModule() noexcept {
    return new Module{};
}

extern "C" HORO_GAME_EXPORT void DestroyGameModule(IGameModule *gameModule) noexcept {
    delete gameModule;
}
