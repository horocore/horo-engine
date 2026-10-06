#include "Horo/Runtime/Scene/AudioSceneExtraction.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <type_traits>

std::size_t RuntimeSceneAllocationCount() noexcept;

namespace {
    using namespace Horo;
    using namespace Horo::Audio;
    using namespace Horo::Runtime;

    const AudioSceneContextHandle Context{AudioRuntimeId::Create(9).Value(), 1, 1};
    static_assert(std::is_trivially_copyable_v<AudioSpatialFrame>);

    RuntimeEntityDefinition Object(std::uint64_t id) {
        RuntimeEntityDefinition entity;
        entity.object = {id};
        return entity;
    }

    std::unique_ptr<RuntimeScene> Scene(std::vector<RuntimeEntityDefinition> entities, std::uint64_t runtime = 1) {
        SceneDefinitionBuilder builder{{1}, {1}};
        for (auto &entity : entities)
            builder.Add(std::move(entity));
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        auto scene = RuntimeScene::Create(definition.Value(), {runtime});
        REQUIRE(scene.HasValue());
        return std::move(scene).Value();
    }

    AudioSceneExtractionInput Input(std::uint64_t sequence, AudioListenerPolicy policy = AudioListenerPolicy::Primary) {
        return {.context = Context, .sequence = sequence, .elapsedSeconds = 0.5F, .policy = policy};
    }

    void Move(RuntimeScene &scene, SceneObjectId object, Math::Transform transform) {
        SceneCommandBuffer commands;
        commands.SetLocalTransform(*scene.View().Find(object), transform);
        REQUIRE(scene.Commit(commands).HasValue());
    }
}  // namespace

TEST_CASE("Audio scene extracts hierarchy poses and continuous or explicit world velocity", "[scene][audio][spatial]") {
    auto parent = Object(1);
    parent.localTransform.translation = {10, 0, 0};
    parent.localTransform.scale = {2, 2, 2};
    parent.localTransform.rotation = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi / 2);
    auto source = Object(2);
    source.parent = SceneObjectId{1};
    source.localTransform.translation = {0, 0, -1};
    source.components.audioSource = AudioSourceComponent{};
    auto scene = Scene({parent, source});
    AudioSceneExtractor extractor;
    auto first = extractor.Capture(scene->View(), Input(1));
    REQUIRE(first.HasValue());
    REQUIRE(first.Value().sourceCount == 1);
    const auto initial = first.Value().Sources()[0];
    REQUIRE(Math::NearlyEqual(initial.motion.current.position, Math::Vec3{8, 0, 0}));
    REQUIRE(Math::NearlyEqual(initial.motion.current.orientation.Rotate({0, 0, -1}), Math::Vec3{-1, 0, 0}));
    REQUIRE(initial.motion.discontinuous);
    REQUIRE(initial.motion.previous == initial.motion.current);
    REQUIRE(initial.motion.velocity == Math::Vec3{});

    parent.localTransform.translation.x = 11;
    Move(*scene, {1}, parent.localTransform);
    auto second = extractor.Capture(scene->View(), Input(2));
    REQUIRE(second.HasValue());
    REQUIRE_FALSE(second.Value().Sources()[0].motion.discontinuous);
    REQUIRE(Math::NearlyEqual(second.Value().Sources()[0].motion.velocity, Math::Vec3{2, 0, 0}));
    REQUIRE(second.Value().Sources()[0].motion.previous == initial.motion.current);
    // The previously published frame remains a self-contained value after Scene mutation.
    REQUIRE(first.Value().Sources()[0].motion.current == initial.motion.current);

    AudioSceneMotionInput override{*scene->View().Find({2}), Math::Vec3{4, 5, 6}, 0};
    auto input = Input(3);
    input.motion = {&override, 1};
    auto explicitMotion = extractor.Capture(scene->View(), input);
    REQUIRE(explicitMotion.HasValue());
    REQUIRE(explicitMotion.Value().Sources()[0].motion.velocity == Math::Vec3{4, 5, 6});
    override.discontinuityRevision = 1;
    input.sequence = 4;
    auto teleport = extractor.Capture(scene->View(), input);
    REQUIRE(teleport.HasValue());
    REQUIRE(teleport.Value().Sources()[0].motion.discontinuous);
    REQUIRE(teleport.Value().Sources()[0].motion.velocity == Math::Vec3{});
    REQUIRE(teleport.Value().Sources()[0].motion.previous == teleport.Value().Sources()[0].motion.current);
    input.sequence = 5;
    auto resumed = extractor.Capture(scene->View(), input);
    REQUIRE(resumed.HasValue());
    REQUIRE_FALSE(resumed.Value().Sources()[0].motion.discontinuous);
    REQUIRE(resumed.Value().Sources()[0].motion.velocity == Math::Vec3{4, 5, 6});
}

