#include "Horo/Gameplay/BehaviorRuntime.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Runtime;

    BehaviorTypeId Type(const std::string_view value = "game.tests.mover") {
        auto parsed = BehaviorTypeId::Parse(value);
        REQUIRE(parsed.HasValue());
        return std::move(parsed).Value();
    }

    struct Recorder {
        std::vector<std::string> calls;
        std::size_t destroyed{};
        std::size_t reloadStateBytes{1};
        bool throwOnCreate{};
        bool throwOnEnable{};
        bool throwOnDisable{};
        bool throwOnDestroy{};
        std::function<void()> onFactory;
        std::function<void()> onCreate;
        std::function<void()> onEnable;
    };

    class RecordingBehavior final : public IBehaviorInstance {
    public:
        explicit RecordingBehavior(Recorder &recorder) : recorder_(&recorder) {}

        void OnCreate(BehaviorContext &) override {
            recorder_->calls.emplace_back("create");
            if (recorder_->onCreate)
                recorder_->onCreate();
            if (recorder_->throwOnCreate)
                throw std::runtime_error{"OnCreate failure"};
        }

        void OnEnable(BehaviorContext &) override {
            recorder_->calls.emplace_back("enable");
            if (recorder_->onEnable)
                recorder_->onEnable();
            if (recorder_->throwOnEnable)
                throw std::runtime_error{"OnEnable failure"};
        }

        void OnStart(BehaviorContext &) override {
            recorder_->calls.emplace_back("start");
        }

        void OnInputAction(BehaviorContext &, const GameplayInputAction &) override {
            recorder_->calls.emplace_back("input");
        }

        void OnEvent(BehaviorContext &, const GameplayEvent &) override {
            recorder_->calls.emplace_back("event");
        }

        void OnFixedUpdate(BehaviorContext &context, FixedDeltaTime) override {
            recorder_->calls.emplace_back("fixed");
            const auto input = context.InputActions();
            if (!input.empty() && input.front().down) {
                auto transform = context.LocalTransform();
                REQUIRE(transform.HasValue());
                Math::Transform moved = transform.Value();
                moved.translation.x += input.front().x;
                REQUIRE(context.SetLocalTransform(moved).HasValue());
            }
            if (!published_) {
                REQUIRE(context.Publish(GameplayEvent{GameplayEventTypeId{"game.tests.moved"}, 1, context.Entity(), {}}).HasValue());
                published_ = true;
            }
        }

        void OnPresentationUpdate(BehaviorContext &, FrameDeltaTime) override {
            recorder_->calls.emplace_back("presentation");
        }

        void OnDisable(BehaviorContext &) override {
            recorder_->calls.emplace_back("disable");
            if (recorder_->throwOnDisable)
                throw std::runtime_error{"OnDisable failure"};
        }

        void OnDestroy(BehaviorContext &) override {
            recorder_->calls.emplace_back("destroy");
            if (recorder_->throwOnDestroy)
                throw std::runtime_error{"OnDestroy failure"};
        }

        Result<std::vector<std::byte>> CaptureReloadState() const override {
            std::vector<std::byte> state(recorder_->reloadStateBytes);
            if (!state.empty())
                state.front() = published_ ? std::byte{1} : std::byte{0};
            return Result<std::vector<std::byte>>::Success(std::move(state));
        }

        Result<void> RestoreReloadState(const std::span<const std::byte> state) override {
            if (state.size() != 1)
                return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadRestoreFailed));
            published_ = state.front() == std::byte{1};
            return Result<void>::Success();
        }

    private:
        Recorder *recorder_{};
        bool published_{};
    };

    IBehaviorInstance *Create(void *userData) {
        auto &recorder = *static_cast<Recorder *>(userData);
        if (recorder.onFactory)
            recorder.onFactory();
        return new RecordingBehavior{recorder};
    }

    void Destroy(void *userData, IBehaviorInstance *instance) noexcept {
        ++static_cast<Recorder *>(userData)->destroyed;
        delete instance;
    }

    BehaviorRegistry Registry(Recorder &recorder, const bool allowMultiple = false) {
        BehaviorRegistry registry;
        BehaviorDescriptor descriptor;
        descriptor.typeId = Type();
        descriptor.displayName = "Mover";
        descriptor.allowMultiple = allowMultiple;
        descriptor.phases.push_back({BehaviorPhase::Gameplay, "game.tests.mover", {}, {}, {}});
        REQUIRE(registry.Register({std::move(descriptor), {&recorder, &Create, &Destroy}}).HasValue());
        REQUIRE(registry.Freeze().HasValue());
        return registry;
    }

    RuntimeSceneDefinition Definition(const std::size_t attachmentCount = 1) {
        RuntimeComponentSet components;
        for (std::size_t index = 0; index < attachmentCount; ++index)
            components.behaviors.push_back({BehaviorInstanceId{index + 1}, Type(), 1, true, {}});
        SceneDefinitionBuilder builder{SceneDefinitionId{4}, SceneDefinitionRevision{1}};
        builder.Add({SceneObjectId{9}, std::nullopt, {}, std::nullopt, std::move(components)});
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        return std::move(definition).Value();
    }
}  // namespace

