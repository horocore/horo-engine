#pragma once

#include "Horo/Assets/AssetCook.h"
#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Prefab/PrefabSpawnService.h"
#include "PrefabTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <format>
#include <functional>
#include <thread>

namespace Horo::Prefab::SpawnTest {
    inline Gameplay::BehaviorTypeId Type() {
        return Gameplay::BehaviorTypeId::Parse("game.test.prefab").Value();
    }

    inline PrefabPropertyId Property(const std::uint64_t id) {
        return PrefabPropertyId::Create(id).Value();
    }

    inline std::vector<Gameplay::BehaviorField> Fields(const std::size_t count = 2) {
        std::vector<Gameplay::BehaviorField> fields;
        for (std::size_t index = 0; index < count; ++index)
            fields.push_back({std::format("value_{}", index), 2.0});
        return fields;
    }

    struct Trace final {
        std::size_t creates{};
        std::size_t starts{};
        std::size_t destroys{};
        std::vector<Gameplay::BehaviorField> lastFields;
        std::shared_ptr<const Gameplay::GameplayPrefabContext> retained;
        std::function<void(Gameplay::BehaviorContext &)> onTick;
        std::function<void(Gameplay::BehaviorContext &)> onCreate;
    };

    class Behavior final : public Gameplay::IBehaviorInstance {
    public:
        explicit Behavior(Trace &trace) : trace_(trace) {}

        void OnCreate(Gameplay::BehaviorContext &context) override {
            ++trace_.creates;
            trace_.lastFields.assign(context.Fields().begin(), context.Fields().end());
            if (trace_.onCreate)
                trace_.onCreate(context);
        }

        void OnStart(Gameplay::BehaviorContext &) override {
            ++trace_.starts;
        }

        void OnFixedUpdate(Gameplay::BehaviorContext &context, Gameplay::FixedDeltaTime) override {
            trace_.retained = context.PrefabContext();
            if (trace_.onTick)
                trace_.onTick(context);
        }

        void OnDestroy(Gameplay::BehaviorContext &) override {
            ++trace_.destroys;
        }

    private:
        Trace &trace_;
    };

    class Participant final : public Runtime::SceneStructuralParticipant {
    public:
        explicit Participant(Gameplay::BehaviorRuntime *const &runtime) : runtime_(runtime) {}

        Runtime::SceneStructuralOwner Owner() const noexcept override {
            return Runtime::SceneStructuralOwner::Gameplay;
        }

        Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(Runtime::RuntimeSceneView active,
                                                                           std::span<const Runtime::RuntimeEntityView> created,
                                                                           std::span<const Runtime::EntityRef> destroyed) override {
            REQUIRE(runtime_ != nullptr);
            return runtime_->MakeStructuralParticipant()->Prepare(active, created, destroyed);
        }

    private:
        Gameplay::BehaviorRuntime *const &runtime_;
    };

    struct Fixture final {
        Trace trace;
        Assets::MemoryAssetProvider bytes;
        JobSystem jobs{{1, 32}};
        Assets::AssetLoadService loads{jobs, bytes};
        Assets::AssetRegistry registry;
        Runtime::RuntimeSceneService scenes{registry, loads};
        PrefabTemplateProvider templates{registry, loads, scenes, PrefabLimitProfile::Create({}).Value()};
        Gameplay::BehaviorRegistry behaviors;
        Gameplay::BehaviorRuntime *activeRunner{};
        std::unique_ptr<Gameplay::BehaviorRuntime> runner;
        std::unique_ptr<PrefabSpawnService> service;
        std::shared_ptr<Gameplay::GameplayPrefabContext> context;
        CancellationSource cancellation;
        std::uint64_t tick{1};
        Sha256Digest digest;
        std::size_t fieldCount;

        explicit Fixture(const std::size_t fields = 2) : fieldCount(fields) {
            const auto assetType = Assets::AssetTypeId::Parse("core.prefab").Value();
            const auto published = registry.Publish({{Test::Asset(), assetType, ProjectPath::Parse("assets/test.prefab").Value(),
                                                      ProjectPath::Parse("assets/test.prefab.horo").Value()}});
            REQUIRE(published.status == Assets::AssetRegistryBuildStatus::Complete);
            REQUIRE(scenes.AddStructuralParticipant(std::make_unique<Participant>(activeRunner)).HasValue());
            REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
            Gameplay::BehaviorDescriptor descriptor;
            descriptor.typeId = Type();
            descriptor.displayName = "Prefab test";
            descriptor.category = "Test";
            descriptor.allowMultiple = true;
            for (const auto &field : Fields(fields))
                descriptor.fields.push_back({field.name, field.value});
            Gameplay::BehaviorFactoryBinding factory{&trace, [](void *data) -> Gameplay::IBehaviorInstance * {
                return new Behavior(*static_cast<Trace *>(data));
            }, [](void *, Gameplay::IBehaviorInstance *instance) noexcept {
                delete instance;
            }};
            const auto registered = behaviors.Register({std::move(descriptor), factory});
            REQUIRE(registered.HasValue());
            REQUIRE(behaviors.Freeze().HasValue());
            Runtime::SceneDefinitionBuilder builder{{7}, {1}};
            Runtime::RuntimeEntityDefinition initial;
            initial.object = {1};
            initial.components.behaviors.push_back({{1}, Type(), 1, true, Fields(fields)});
            builder.Add(std::move(initial));
            REQUIRE(scenes.QueuePreparation(std::move(builder).Build().Value()).HasValue());
            Commit();
            REQUIRE(templates.Startup(cancellation.Token()).HasValue());
            service = std::make_unique<PrefabSpawnService>(templates, scenes);
            auto admitted = service->Acquire({"game.test", scenes.ActiveScene()->RuntimeId(), true, true});
            REQUIRE(admitted.HasValue());
            context = std::move(admitted).Value();
            auto runtime = Gameplay::BehaviorRuntime::Create(scenes, behaviors, {}, {}, context);
            REQUIRE(runtime.HasValue());
            runner = std::move(runtime).Value();
            activeRunner = runner.get();
            Put(Data());
        }

