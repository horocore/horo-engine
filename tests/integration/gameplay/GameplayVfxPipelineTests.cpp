#include "GameplayRuntimeTestSupport.h"
#include "Horo/Gameplay/GameplayVfxScene.h"
#include "Horo/Gameplay/LuaBehavior.h"
#include "LuaVfxBinding.h"
#include "support/VfxTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <limits>
#include <stdexcept>

extern "C" {
#include <lua.h>
}

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Runtime;
    using Status = Vfx::VfxSpawnStatus;

    Vfx::VfxIdentityScope SourceOwner() {
        const auto scope = Vfx::VfxIdentityScope::Create(174920);
        REQUIRE(scope.HasValue());
        return scope.Value();
    }

    Assets::AssetId EffectAsset() {
        return Vfx::Tests::ParticleMaterial("17490000-0000-4000-8000-000000000088");
    }

    Vfx::VfxSpawnAssetPreparation Recipe() {
        auto data = Vfx::Tests::ValidParticleDescriptorData();
        data.maximumParticles = 4;
        data.simulationPreference = Vfx::SimulationPreference::RequireCPU;
        data.shape = Vfx::ParticleEmitterShape::Point;
        data.spawnRate = {0.0, 0.0};
        data.lifetimeSeconds = {2.0, 2.0};
        data.initialSpeed = {0.0, 0.0};
        data.initialSize = {1.0, 1.0};
        data.initialOpacity = {1.0, 1.0};
        data.sortMode = Vfx::ParticleSortMode::None;
        data.collisionMode = Vfx::ParticleCollisionMode::None;
        return {.asset = EffectAsset(),
                .particles = std::make_shared<const Vfx::ParticleSystemDescriptor>(Vfx::Tests::ParticleDescriptor(data)),
                .simulation = {.effectSeed = 7, .maximumBurstParticles = 4, .maximumDeltaSeconds = 1.0F},
                .maximumInstances = 8,
                .initialBurst = 1,
                .durationSeconds = 1.0F,
                .material = {1}};
    }

    Gameplay::GameplayVfxBindingDocument Bindings(const std::uint64_t generation = 1, const float scale = 1.0F) {
        Gameplay::GameplayVfxBinding binding;
        binding.destination = 1;
        binding.event = Gameplay::GameplayEventTypeId{"game.tests.present"};
        binding.layers.push_back({.effectAsset = EffectAsset(), .scale = scale});
        return {.generation = generation, .bindings = {std::move(binding)}};
    }

    Gameplay::GameplayVfxSceneConfiguration Configuration(const Assets::AssetRegistrySnapshot &registry,
                                                          const std::shared_ptr<const Assets::IAssetProvider> &provider) {
        return {.recipes = {Recipe()},
                .spawn = {.maximumPending = 8,
                          .maximumResults = 8,
                          .maximumOwners = 2,
                          .maximumProducers = 2,
                          .maximumInstances = 8,
                          .maximumParticles = 32,
                          .maximumBytes = 8U * 1024U * 1024U,
                          .maximumDeltaSeconds = 1.0F},
                .extraction =
                    {.maximumBatches = 8, .maximumInstances = 32, .maximumDrawCalls = 8, .frameSlots = 3, .maximumBytes = 1024U * 1024U},
                .source = {.maximumPending = 8, .maximumModules = 1, .maximumBytes = 1024U * 1024U},
                .bindings = Bindings(),
                .registry = registry,
                .provider = provider,
                .grants = {{.moduleId = "game.tests",
                            .owner = SourceOwner(),
                            .ownerGeneration = 1,
                            .producer = 9,
                            .events = {Gameplay::GameplayEventTypeId{"game.tests.present"}}}}};
    }

    BehaviorTypeId Type() {
        return BehaviorTypeId::Parse("game.tests.vfx_emitter").Value();
    }

    struct SceneFixture final {
        Assets::AssetRegistry assets;
        std::shared_ptr<Assets::MemoryAssetProvider> provider{std::make_shared<Assets::MemoryAssetProvider>()};
        std::shared_ptr<GameplayVfxScene> vfx;
        RuntimeSceneService scenes;
        CancellationToken cancellation;
        std::uint64_t frame{};

        explicit SceneFixture(const std::uint64_t initialId = 174900) : scenes(SceneRuntimeId{initialId}) {
            const Assets::AssetRecord record{.id = EffectAsset(),
                                             .type = Assets::AssetTypeId::Parse("core.vfx.effect").Value(),
                                             .sourcePath = ProjectPath::Parse("Assets/Effects/presentation.vfx").Value(),
                                             .metadataPath = ProjectPath::Parse("Assets/Effects/presentation.vfx.horo").Value()};
            REQUIRE(assets.Publish({record}).status == Assets::AssetRegistryBuildStatus::Complete);
            provider->Insert(EffectAsset(), {'{', '}'});
            auto config = Configuration(assets.Snapshot(), provider);
            GameplayVfxBinding tagged = config.bindings.bindings.front();
            tagged.destination = 2;
            tagged.event = {};
            tagged.tag = {42};
            config.bindings.bindings.push_back(std::move(tagged));
            config.grants.front().tags.push_back({42});
            auto created = GameplayVfxScene::Create(std::move(config));
            REQUIRE(created.HasValue());
            vfx = std::move(created).Value();
            REQUIRE(scenes.AddActivationParticipant(vfx->MakeActivationParticipant()).HasValue());
            REQUIRE(scenes.Startup(cancellation).HasValue());
            REQUIRE(scenes.QueuePreparation(Tests::SingleBehaviorSceneDefinition({1749}, {1}, {1}, {1}, Type())).HasValue());
            Phase(RuntimePhase::CommitDeferredLifecycleChanges);
        }

        ~SceneFixture() {
            scenes.Shutdown();
        }

        void Phase(RuntimePhase phase) {
            REQUIRE(scenes.OnPhase(phase, {.frameNumber = ++frame, .cancellation = cancellation}).HasValue());
        }

        std::unique_ptr<BehaviorRuntime> Activate(BehaviorRegistry &registry) {
            REQUIRE(registry.Freeze().HasValue());
            auto runtime = BehaviorRuntime::Create(scenes, registry, {}, {.vfx = vfx->Module("game.tests"), .vfxModuleId = "game.tests"});
            REQUIRE(runtime.HasValue());
            return std::move(runtime).Value();
        }

        void Extract(std::size_t count) {
            Phase(RuntimePhase::RenderExtraction);
            REQUIRE(scenes.VfxFrame().report.status == Vfx::VfxExtractionStatus::Complete);
            CHECK(scenes.VfxFrame().snapshot.Instances().size() == count);
        }
    };

    struct NativeControl final {
        std::shared_ptr<const GameplayVfxContext> retained;
        GameplayVfxSubmission submission;
        bool fail{};
        bool move{true};
    };

    class NativeEmitter final : public IBehaviorInstance {
    public:
        explicit NativeEmitter(NativeControl &control) : control(control) {}

        void OnFixedUpdate(BehaviorContext &context, FixedDeltaTime) override {
            control.retained = context.VfxContext();
            REQUIRE(control.retained);
            const auto lookup = control.retained->ResolveEvent("game.tests.present");
            REQUIRE(lookup.status == Status::Accepted);
            GameplayVfxEventPayload payload;
            payload.transform.position = Math::WorldCoordinate64::FromMillimeters(1000, 2000, 3000);
            control.submission = control.retained->Emit(lookup.key, payload);
            if (control.fail)
                throw std::runtime_error("source callback rejected");
            if (control.move) {
                auto transform = context.LocalTransform();
                REQUIRE(transform.HasValue());
                auto moved = transform.Value();
                moved.translation.x = 7.0F;
                REQUIRE(context.SetLocalTransform(moved).HasValue());
            }
        }

    private:
        NativeControl &control;
    };

    void Register(BehaviorRegistry &registry, NativeControl &control) {
        BehaviorDescriptor descriptor;
        descriptor.typeId = Type();
        descriptor.displayName = "VFX native callback";
        const BehaviorFactoryBinding factory{&control, [](void *data) -> IBehaviorInstance * {
            return new NativeEmitter{*static_cast<NativeControl *>(data)};
        }, [](void *, IBehaviorInstance *instance) noexcept {
            delete instance;
        }};
        REQUIRE(registry.Register({std::move(descriptor), factory}).HasValue());
    }
}  // namespace

