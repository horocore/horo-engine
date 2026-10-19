#pragma once
/** @file PrefabAtomicOwnerTestSupport.h
 * @brief Real artifact/provider/native Physics/AI/module composition for atomic runtime template acceptance. */
#include "AiSceneTestSupport.h"
#include "CommittedSceneAssets.h"
#include "GameplayModuleTestSupport.h"
#include "GameplayWorldComposition.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/Runtime/Scene/BuiltinSceneComponents.h"
#include "Horo/Scene/SceneSource.h"
#include "PhysicsSceneActivationTestSupport.h"
#include "ScenePhysicsHost.h"
#include "assets/AssetCookOutputFixture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Application::PrefabAtomicTest {
    using Internal::CommittedSceneAssets;
    using Internal::GameplayPhysicsPermission;
    using Internal::GameplayPrefabPermission;
    using Internal::GameplayWorldComposition;
    using Internal::GameplayWorldSelection;
    using Internal::MakeGameplayStructuralParticipant;
    using Internal::ScenePhysicsHost;
    using Kind = Runtime::BuiltinSceneComponentKind;

    inline Gameplay::BehaviorTypeId BehaviorType(std::string_view name) {
        return Gameplay::BehaviorTypeId::Parse(name).Value();
    }

    /** @brief Encodes through the real Scene source codec; tests do not maintain another typed field interpretation. */
    inline Prefab::RawComponentPayload Builtin(const SceneSource::SceneObjectComponentSet &components, Kind kind, std::string_view field,
                                               std::uint64_t instance, bool collection = false) {
        SceneSource::SceneObjectSnapshot object{.id = {1}, .name = "Typed payload", .components = components};
        const std::array objects{object};
        auto encoded = nlohmann::json::parse(SceneSource::EncodeSceneSource({objects, {}}));
        auto value = encoded.at("objects").at(0).at("components").at(std::string(field));
        if (collection)
            value = value.at(0);
        std::string_view type;
        for (const auto &schema : Runtime::BuiltinSceneComponentSchemas())
            if (schema.kind == kind)
                type = schema.typeId;
        REQUIRE_FALSE(type.empty());
        Prefab::RawComponentPayload raw{.instance = Prefab::PrefabComponentInstanceId::Create(instance).Value(),
                                        .component = {.typeId = Gameplay::ComponentTypeId::Parse(type).Value(),
                                                      .schemaVersion = 1,
                                                      .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson}};
        for (const auto byte : value.dump())
            raw.component.payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
        return raw;
    }

    inline Sha256Digest Put(Assets::MemoryAssetProvider &provider, Assets::AssetId asset, const Assets::AssetTypeId &type,
                            const AssetCookTargetId &target, std::vector<std::uint8_t> payload) {
        const auto digest = ComputeSha256(std::as_bytes(std::span(payload)));
        auto encoded = Assets::EncodeCookedArtifact({asset, type, target, {}, {}, digest, std::move(payload)});
        REQUIRE(encoded.HasValue());
        const auto envelope = ComputeSha256(std::as_bytes(std::span(encoded.Value())));
        provider.Insert(asset, std::move(encoded).Value());
        return envelope;
    }