TEST_CASE("Gameplay group preparation never constructs callbacks and preserves the existing runner", "[prefab][gameplay]") {
    Recorder recorder;
    BehaviorRegistry registry = Registry(recorder);
    auto sceneResult = RuntimeScene::Create(Definition(), SceneRuntimeId{12});
    REQUIRE(sceneResult.HasValue());
    auto scene = std::move(sceneResult).Value();
    auto runtimeResult = BehaviorRuntime::Create(*scene, registry);
    REQUIRE(runtimeResult.HasValue());
    auto runtime = std::move(runtimeResult).Value();
    auto participant = runtime->MakeStructuralParticipant();
    RuntimeComponentSet components;
    components.behaviors.push_back({BehaviorInstanceId{2}, Type(), 1, true, {}});
    const RuntimeEntityView projected{.entity = {SceneRuntimeId{12}, EntityId{1, 1}}, .components = &components};
    const auto before = recorder.calls;
    {
        auto preparation = participant->Prepare(scene->View(), {&projected, 1}, {});
        REQUIRE(preparation.HasValue());
        auto candidate = std::move(preparation).Value();
        CHECK(candidate->ValidatePublication().HasValue());
        CHECK(recorder.calls == before);
        CHECK(runtime->InstanceCount() == 1);
        CHECK(runtime->FixedUpdate({}, FixedDeltaTime{}).HasError());
        // Rollback is metadata-only: no factory, no hooks, no existing instance teardown.
    }
    CHECK(recorder.calls == before);
    CHECK(recorder.destroyed == 0);
    {
        auto preparation = participant->Prepare(scene->View(), {&projected, 1}, {});
        REQUIRE(preparation.HasValue());
        auto candidate = std::move(preparation).Value();
        SceneCommandBuffer commands;
        (void)commands.Create({.components = components});
        REQUIRE(candidate->ValidatePublication().HasValue());
        REQUIRE(scene->Commit(commands).HasValue());
        candidate->Publish();
        REQUIRE(candidate->AfterPublication().HasValue());
        CHECK(runtime->InstanceCount() == 2);
        CHECK(std::ranges::count(recorder.calls, "create") == 2);
        CHECK(recorder.destroyed == 0);
    }
    runtime->Shutdown();
    CHECK(recorder.destroyed == 2);
}