TEST_CASE("Native Scene behavior commits actual presentation commands only after structural publication", "[vfx][gameplay]") {
    SceneFixture scene;
    NativeControl control;
    BehaviorRegistry registry;
    Register(registry, control);
    auto runtime = scene.Activate(registry);
    REQUIRE(scene.vfx->BeginTick(1, 0.1F) == Status::Accepted);
    REQUIRE(runtime->FixedUpdate({}, FixedDeltaTime{0.1}).HasValue());
    REQUIRE(control.submission.status == Status::Accepted);
    const auto receipt = runtime->VfxStructuralReceipt();
    REQUIRE(receipt);
    CHECK_FALSE(receipt->Complete());
    scene.Extract(0);
    scene.Phase(RuntimePhase::CommitDeferredLifecycleChanges);
    REQUIRE(receipt->Complete());
    REQUIRE(receipt->ResultValue());
    CHECK(scene.vfx->CommitTick(1).status == Status::Accepted);
    scene.Extract(1);
    const auto lookup = control.retained->ResolveEvent("game.tests.present");
    CHECK(control.retained->Emit(lookup.key, {}).status != Status::Accepted);
    runtime->Shutdown();
    CHECK_FALSE(control.retained->IsUsable());
    CHECK(scene.vfx->Module("game.tests")->IsUsable());
}