TEST_CASE("Audio listener policies are deterministic and retain unselected motion", "[scene][audio][spatial]") {
    auto a = Object(1);
    auto b = Object(2);
    auto c = Object(3);
    a.components.audioListener = AudioListenerComponent{1, 10, 1};
    b.components.audioListener = AudioListenerComponent{1, 10, 3};
    c.components.audioListener = AudioListenerComponent{2, 5, 2};
    auto scene = Scene({a, b, c});
    AudioSceneExtractor extractor;
    auto primary = extractor.Capture(scene->View(), Input(1));
    REQUIRE(primary.HasValue());
    REQUIRE(primary.Value().listenerCount == 1);
    REQUIRE(primary.Value().Listeners()[0].identity.slot == 1);
    REQUIRE(primary.Value().Listeners()[0].weight == 1);
    b.localTransform.translation = {1, 0, 0};
    Move(*scene, {2}, b.localTransform);
    auto perView = extractor.Capture(scene->View(), Input(2, AudioListenerPolicy::PerView));
    REQUIRE(perView.HasValue());
    REQUIRE(perView.Value().listenerCount == 2);
    REQUIRE(perView.Value().Listeners()[0].view == 1);
    REQUIRE(perView.Value().Listeners()[1].view == 2);
    REQUIRE(perView.Value().Listeners()[0].identity.slot == 1);
    auto all = extractor.Capture(scene->View(), Input(3, AudioListenerPolicy::WeightedAll));
    REQUIRE(all.HasValue());
    REQUIRE(all.Value().listenerCount == 3);
    REQUIRE_FALSE(all.Value().Listeners()[1].motion.discontinuous);
    REQUIRE(Math::NearlyEqual(all.Value().Listeners()[1].motion.previous.position, Math::Vec3{1, 0, 0}));
    REQUIRE(Math::NearlyEqual(all.Value().Listeners()[1].weight, 0.5F));
    REQUIRE(
        Math::NearlyEqual(all.Value().Listeners()[0].weight + all.Value().Listeners()[1].weight + all.Value().Listeners()[2].weight, 1));
    SceneCommandBuffer destroy;
    destroy.Destroy(*scene->View().Find({1}));
    REQUIRE(scene->Commit(destroy).HasValue());
    auto replacement = extractor.Capture(scene->View(), Input(4));
    REQUIRE(replacement.HasValue());
    REQUIRE(replacement.Value().Listeners()[0].identity.slot == 2);
}