TEST_CASE("Gameplay structural admission rejects unknown attachments without factory side effects", "[prefab][gameplay]") {
    Recorder recorder;
    BehaviorRegistry registry = Registry(recorder);
    auto sceneResult = RuntimeScene::Create(Definition(), SceneRuntimeId{12});
    REQUIRE(sceneResult.HasValue());
    auto scene = std::move(sceneResult).Value();
    auto runtimeResult = BehaviorRuntime::Create(*scene, registry);
    REQUIRE(runtimeResult.HasValue());
    auto runtime = std::move(runtimeResult).Value();
    auto participant = runtime->MakeStructuralParticipant();
    RuntimeComponentSet components;
    components.behaviors.push_back({BehaviorInstanceId{2}, Type("game.tests.unknown"), 1, true, {}});
    const RuntimeEntityView projected{.entity = {SceneRuntimeId{12}, EntityId{1, 1}}, .components = &components};
    const auto before = recorder.calls;
    CHECK(participant->Prepare(scene->View(), {&projected, 1}, {}).HasError());
    CHECK(recorder.calls == before);
    CHECK(runtime->InstanceCount() == 1);
    CHECK(recorder.destroyed == 0);
}

TEST_CASE("Gameplay structural callbacks defer reentrant shutdown until borrowed instances are released", "[prefab][gameplay]") {
    Recorder recorder;
    BehaviorRegistry registry = Registry(recorder);
    auto sceneResult = RuntimeScene::Create(Definition(), SceneRuntimeId{12});
    REQUIRE(sceneResult.HasValue());
    auto scene = std::move(sceneResult).Value();
    auto runtimeResult = BehaviorRuntime::Create(*scene, registry);
    REQUIRE(runtimeResult.HasValue());
    auto runtime = std::move(runtimeResult).Value();
    auto participant = runtime->MakeStructuralParticipant();
    RuntimeComponentSet components;
    components.behaviors.push_back({BehaviorInstanceId{2}, Type(), 1, true, {}});
    const RuntimeEntityView projected{.entity = {SceneRuntimeId{12}, EntityId{1, 1}}, .components = &components};
    auto preparation = participant->Prepare(scene->View(), {&projected, 1}, {});
    REQUIRE(preparation.HasValue());
    auto candidate = std::move(preparation).Value();
    const auto requestShutdown = [&] {
        runtime->Shutdown();
        CHECK(recorder.destroyed == 0);
    };
    SECTION("factory") {
        recorder.onFactory = requestShutdown;
    }
    SECTION("OnCreate") {
        recorder.onCreate = requestShutdown;
    }
    SECTION("OnEnable") {
        recorder.onEnable = requestShutdown;
    }
    SceneCommandBuffer commands;
    (void)commands.Create({.components = components});
    REQUIRE(candidate->ValidatePublication().HasValue());
    REQUIRE(scene->Commit(commands).HasValue());
    candidate->Publish();
    CHECK(candidate->AfterPublication().HasError());
    candidate.reset();
    CHECK(runtime->InstanceCount() == 0);
    CHECK(recorder.destroyed == 2);
    CHECK(scene->View().EntityAt(1).has_value());
}

TEST_CASE("Gameplay group replacement admits retiring instances at the full live budget", "[prefab][gameplay]") {
    Recorder recorder;
    BehaviorRegistry registry = Registry(recorder);
    auto sceneResult = RuntimeScene::Create(Definition(), SceneRuntimeId{12});
    REQUIRE(sceneResult.HasValue());
    auto scene = std::move(sceneResult).Value();
    auto runtimeResult = BehaviorRuntime::Create(*scene, registry, {.maximumInstances = 1});
    REQUIRE(runtimeResult.HasValue());
    auto runtime = std::move(runtimeResult).Value();
    auto participant = runtime->MakeStructuralParticipant();
    const auto old = scene->View().Find(SceneObjectId{9});
    REQUIRE(old.has_value());
    RuntimeComponentSet components;
    components.behaviors.push_back({BehaviorInstanceId{2}, Type(), 1, true, {}});
    const RuntimeEntityView projected{.entity = {SceneRuntimeId{12}, EntityId{0, 2}}, .components = &components};
    auto preparation = participant->Prepare(scene->View(), {&projected, 1}, {&*old, 1});
    REQUIRE(preparation.HasValue());
    auto candidate = std::move(preparation).Value();
    CHECK(recorder.destroyed == 0);
    SceneCommandBuffer commands;
    commands.Destroy(*old);
    (void)commands.Create({.components = components});
    REQUIRE(candidate->ValidatePublication().HasValue());
    REQUIRE(scene->Commit(commands).HasValue());
    candidate->Publish();
    REQUIRE(candidate->AfterPublication().HasValue());
    candidate.reset();
    CHECK(runtime->InstanceCount() == 1);
    CHECK(recorder.destroyed == 1);
    runtime->Shutdown();
    CHECK(recorder.destroyed == 2);
}