TEST_CASE("Failed native callbacks abort source commands and retained clients cannot extend callback authority", "[vfx][gameplay]") {
    SceneFixture scene;
    NativeControl control;
    control.fail = true;
    BehaviorRegistry registry;
    Register(registry, control);
    auto runtime = scene.Activate(registry);
    REQUIRE(scene.vfx->BeginTick(1, 0.1F) == Status::Accepted);
    CHECK_THROWS_AS(runtime->FixedUpdate({}, FixedDeltaTime{0.1}), std::runtime_error);
    scene.vfx->AbortTick(1);
    REQUIRE(scene.vfx->BeginTick(1, 0.1F) == Status::Accepted);
    CHECK(scene.vfx->CommitTick(1).status == Status::Accepted);
    scene.Extract(0);
    const auto lookup = control.retained->ResolveEvent("game.tests.present");
    CHECK(control.retained->Emit(lookup.key, {}).status != Status::Accepted);
}

TEST_CASE("Lua callbacks use the genuine scoped mapping for events and effect tags", "[vfx][gameplay][lua]") {
    SceneFixture scene;
    const auto source = R"(
return horo.behavior {
    type_id = "game.tests.vfx_emitter", display_name = "VFX Lua",
    on_fixed_update = function(ctx, dt)
        local first = ctx.vfx.emit("game.tests.present", {x_mm=1000,y_mm=2000,z_mm=3000,scale=2})
        local second = ctx.vfx.emit(42, {x_mm=4000,y_mm=0,z_mm=0})
        local unknown = ctx.vfx.emit("game.tests.absent", {x_mm=0,y_mm=0,z_mm=0})
        ctx.transform.set_position(first,second,unknown)
    end
})";
    auto program = LuaBehaviorProgram::Compile(source, Type(), "presentation.horo_script");
    REQUIRE(program.HasValue());
    BehaviorRegistry registry;
    REQUIRE(registry.Register(program.Value()->Registration()).HasValue());
    auto runtime = scene.Activate(registry);
    REQUIRE(scene.vfx->BeginTick(1, 0.1F) == Status::Accepted);
    REQUIRE(runtime->FixedUpdate({}, FixedDeltaTime{0.1}).HasValue());
    auto receipt = runtime->VfxStructuralReceipt();
    REQUIRE(receipt);
    scene.Phase(RuntimePhase::CommitDeferredLifecycleChanges);
    REQUIRE(receipt->ResultValue());
    const auto view = scene.scenes.ActiveScene().value();
    const auto entity = view.Find({1});
    REQUIRE(entity);
    const auto actor = view.Get(*entity);
    REQUIRE(actor.HasValue());
    CHECK(actor.Value().localTransform->translation.x == static_cast<float>(Status::Accepted));
    CHECK(actor.Value().localTransform->translation.y == static_cast<float>(Status::Accepted));
    CHECK(actor.Value().localTransform->translation.z != static_cast<float>(Status::Accepted));
    CHECK(scene.vfx->CommitTick(1).status == Status::Accepted);
    scene.Extract(2);
}