TEST_CASE("Audio extraction rejects invalid input atomically and closes on shutdown", "[scene][audio][spatial]") {
    auto source = Object(1);
    source.components.audioSource = AudioSourceComponent{};
    auto scene = Scene({source});
    AudioSceneExtractor extractor;
    REQUIRE(extractor.Capture(scene->View(), Input(1)).HasValue());
    Move(*scene, {1}, Math::Transform{.translation = {1, 0, 0}});
    auto invalid = Input(2);
    invalid.elapsedSeconds = 0;
    REQUIRE(extractor.Capture(scene->View(), invalid).HasError());
    invalid.elapsedSeconds = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(extractor.Capture(scene->View(), invalid).HasError());
    invalid = Input(2);
    invalid.policy = static_cast<AudioListenerPolicy>(99);
    REQUIRE(extractor.Capture(scene->View(), invalid).HasError());
    const auto entity = *scene->View().Find({1});
    std::array<AudioSceneMotionInput, 2> duplicate{{{entity, {}, 0}, {entity, {}, 0}}};
    invalid = Input(2);
    invalid.motion = duplicate;
    REQUIRE(extractor.Capture(scene->View(), invalid).HasError());
    duplicate[0].velocity = Math::Vec3{std::numeric_limits<float>::infinity(), 0, 0};
    invalid.motion = {duplicate.data(), 1};
    REQUIRE(extractor.Capture(scene->View(), invalid).HasError());
    auto recovered = extractor.Capture(scene->View(), Input(2));
    REQUIRE(recovered.HasValue());
    REQUIRE(Math::NearlyEqual(recovered.Value().Sources()[0].motion.velocity, Math::Vec3{2, 0, 0}));
    REQUIRE(extractor.Capture(scene->View(), Input(2)).HasError());
    auto staleView = scene->View();
    Move(*scene, {1}, Math::Transform{});
    REQUIRE(extractor.Capture(staleView, Input(3)).HasError());
    extractor.Shutdown();
    extractor.Shutdown();
    REQUIRE(extractor.Capture(scene->View(), Input(3)).HasError());
}

TEST_CASE("Audio extraction resets generations and requires new contexts for replacement scenes", "[scene][audio][spatial]") {
    auto source = Object(1);
    source.components.audioSource = AudioSourceComponent{};
    auto scene = Scene({source});
    AudioSceneExtractor extractor;
    auto before = extractor.Capture(scene->View(), Input(1));
    REQUIRE(before.HasValue());
    auto replacement = Scene({source}, 2);
    REQUIRE(extractor.Capture(replacement->View(), Input(2)).HasError());
    auto input = Input(2);
    input.context.generation = 2;
    auto after = extractor.Capture(replacement->View(), input);
    REQUIRE(after.HasValue());
    REQUIRE(after.Value().Sources()[0].identity != before.Value().Sources()[0].identity);
    REQUIRE(after.Value().Sources()[0].motion.discontinuous);
    SceneCommandBuffer destroy;
    destroy.Destroy(*replacement->View().Find({1}));
    REQUIRE(replacement->Commit(destroy).HasValue());
    input.sequence = 3;
    REQUIRE(extractor.Capture(replacement->View(), input).Value().sourceCount == 0);
    SceneCommandBuffer create;
    RuntimeEntityCreateInfo info;
    info.components.audioSource = AudioSourceComponent{};
    (void)create.Create(info);
    REQUIRE(replacement->Commit(create).HasValue());
    input.sequence = 4;
    auto reused = extractor.Capture(replacement->View(), input);
    REQUIRE(reused.HasValue());
    REQUIRE(reused.Value().Sources()[0].identity != after.Value().Sources()[0].identity);
    REQUIRE(reused.Value().Sources()[0].motion.discontinuous);
}