TEST_CASE("behavior runtime owns deterministic lifecycle input events and deferred transform mutation") {
    Recorder recorder;
    BehaviorRegistry registry = Registry(recorder);
    auto sceneResult = RuntimeScene::Create(Definition(), SceneRuntimeId{12});
    REQUIRE(sceneResult.HasValue());
    std::unique_ptr<RuntimeScene> scene = std::move(sceneResult).Value();

    auto runtimeResult = BehaviorRuntime::Create(*scene, registry);
    REQUIRE(runtimeResult.HasValue());
    std::unique_ptr<BehaviorRuntime> runtime = std::move(runtimeResult).Value();
    REQUIRE((recorder.calls == std::vector<std::string>{"create", "enable"}));

    const GameplayInputAction move{GameplayActionId{"game.tests.move"}, 2.0F, 0.0F, true, true, false};
    REQUIRE(runtime->FixedUpdate({&move, 1}, FixedDeltaTime{1.0 / 60.0}).HasValue());
    REQUIRE((recorder.calls == std::vector<std::string>{"create", "enable", "start", "input", "fixed"}));
    auto entity = scene->View().Find(SceneObjectId{9});
    REQUIRE(entity.has_value());
    auto view = scene->View().Get(*entity);
    REQUIRE(view.HasValue());
    REQUIRE(view.Value().localTransform->translation.x == 2.0F);

    REQUIRE(runtime->FixedUpdate({}, FixedDeltaTime{1.0 / 60.0}).HasValue());
    REQUIRE(recorder.calls.at(5) == "event");
    REQUIRE(recorder.calls.at(6) == "fixed");
    runtime->PresentationUpdate(FrameDeltaTime{1.0 / 120.0});
    REQUIRE(recorder.calls.back() == "presentation");

    REQUIRE(runtime->SetEnabled(BehaviorInstanceId{1}, false).HasValue());
    REQUIRE(recorder.calls.back() == "disable");
    REQUIRE(runtime->SetEnabled(BehaviorInstanceId{1}, true).HasValue());
    REQUIRE(recorder.calls.back() == "enable");
    REQUIRE(runtime->FixedUpdate({}, FixedDeltaTime{1.0 / 60.0}).HasValue());
    REQUIRE(std::ranges::count(recorder.calls, "start") == 1);

    runtime->Shutdown();
    REQUIRE(recorder.calls.at(recorder.calls.size() - 2) == "disable");
    REQUIRE(recorder.calls.back() == "destroy");
    REQUIRE(recorder.destroyed == 1);
}

TEST_CASE("behavior runtime rejects duplicate attachments unless the descriptor allows them") {
    Recorder recorder;
    BehaviorRegistry registry = Registry(recorder);
    auto scene = RuntimeScene::Create(Definition(2), SceneRuntimeId{12});
    REQUIRE(scene.HasValue());
    auto runtime = BehaviorRuntime::Create(*scene.Value(), registry);
    REQUIRE(runtime.HasError());
    REQUIRE(recorder.destroyed == 1);
}