TEST_CASE("Behavior VFX admission denies foreign module and Scene holders before executing callbacks", "[vfx][gameplay]") {
    SceneFixture first;
    SceneFixture second{175000};
    NativeControl control;
    BehaviorRegistry registry;
    Register(registry, control);
    REQUIRE(registry.Freeze().HasValue());
    CHECK(BehaviorRuntime::Create(first.scenes, registry, {}, {.vfx = first.vfx->Module("game.tests"), .vfxModuleId = "game.other"})
              .HasError());
    REQUIRE(second.vfx->Scene() != first.vfx->Scene());
    CHECK(BehaviorRuntime::Create(first.scenes, registry, {}, {.vfx = second.vfx->Module("game.tests"), .vfxModuleId = "game.tests"})
              .HasError());
}

namespace {
    /** @brief Observes the real entity holder while delegating every lifecycle call to the actual Lua factory instance. */
    struct LuaLifetimeControl final {
        BehaviorFactoryBinding delegate;
        std::weak_ptr<const GameplayVfxContext> observed;
        bool called{};
    };

    class ObservedLua final : public IBehaviorInstance {
    public:
        ObservedLua(LuaLifetimeControl &control, IBehaviorInstance *delegate) : control(control), delegate(delegate) {}

        ~ObservedLua() override {
            control.delegate.destroy(control.delegate.userData, delegate);
        }

        void OnCreate(BehaviorContext &context) override {
            delegate->OnCreate(context);
        }

        void OnEnable(BehaviorContext &context) override {
            delegate->OnEnable(context);
        }

        void OnStart(BehaviorContext &context) override {
            delegate->OnStart(context);
        }

        void OnDisable(BehaviorContext &context) override {
            delegate->OnDisable(context);
        }

        void OnDestroy(BehaviorContext &context) override {
            delegate->OnDestroy(context);
        }

        void OnFixedUpdate(BehaviorContext &context, FixedDeltaTime delta) override {
            control.observed = context.VfxContext();
            control.called = true;
            delegate->OnFixedUpdate(context, delta);
        }

    private:
        LuaLifetimeControl &control;
        IBehaviorInstance *delegate;
    };

    IBehaviorInstance *CreateObservedLua(void *data) {
        auto &control = *static_cast<LuaLifetimeControl *>(data);
        auto destroy = [&control](IBehaviorInstance *instance) {
            control.delegate.destroy(control.delegate.userData, instance);
        };
        std::unique_ptr<IBehaviorInstance, decltype(destroy)> inner{control.delegate.create(control.delegate.userData), destroy};
        if (!inner)
            return nullptr;
        auto wrapper = std::make_unique<ObservedLua>(control, inner.get());
        (void)inner.release();
        return wrapper.release();
    }