#if HORO_TEST_PHYSICS_NATIVE
    enum class Fault {
        None,
        Material,
        MaterialIdentity,
        AiDescriptor,
        GameplayDescriptor,
        DuplicateAgent,
        BodyEndpoint,
        Navigation,
        DisabledNavigation,
        DuplicateFacts,
        ForeignFacts,
        MissingFacts,
        DuplicateProducer
    };

    /** @brief Deliberately corrupts facts around a real AI candidate to test Scene's independent host-authority fence. */
    class InvalidIdentityCandidate final : public Runtime::SceneStructuralCandidate {
    public:
        InvalidIdentityCandidate(std::unique_ptr<Runtime::SceneStructuralCandidate> owner, Fault fault) : owner_(std::move(owner)) {
            using enum Fault;
            const auto original = owner_->AgentIdentities();
            facts_.assign(original.begin(), original.end());
            REQUIRE_FALSE(facts_.empty());
            if (fault == DuplicateFacts)
                facts_.push_back(facts_.front());
            else if (fault == ForeignFacts)
                ++facts_.front().entity.runtime.value;
            else if (fault == MissingFacts)
                facts_.pop_back();
        }

        std::span<const Runtime::SceneAgentIdentityProjection> AgentIdentities() const noexcept override {
            return facts_;
        }

        Result<void> ValidatePublication() const override {
            return owner_->ValidatePublication();
        }

        void Publish() noexcept override {
            owner_->Publish();
        }

        Result<void> AfterPublication() override {
            return owner_->AfterPublication();
        }

    private:
        std::unique_ptr<Runtime::SceneStructuralCandidate> owner_;
        std::vector<Runtime::SceneAgentIdentityProjection> facts_;
    };

    class InvalidIdentityParticipant final : public Runtime::SceneStructuralParticipant {
    public:
        InvalidIdentityParticipant(std::unique_ptr<Runtime::SceneStructuralParticipant> owner, Fault fault)
            : owner_(std::move(owner)), fault_(fault) {}

        Runtime::SceneStructuralOwner Owner() const noexcept override {
            return owner_->Owner();
        }

        Runtime::SceneStructuralPreparationPhase PreparationPhase() const noexcept override {
            return owner_->PreparationPhase();
        }

        Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(Runtime::RuntimeSceneView active,
                                                                           std::span<const Runtime::RuntimeEntityView> created,
                                                                           std::span<const Runtime::EntityRef> destroyed) override {
            auto prepared = owner_->Prepare(active, created, destroyed);
            if (prepared.HasError())
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(prepared.ErrorValue());
            return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Success(
                std::make_unique<InvalidIdentityCandidate>(std::move(prepared).Value(), fault_));
        }

    private:
        std::unique_ptr<Runtime::SceneStructuralParticipant> owner_;
        Fault fault_;
    };

    /** @brief Borrows only the immutable app capture; no worker is submitted before capture admission. */
    class CapturedEnvelopeProvider final : public Assets::IAssetProvider {
    public:
        std::unique_ptr<CommittedSceneAssets> capture;

        Result<bool> Exists(Assets::AssetId id, const CancellationToken &token) const override {
            return capture->ArtifactProvider().Exists(id, token);
        }

        Result<std::vector<std::uint8_t>> Load(Assets::AssetId id, const CancellationToken &token) const override {
            return capture->ArtifactProvider().Load(id, token);
        }
    };

    struct Fixture final {
        const Assets::AssetId asset{Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value()};
        const Assets::AssetId material{Physics::Asset(91)};
        const AssetCookTargetId target{AssetCookTargetId::Parse("linux-x64").Value()};
        Assets::MemoryAssetProvider bytes;
        Assets::OutputTestSupport::TempDir committedRoot;
        CapturedEnvelopeProvider committed;
        JobSystem jobs{{1, 32}};
        Assets::AssetLoadService loads{jobs, committed};
        Assets::AssetRegistry registry;
        std::unique_ptr<Physics::PhysicsRuntime> physics{
            std::move(Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical)).Value()};
        AI::TestSupport::ActivationFixture controller{AI::TestSupport::MakeFixture(1, 11, 1, 0)};
        std::array<AI::AiControllerDescriptor, 1> catalog{controller.descriptor};
        AI::AiSceneRuntime ai{std::move(AI::AiSceneRuntime::Create()).Value()};
        Physics::PhysicsSceneActivationAuthority authority;
        ScenePhysicsHost native;
        Runtime::RuntimeSceneService scenes{registry, loads};
        Prefab::PrefabTemplateProvider templates{registry, loads, scenes, Prefab::PrefabLimitProfile::Create({}).Value()};
        std::shared_ptr<Gameplay::GameModuleRetirementQueue> retirements{std::make_shared<Gameplay::GameModuleRetirementQueue>()};
        std::unique_ptr<GameplayWorldComposition> gameplay;
        GameplayWorldComposition *activeGameplay{};
        CancellationSource cancellation;
        Sha256Digest artifact;
        Sha256Digest materialArtifact;
        std::uint64_t frame{};

        explicit Fixture(bool aiFirst = false, Fault fault = Fault::None) {
            PublishRegistry();
            PublishMaterial(fault);
            artifact = PublishTemplate(fault);
            CaptureCommittedGeneration();
            RegisterOwners(aiFirst, fault);
            REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
            REQUIRE(scenes.QueuePreparation(InitialScene()).HasValue());
            Pump();
            REQUIRE(scenes.ActiveScene().has_value());
            REQUIRE(templates.Startup(cancellation.Token()).HasValue());
            ActivateGameplay();
        }

        ~Fixture() {
            if (gameplay)
                gameplay->Shutdown();
            activeGameplay = nullptr;
            gameplay.reset();
            templates.Shutdown();
            scenes.Shutdown();
            ai.BeginShutdown();
            (void)retirements->Drain();
            loads.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Drain);
            physics->Shutdown();
        }

        void PublishRegistry() {
            const auto prefab = Assets::AssetTypeId::Parse("core.prefab").Value();
            const auto materialType = Assets::AssetTypeId::Parse("core.physics_material").Value();
            REQUIRE(registry
                        .Publish({{asset, prefab, ProjectPath::Parse("assets/spawn.prefab").Value(),
                                   ProjectPath::Parse("assets/spawn.prefab.horo").Value()},
                                  {material, materialType, ProjectPath::Parse("assets/material.horoasset").Value(),
                                   ProjectPath::Parse("assets/material.horoasset.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
        }

        void PublishMaterial(Fault fault) {
            auto value = Physics::PhysicsMaterialAsset::Create(
                {.id = Physics::PhysicsMaterialAssetId::FromAssetId(fault == Fault::MaterialIdentity ? asset : material),
                 .sourceRevision = 1});
            REQUIRE(value.HasValue());
            auto encoded = Physics::EncodePhysicsMaterialAsset(value.Value());
            REQUIRE(encoded.HasValue());
            std::vector<std::uint8_t> payload;
            for (const auto byte : encoded.Value())
                payload.push_back(std::to_integer<std::uint8_t>(byte));
            if (fault == Fault::Material)
                payload = {1, 2, 3};
            materialArtifact =
                Put(bytes, material, Assets::AssetTypeId::Parse("core.physics_material").Value(), target, std::move(payload));
        }

        /** @brief Publishes the actual envelopes once, then routes every load through the production app snapshot. */
        void CaptureCommittedGeneration() {
            std::vector<Assets::AssetCookManifestEntry> entries;
            std::vector<std::vector<std::uint8_t>> envelopes;
            std::array ids{asset, material};
            std::ranges::sort(ids, {}, &Assets::AssetId::ToString);
            for (const auto id : ids) {
                auto loaded = bytes.Load(id, {});
                REQUIRE(loaded.HasValue());
                auto decoded = Assets::DecodeCookedArtifact(loaded.Value());
                REQUIRE(decoded.HasValue());
                entries.emplace_back(id, decoded.Value().type, id.ToString() + ".cooked",
                                     ComputeSha256(std::as_bytes(std::span(loaded.Value()))));
                envelopes.push_back(std::move(loaded).Value());
            }
            REQUIRE(Assets::OutputTestSupport::PublishFixture(committedRoot.path, target, entries, envelopes).HasValue());
            auto captured = CommittedSceneAssets::Capture({committedRoot.path, target}, registry.Snapshot());
            REQUIRE(captured.HasValue());
            committed.capture = std::move(captured).Value();
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto retained = committed.capture->ArtifactProvider().Load(entries[index].assetId, {});
                REQUIRE(retained.HasValue());
                CHECK(retained.Value() == envelopes[index]);
                CHECK(ComputeSha256(std::as_bytes(std::span(retained.Value()))) == entries[index].artifactHash);
            }
        }

        Prefab::CookedPrefabEntity Root() const {
            Prefab::CookedPrefabEntity root;
            root.provenance = {asset, {}, ComputeSha256(std::as_bytes(std::span("source", 6)))};
            root.members.emplace_back(Gameplay::BehaviorComponent{{5}, BehaviorType("game.tests.dynamic_mover")});
            SceneSource::SceneObjectComponentSet values{.aiAgent = controller.agent.agent, .aiController = controller.agent.controller};
            root.members.emplace_back(Builtin(values, Kind::AiAgent, "aiAgent", 101));
            root.members.emplace_back(Builtin(values, Kind::AiController, "aiController", 102));
            return root;
        }

        Prefab::CookedPrefabEntity Child(const Prefab::CookedPrefabEntity &root, Fault fault) const {
            using enum Fault;
            auto child = root;
            child.parent = Prefab::CookedPrefabEntitySlot{0};
            child.provenance.sourceObject = Prefab::PrefabObjectAddress::Create({}, {7}).Value();
            std::get<Gameplay::BehaviorComponent>(child.members[0]).instanceId.value = 6;
            auto agent = controller.agent.agent;
            agent.agent = AI::TestSupport::MakeIdentity<AI::AgentId>(fault == DuplicateAgent ? 1 : 2);
            SceneSource::SceneObjectComponentSet values{.aiAgent = agent,
                                                        .rigidBody = Runtime::RigidBodyComponent{.id = {200}, .body = {1}},
                                                        .colliders = {Physics::Collider(201, 1, fault == BodyEndpoint ? 99 : 7, 1, material,
                                                                                        Runtime::PhysicsAnalyticCollider{
                                                                                            Runtime::PhysicsBoxCollider{}})}};
            child.members[1] = Builtin(values, Kind::AiAgent, "aiAgent", 101);
            if (fault == AiDescriptor) {
                values.aiController = controller.agent.controller;
                values.aiController->controller = AI::TestSupport::MakeIdentity<AI::ControllerTypeId>(99);
                child.members[2] = Builtin(values, Kind::AiController, "aiController", 102);
            }
            if (fault == GameplayDescriptor)
                std::get<Gameplay::BehaviorComponent>(child.members[0]).typeId = BehaviorType("game.tests.missing");
            child.members.emplace_back(Builtin(values, Kind::RigidBody, "rigidBody", 201));
            child.members.emplace_back(Builtin(values, Kind::Collider, "colliders", 202, true));
            if (fault == Navigation || fault == DisabledNavigation) {
                values.navigationAgent =
                    Runtime::NavigationAgentComponent{.profile = Navigation::NavigationAgentProfileId::Create(7).Value(),
                                                      .filter = Navigation::NavigationFilterId::Create(9).Value(),
                                                      .enabled = fault == Navigation};
                child.members.emplace_back(Builtin(values, Kind::NavigationAgent, "navigationAgent", 203));
            }
            return child;
        }

        Sha256Digest PublishTemplate(Fault fault) {
            Prefab::CookedPrefabData data;
            data.assetId = asset;
            const auto root = Root();
            data.entities = {root, Child(root, fault)};
            data.dependencies = {{{material, Assets::AssetTypeId::Parse("core.physics_material").Value()}, materialArtifact}};
            auto cooked = Prefab::CookedPrefab::Create(std::move(data), Prefab::PrefabLimitProfile::Create({}).Value());
            REQUIRE(cooked.HasValue());
            std::vector<std::uint8_t> payload;
            for (const auto byte : cooked.Value().Bytes())
                payload.push_back(std::to_integer<std::uint8_t>(byte));
            return Put(bytes, asset, Assets::AssetTypeId::Parse("core.prefab").Value(), target, std::move(payload));
        }

        std::unique_ptr<Runtime::SceneStructuralParticipant> AiOwner(std::span<const AI::AiControllerDescriptor> descriptors, Fault fault) {
            using enum Fault;
            auto owner = ai.MakeStructuralParticipant(descriptors);
            if (fault == DuplicateFacts || fault == ForeignFacts || fault == MissingFacts)
                return std::make_unique<InvalidIdentityParticipant>(std::move(owner), fault);
            return owner;
        }

        void RegisterOwners(bool aiFirst, Fault fault) {
            auto initial = native.MakeParticipant(*physics, authority, Physics::Settings());
            REQUIRE(initial.HasValue());
            REQUIRE(scenes.AddActivationParticipant(std::move(initial).Value()).HasValue());
            REQUIRE(scenes.AddActivationParticipant(std::make_unique<AI::AiSceneActivationParticipant>(ai, catalog)).HasValue());
            const std::span<const AI::AiControllerDescriptor> structuralCatalog =
                fault == Fault::AiDescriptor ? std::span<const AI::AiControllerDescriptor>{}
                                             : std::span<const AI::AiControllerDescriptor>{catalog};
            if (aiFirst)
                REQUIRE(scenes.AddStructuralParticipant(AiOwner(structuralCatalog, fault)).HasValue());
            auto structural = native.MakeStructuralParticipant();
            REQUIRE(structural.HasValue());
            REQUIRE(scenes.AddStructuralParticipant(std::move(structural).Value()).HasValue());
            REQUIRE(scenes.AddStructuralParticipant(MakeGameplayStructuralParticipant(activeGameplay)).HasValue());
            if (!aiFirst)
                REQUIRE(scenes.AddStructuralParticipant(AiOwner(structuralCatalog, fault)).HasValue());
            if (fault == Fault::DuplicateProducer)
                REQUIRE(scenes.AddStructuralParticipant(ai.MakeStructuralParticipant(catalog)).HasError());
        }

        Runtime::RuntimeSceneDefinition InitialScene() const {
            Runtime::SceneDefinitionBuilder builder{{7}, {1}};
            Runtime::RuntimeEntityDefinition spawner{.object = {1},
                                                     .components = {.aiAgent = controller.agent.agent,
                                                                    .aiController = controller.agent.controller}};
            spawner.components.behaviors.push_back({{1},
                                                    BehaviorType("game.tests.prefab_spawner"),
                                                    1,
                                                    true,
                                                    {{"asset", asset.ToString()}, {"digest", FormatSha256(artifact)}}});
            builder.Add(std::move(spawner));
            return std::move(builder).Build().Value();
        }

        void ActivateGameplay() {
            auto scope = native.Borrow();
            REQUIRE(scope.HasValue());
            GameplayWorldSelection selection{.moduleId = "game.tests",
                                             .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                             .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                             .physicsPermission = GameplayPhysicsPermission::Granted,
                                             .prefabPermission = GameplayPrefabPermission::Granted,
                                             .moduleRetirements = retirements};
            auto composed = GameplayWorldComposition::Create(scenes, templates, &scope.Value().physics, 1, selection, nullptr,
                                                             &scope.Value().character);
            REQUIRE(composed.HasValue());
            gameplay = std::move(composed).Value();
            activeGameplay = gameplay.get();
        }

        void Pump() {
            const Runtime::FrameContext context{.frameNumber = ++frame, .cancellation = cancellation.Token()};
            const auto phase = Runtime::RuntimePhase::CommitDeferredLifecycleChanges;
            auto result = gameplay ? gameplay->OnPhase(phase, context) : scenes.OnPhase(phase, context);
            REQUIRE(result.HasValue());
        }

        std::size_t Count() const {
            std::size_t count{};
            if (const auto scene = scenes.ActiveScene(); scene)
                for (std::size_t index = 0; index < scene->SlotCount(); ++index)
                    count += static_cast<std::size_t>(scene->EntityAt(index).has_value());
            return count;
        }

        void Reach(std::size_t count) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (Count() != count && std::chrono::steady_clock::now() < deadline) {
                Pump();
                if (Count() != count)
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            REQUIRE(Count() == count);
            REQUIRE_FALSE(scenes.TakeOperationError());
        }

        /** @brief Copies the actual native module's existing operation observation while its image is pinned. */
        Prefab::PrefabOperation SpawnOperation() const {
            auto image = Platform::LoadDynamicLibrary(HORO_TEST_GAME_MODULE_PATH);
            REQUIRE(image.HasValue());
            void *symbol = image.Value()->GetSymbol("HoroTestCopyPrefabOperations");
            REQUIRE(symbol != nullptr);
            using Copy = void (*)(Prefab::PrefabOperation *, Prefab::PrefabOperation *, Prefab::PrefabInstance *);
            static_assert(sizeof(Copy) == sizeof(symbol));
            const auto copy = std::bit_cast<Copy>(symbol);
            Prefab::PrefabOperation spawn;
            Prefab::PrefabOperation despawn;
            Prefab::PrefabInstance instance;
            copy(&spawn, &despawn, &instance);
            return spawn;
        }

        Prefab::PrefabOperation AwaitRejection() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
                Pump();
                if (auto operation = SpawnOperation(); operation.Failure()) {
                    REQUIRE(operation.State() == Prefab::PrefabSpawnState::Failed);
                    return operation;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("A malformed prefab did not produce its typed rejection.");
            return {};
        }

        void AwaitState(const Prefab::PrefabOperation &operation, Prefab::PrefabSpawnState expected) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (operation.State() == Prefab::PrefabSpawnState::Pending && std::chrono::steady_clock::now() < deadline) {
                Pump();
                if (operation.State() == Prefab::PrefabSpawnState::Pending)
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            REQUIRE(operation.State() == expected);
        }

        Prefab::PrefabTemplateLease LoadLease() {
            auto request = templates.LoadAsync({asset, artifact, target}, cancellation.Token());
            REQUIRE(request.HasValue());
            auto handle = std::move(request).Value();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (handle.State() == Prefab::PrefabTemplateLoadState::Loading && std::chrono::steady_clock::now() < deadline) {
                REQUIRE(templates.Advance().HasValue());
                if (handle.State() == Prefab::PrefabTemplateLoadState::Loading)
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            REQUIRE(handle.State() == Prefab::PrefabTemplateLoadState::Succeeded);
            auto lease = templates.TakeResult(handle);
            REQUIRE(lease.HasValue());
            return std::move(lease).Value();
        }

        void RestartGameplay() {
            gameplay->Shutdown();
            activeGameplay = nullptr;
            gameplay.reset();
            ActivateGameplay();
        }

        std::uint32_t Bodies() {
            auto scope = native.Borrow();
            REQUIRE(scope.HasValue());
            auto observed = scope.Value().physics.ReadSceneActivation();
            REQUIRE(observed.HasValue());
            return observed.Value().staticBodies + observed.Value().awakeMovingBodies + observed.Value().sleepingMovingBodies;
        }

        void Tick(std::uint64_t tick) {
            REQUIRE(gameplay->FixedTick({tick, Duration::FromNanoseconds(10'000'000), cancellation.Token()}).HasValue());
        }
    };
#endif
}  // namespace Horo::Application::PrefabAtomicTest