TEST_CASE("behavior runtime contains activation exceptions and releases partial instances") {
    Recorder createFailure;
    createFailure.throwOnCreate = true;
    BehaviorRegistry createRegistry = Registry(createFailure);
    auto createScene = RuntimeScene::Create(Definition(), SceneRuntimeId{15});
    REQUIRE(createScene.HasValue());
    const auto createResult = BehaviorRuntime::Create(*createScene.Value(), createRegistry);
    REQUIRE(createResult.HasError());
    CHECK(createResult.ErrorValue().code.Value() == GameplayErrors::GameplayFactoryFailed.code.Value());
    CHECK(createFailure.destroyed == 1);
    CHECK(std::ranges::count(createFailure.calls, "destroy") == 1);

    Recorder enableFailure;
    enableFailure.throwOnEnable = true;
    BehaviorRegistry enableRegistry = Registry(enableFailure);
    auto enableScene = RuntimeScene::Create(Definition(), SceneRuntimeId{16});
    REQUIRE(enableScene.HasValue());
    const auto enableResult = BehaviorRuntime::Create(*enableScene.Value(), enableRegistry);
    REQUIRE(enableResult.HasError());
    CHECK(enableResult.ErrorValue().code.Value() == GameplayErrors::GameplayFactoryFailed.code.Value());
    CHECK(enableFailure.destroyed == 1);
    CHECK(std::ranges::count(enableFailure.calls, "disable") == 1);
    CHECK(std::ranges::count(enableFailure.calls, "destroy") == 1);
}

TEST_CASE("behavior runtime reports rollback callback exceptions after releasing the factory instance") {
    Recorder recorder;
    recorder.throwOnEnable = true;
    recorder.throwOnDisable = true;
    recorder.throwOnDestroy = true;
    BehaviorRegistry registry = Registry(recorder);
    auto scene = RuntimeScene::Create(Definition(), SceneRuntimeId{17});
    REQUIRE(scene.HasValue());
    const auto result = BehaviorRuntime::Create(*scene.Value(), registry);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == GameplayErrors::GameplayFactoryFailed.code.Value());
    CHECK(recorder.destroyed == 1);
    CHECK(std::ranges::count(recorder.calls, "disable") == 1);
    CHECK(std::ranges::count(recorder.calls, "destroy") == 1);
}

TEST_CASE("behavior runtime restores bounded instance state without restarting an established instance") {
    Recorder originalRecorder;
    BehaviorRegistry originalRegistry = Registry(originalRecorder);
    auto scene = RuntimeScene::Create(Definition(), SceneRuntimeId{13});
    REQUIRE(scene.HasValue());
    auto original = BehaviorRuntime::Create(*scene.Value(), originalRegistry);
    REQUIRE(original.HasValue());
    REQUIRE(original.Value()->FixedUpdate({}, FixedDeltaTime{1.0 / 60.0}).HasValue());
    auto snapshot = original.Value()->CaptureReloadSnapshot();
    REQUIRE(snapshot.HasValue());
    original.Value()->Shutdown();

    Recorder replacementRecorder;
    BehaviorRegistry replacementRegistry = Registry(replacementRecorder);
    auto replacement = BehaviorRuntime::Create(*scene.Value(), replacementRegistry);
    REQUIRE(replacement.HasValue());
    REQUIRE(replacement.Value()->RestoreReloadSnapshot(snapshot.Value()).HasValue());
    REQUIRE(replacement.Value()->FixedUpdate({}, FixedDeltaTime{1.0 / 60.0}).HasValue());
    REQUIRE(std::ranges::count(replacementRecorder.calls, "start") == 0);
    REQUIRE(std::ranges::count(replacementRecorder.calls, "fixed") == 1);
}

TEST_CASE("behavior runtime rejects an oversized reload payload without shutting down the active generation") {
    Recorder recorder;
    recorder.reloadStateBytes = MaximumBehaviorReloadStateBytes + 1;
    BehaviorRegistry registry = Registry(recorder);
    auto scene = RuntimeScene::Create(Definition(), SceneRuntimeId{14});
    REQUIRE(scene.HasValue());
    auto runtime = BehaviorRuntime::Create(*scene.Value(), registry);
    REQUIRE(runtime.HasValue());

    auto snapshot = runtime.Value()->CaptureReloadSnapshot();
    REQUIRE(snapshot.HasError());
    REQUIRE(snapshot.ErrorValue().code.Value() == GameplayErrors::GameplayReloadSnapshotInvalid.code.Value());
    REQUIRE(runtime.Value()->FixedUpdate({}, FixedDeltaTime{1.0 / 60.0}).HasValue());
}