TEST_CASE("Audio spatial extraction bounds sources listeners and scene work", "[scene][audio][spatial]") {
    std::vector<RuntimeEntityDefinition> objects;
    for (std::size_t index = 0; index < MaximumSpatialAudioSources; ++index) {
        auto source = Object(index + 1);
        source.components.audioSource = AudioSourceComponent{};
        objects.push_back(source);
    }
    auto atLimit = Scene(objects);
    AudioSceneExtractor extractor;
    REQUIRE(extractor.Capture(atLimit->View(), Input(1)).Value().sourceCount == MaximumSpatialAudioSources);
    objects.push_back(objects.back());
    objects.back().object = {MaximumSpatialAudioSources + 1};
    auto overflow = Scene(objects, 2);
    auto input = Input(2);
    input.context.generation = 2;
    REQUIRE(extractor.Capture(overflow->View(), input).HasError());
    REQUIRE(extractor.Capture(atLimit->View(), Input(2)).HasValue());
    objects.clear();
    for (std::size_t index = 0; index <= MaximumSpatialAudioListeners; ++index) {
        auto listener = Object(index + 1);
        listener.components.audioListener = AudioListenerComponent{};
        objects.push_back(listener);
    }
    auto last = objects.back();
    objects.pop_back();
    auto listenerLimit = Scene(objects, 3);
    input.context.generation = 3;
    input.sequence = 3;
    REQUIRE(extractor.Capture(listenerLimit->View(), input).Value().listenerCount == 1);
    objects.push_back(last);
    auto listeners = Scene(objects, 5);
    input.context.generation = 5;
    input.sequence = 4;
    REQUIRE(extractor.Capture(listeners->View(), input).HasError());
    auto empty = Scene({}, 4);
    input.context.generation = 4;
    input.sequence = 4;
    REQUIRE(extractor.Capture(empty->View(), input).Value().listenerCount == 0);
}

TEST_CASE("Authored audio listeners reject nonfinite weights and unknown split views", "[scene][audio][spatial]") {
    auto listener = Object(1);
    listener.components.audioListener = AudioListenerComponent{MaximumSpatialAudioViews + 1};
    REQUIRE(ValidateRuntimeEntityDefinition(listener).HasError());
    listener.components.audioListener = AudioListenerComponent{0, 0, 0};
    REQUIRE(ValidateRuntimeEntityDefinition(listener).HasError());
    listener.components.audioListener->weight = std::numeric_limits<float>::infinity();
    REQUIRE(ValidateRuntimeEntityDefinition(listener).HasError());
    listener.components.audioListener = AudioListenerComponent{};
    REQUIRE(ValidateRuntimeEntityDefinition(listener).HasValue());
}

TEST_CASE("Audio extraction contains motion overflow and global discontinuities", "[scene][audio][spatial]") {
    auto source = Object(1);
    source.components.audioSource = AudioSourceComponent{};
    auto scene = Scene({source});
    AudioSceneExtractor extractor;
    REQUIRE(extractor.Capture(scene->View(), Input(1)).HasValue());
    Move(*scene, {1}, Math::Transform{.translation = {std::numeric_limits<float>::max(), 0, 0}});
    auto input = Input(2);
    input.elapsedSeconds = std::numeric_limits<float>::min();
    REQUIRE(extractor.Capture(scene->View(), input).HasError());
    input.discontinuous = true;
    auto reset = extractor.Capture(scene->View(), input);
    REQUIRE(reset.HasValue());
    REQUIRE(reset.Value().Sources()[0].motion.velocity == Math::Vec3{});
}

TEST_CASE("Audio extraction hierarchy and slot budgets reject excess work", "[scene][audio][spatial]") {
    std::vector<RuntimeEntityDefinition> objects;
    for (std::uint64_t index = 1; index <= 65; ++index) {
        auto object = Object(index);
        if (index > 1)
            object.parent = SceneObjectId{index - 1};
        if (index == 65)
            object.components.audioSource = AudioSourceComponent{};
        objects.push_back(object);
    }
    auto deep = Scene(objects);
    AudioSceneExtractor extractor;
    REQUIRE(extractor.Capture(deep->View(), Input(1)).HasError());
    objects.clear();
    for (std::uint64_t index = 1; index <= 4097; ++index)
        objects.push_back(Object(index));
    auto oversized = Scene(objects, 2);
    REQUIRE(extractor.Capture(oversized->View(), Input(1)).HasError());
}