    std::string PressureScript(const std::size_t padding, const bool exhaustAfterEmit) {
        const auto suffix = exhaustAfterEmit ? "local excess = string.rep('z', 256 * 1024)" : "";
        return "local held = string.rep('x', " + std::to_string(padding) + R"()
return horo.behavior {
 type_id = 'game.tests.vfx_emitter', display_name = 'VFX memory pressure',
 on_fixed_update = function(ctx, dt)
   if #held > 0 then
     ctx.vfx.emit('game.tests.present', {x_mm=1000,y_mm=0,z_mm=0})
   end
 )" + suffix + R"(
 end
})";
    }

    /** @brief Exercises a real VM allocation budget; no C++ allocation instrumentation or fake scope is involved. */
    std::optional<bool> RunLuaPressure(const std::size_t padding, const bool exhaustAfterEmit) {
        auto program = LuaBehaviorProgram::Compile(PressureScript(padding, exhaustAfterEmit), Type(), "vfx-memory.horo_script",
                                                   {.maximumMemoryBytes = 64U * 1024U, .maximumInstructionsPerCallback = 100'000});
        if (program.HasError())
            return std::nullopt;
        SceneFixture scene;
        auto registration = program.Value()->Registration();
        LuaLifetimeControl control{registration.factory};
        registration.factory = {&control, CreateObservedLua, [](void *, IBehaviorInstance *instance) noexcept {
            delete instance;
        }};
        BehaviorRegistry registry;
        REQUIRE(registry.Register(std::move(registration)).HasValue());
        auto runtime = scene.Activate(registry);
        REQUIRE(scene.vfx->BeginTick(1, 0.1F) == Status::Accepted);
        REQUIRE(runtime->FixedUpdate({}, FixedDeltaTime{0.1}).HasValue());
        REQUIRE(control.called);
        const auto report = scene.vfx->CommitTick(1);
        REQUIRE(report.status == Status::Accepted);
        scene.Extract(report.submitted);
        runtime->Shutdown();
        runtime.reset();
        CHECK(control.observed.expired());
        return report.submitted != 0;
    }
}  // namespace

namespace {
    /** @brief POD fault state applies only to real Lua allocation growth inside pcall, never C++ allocation. */
    struct LuaAllocationFault final {
        std::size_t grows{};
        std::size_t failFrom{std::numeric_limits<std::size_t>::max()};
        bool enabled{};
    };

    void *FaultLuaAllocate(void *context, void *memory, std::size_t previous, std::size_t requested) noexcept {
        auto &fault = *static_cast<LuaAllocationFault *>(context);
        if (requested == 0) {
            std::free(memory);
            return nullptr;
        }
        if (fault.enabled && (memory == nullptr || requested > previous)) {
            ++fault.grows;
            if (fault.grows >= fault.failFrom)
                return nullptr;
        }
        return std::realloc(memory, requested);
    }

    int ConstructRealVfxContext(lua_State *state) {
        const auto *client = static_cast<const std::shared_ptr<const GameplayVfxContext> *>(lua_touserdata(state, lua_upvalueindex(1)));
        Horo::Gameplay::Detail::PushLuaVfxContext(state, *client);
        return 1;
    }

    /** @brief All C++ owners remain outside the protected C callback; failed construction must release every extra holder. */
    bool CheckLuaConstructionFault(const std::shared_ptr<const GameplayVfxContext> &client, const std::size_t failFrom) {
        LuaAllocationFault fault;
        lua_State *state = lua_newstate(FaultLuaAllocate, &fault);
        REQUIRE(state);
        lua_pushlightuserdata(state, const_cast<std::shared_ptr<const GameplayVfxContext> *>(&client));
        lua_pushcclosure(state, ConstructRealVfxContext, 1);
        const auto baseline = client.use_count();
        fault.failFrom = failFrom;
        fault.enabled = true;
        const int result = lua_pcall(state, 0, 1, 0);
        fault.enabled = false;
        // No Catch assertion, logging or injected allocation is allowed between the call and actual VM finalization.
        lua_close(state);
        INFO(failFrom);
        CHECK(client.use_count() == baseline);
        REQUIRE((result == LUA_OK || result == LUA_ERRMEM));
        return result == LUA_ERRMEM;
    }
}  // namespace

TEST_CASE("Lua context construction releases real C++ scopes across protected Lua allocation failures", "[vfx][gameplay][lua][memory]") {
    SceneFixture scene;
    NativeControl control;
    control.move = false;
    BehaviorRegistry registry;
    Register(registry, control);
    auto runtime = scene.Activate(registry);
    REQUIRE(scene.vfx->BeginTick(1, 0.1F) == Status::Accepted);
    REQUIRE(runtime->FixedUpdate({}, FixedDeltaTime{0.1}).HasValue());
    scene.vfx->AbortTick(1);
    const auto client = control.retained;
    REQUIRE(client);
    std::size_t successful{}, exhausted{};
    for (std::size_t point = 1; point <= 64; ++point) {
        if (CheckLuaConstructionFault(client, point))
            ++exhausted;
        else
            ++successful;
    }
    CHECK(successful > 0);
    CHECK(exhausted > 0);
    REQUIRE(scene.vfx->BeginTick(1, 0.1F) == Status::Accepted);
    CHECK(scene.vfx->CommitTick(1).status == Status::Accepted);
    scene.Extract(0);
}

TEST_CASE("Lua VM exhaustion after emission cancels the failed callback without retaining its C++ scope", "[vfx][gameplay][lua][memory]") {
    const auto result = RunLuaPressure(1024, true);
    REQUIRE(result.has_value());
    CHECK_FALSE(*result);
}