        ~Fixture() {
            if (runner)
                runner->Shutdown();
            if (service)
                service->Shutdown();
            templates.Shutdown();
            scenes.Shutdown();
        }

        CookedPrefabData Data() const {
            CookedPrefabData data;
            data.assetId = Test::Asset();
            CookedPrefabEntity root;
            root.provenance = {data.assetId, {}, ComputeSha256(std::as_bytes(std::span("source", 6)))};
            root.members.push_back(Gameplay::BehaviorComponent{{9}, Type(), 1, true, Fields(fieldCount)});
            auto child = root;
            child.parent = CookedPrefabEntitySlot{0};
            child.provenance.sourceObject = PrefabObjectAddress::Create({}, {7}).Value();
            std::get<Gameplay::BehaviorComponent>(child.members.front()).instanceId.value = 10;
            data.entities = {root, child};
            data.initialization.push_back({{1}, {{0}, 0}, 0, PrefabInitializationKind::Number, true, 0.0, 100.0});
            data.initialization.push_back({{2}, {{0}, 0}, 1, PrefabInitializationKind::Number, false, 0.0, 100.0});
            return data;
        }

        void Put(CookedPrefabData data) {
            auto cooked = CookedPrefab::Create(std::move(data), PrefabLimitProfile::Create({}).Value());
            REQUIRE(cooked.HasValue());
            std::vector<std::uint8_t> payload;
            for (const auto byte : cooked.Value().Bytes())
                payload.push_back(std::to_integer<std::uint8_t>(byte));
            const auto hash = ComputeSha256(std::as_bytes(std::span(payload)));
            auto envelope = Assets::EncodeCookedArtifact({Test::Asset(),
                                                          Assets::AssetTypeId::Parse("core.prefab").Value(),
                                                          AssetCookTargetId::Parse("linux-x64").Value(),
                                                          {},
                                                          {},
                                                          hash,
                                                          std::move(payload)});
            REQUIRE(envelope.HasValue());
            digest = ComputeSha256(std::as_bytes(std::span(envelope.Value())));
            templates.Evict(Test::Asset());
            bytes.Insert(Test::Asset(), std::move(envelope).Value());
        }

        PrefabSpawnRequest Request() const {
            PrefabSpawnRequest request;
            request.source = {Test::Asset(), digest, AssetCookTargetId::Parse("linux-x64").Value()};
            request.notBeforeTick = tick;
            request.initialization.push_back({{1}, 42.0});
            return request;
        }

        Runtime::FrameContext Frame() const {
            return {1, {}, 0.0, 0, {}, false, cancellation.Token()};
        }

        void Commit() {
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Frame()).HasValue());
        }

        void Advance() {
            REQUIRE(service->Advance(tick).HasValue());
        }

        void Warm() {
            auto load = templates.LoadAsync(Request().source);
            REQUIRE(load.HasValue());
            for (std::size_t attempt = 0; attempt < 2000 && load.Value().State() == PrefabTemplateLoadState::Loading; ++attempt) {
                REQUIRE(templates.Advance().HasValue());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            REQUIRE(templates.TakeResult(load.Value()).HasValue());
        }

        void Drain(const PrefabOperation &operation) {
            for (std::size_t attempt = 0; attempt < 2000 && operation.State() == PrefabSpawnState::Pending; ++attempt) {
                Advance();
                Commit();
                Advance();
                if (operation.State() == PrefabSpawnState::Pending)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            REQUIRE(operation.State() != PrefabSpawnState::Pending);
        }

        std::size_t Count() const {
            std::size_t count{};
            const auto scene = scenes.ActiveScene();
            if (scene)
                for (std::size_t index = 0; index < scene->SlotCount(); ++index)
                    if (scene->EntityAt(index))
                        ++count;
            return count;
        }
    };
}  // namespace Horo::Prefab::SpawnTest