TEST_CASE("Spatial scene capture does not allocate at simultaneous source listener and override ceilings", "[scene][audio][spatial]") {
    std::vector<RuntimeEntityDefinition> objects;
    for (std::size_t index = 0; index < MaximumSpatialAudioSources + MaximumSpatialAudioListeners; ++index) {
        auto object = Object(index + 1);
        if (index < MaximumSpatialAudioSources)
            object.components.audioSource = AudioSourceComponent{};
        else
            object.components.audioListener = AudioListenerComponent{};
        objects.push_back(object);
    }
    auto scene = Scene(objects);
    std::vector<AudioSceneMotionInput> overrides;
    for (const auto &object : objects)
        overrides.push_back({*scene->View().Find(object.object), Math::Vec3{1, 2, 3}, 0});
    AudioSceneExtractor extractor;
    auto input = Input(1, AudioListenerPolicy::WeightedAll);
    input.motion = overrides;
    REQUIRE(extractor.Capture(scene->View(), input).HasValue());
    input.sequence = 2;
    const auto before = RuntimeSceneAllocationCount();
    auto frame = extractor.Capture(scene->View(), input);
    const auto after = RuntimeSceneAllocationCount();
    REQUIRE(frame.HasValue());
    REQUIRE(frame.Value().sourceCount == MaximumSpatialAudioSources);
    REQUIRE(frame.Value().listenerCount == MaximumSpatialAudioListeners);
    REQUIRE(after == before);
}

TEST_CASE("Disabled listeners and sources stay absent and malformed contexts stay rejected", "[scene][audio][spatial]") {
    auto object = Object(1);
    object.components.audioSource = AudioSourceComponent{};
    object.components.audioSource->enabled = false;
    object.components.audioListener = AudioListenerComponent{};
    object.components.audioListener->enabled = false;
    auto scene = Scene({object});
    AudioSceneExtractor extractor;
    REQUIRE(extractor.Capture({}, Input(1)).HasError());
    auto input = Input(1);
    input.context = {};
    REQUIRE(extractor.Capture(scene->View(), input).HasError());
    input = Input(1);
    AudioSceneMotionInput foreign{{{999}, {0, 1}}, {}, 0};
    input.motion = {&foreign, 1};
    REQUIRE(extractor.Capture(scene->View(), input).HasError());
    auto frame = extractor.Capture(scene->View(), Input(1));
    REQUIRE(frame.HasValue());
    REQUIRE(frame.Value().sourceCount == 0);
    REQUIRE(frame.Value().listenerCount == 0);
}

TEST_CASE("Weighted listener selection avoids float summation overflow", "[scene][audio][spatial]") {
    auto a = Object(1);
    auto b = Object(2);
    a.components.audioListener = AudioListenerComponent{0, 0, std::numeric_limits<float>::max()};
    b.components.audioListener = AudioListenerComponent{1, 0, std::numeric_limits<float>::max()};
    auto scene = Scene({a, b});
    AudioSceneExtractor extractor;
    auto frame = extractor.Capture(scene->View(), Input(1, AudioListenerPolicy::WeightedAll));
    REQUIRE(frame.HasValue());
    REQUIRE(frame.Value().Listeners()[0].weight == 0.5F);
    REQUIRE(frame.Value().Listeners()[1].weight == 0.5F);
    scene.reset();
    REQUIRE(frame.Value().Listeners()[1].motion.current.orientation == Math::Quaternion::Identity());
}

TEST_CASE("Audio motion overrides reject unrelated live scene entities", "[scene][audio][spatial]") {
    auto scene = Scene({Object(1)});
    AudioSceneExtractor extractor;
    AudioSceneMotionInput unrelated{*scene->View().Find({1}), Math::Vec3{1, 2, 3}, 0};
    auto input = Input(1);
    input.motion = {&unrelated, 1};
    REQUIRE(extractor.Capture(scene->View(), input).HasError());
    REQUIRE(extractor.Capture(scene->View(), Input(1)).HasValue());
}
