#include "ReplicationStateCodecTestSupport.h"

namespace Horo::Network {
    using namespace StateCodecTestSupport;

    TEST_CASE("State encoding consumes actual committed Scene snapshots without reopening owner reads", "[network][state-codec][scene]") {
        Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, {1}};
        Runtime::RuntimeEntityDefinition definition;
        definition.object = Runtime::SceneObjectId{1};
        builder.Add(definition);
        auto scene = std::shared_ptr<Runtime::RuntimeScene>{
            Runtime::RuntimeScene::Create(std::move(builder).Build().Value(), Runtime::SceneRuntimeId{10}).Value()};
        const auto entity = *scene->View().Find(Runtime::SceneObjectId{1});
        const std::array fields{SceneReplicationFieldBinding{FieldIdValue(1), SceneReplicationProperty::TranslationX},
                                SceneReplicationFieldBinding{FieldIdValue(2), SceneReplicationProperty::TranslationY},
                                SceneReplicationFieldBinding{FieldIdValue(3), SceneReplicationProperty::TranslationZ}};
        auto source = SceneReplicationCommitSource::Create(scene, fields).Value();
        auto lifecycle = Lifecycle();
        const auto serializer = std::make_shared<CountingCodec>();
        const auto registry = Registry(serializer, 3);
        REQUIRE(lifecycle.RegisterObject(World().scene, World().session, Object()).HasValue());
        const std::array targets{ReplicationCaptureTarget{Object(), source}};
        auto capture = ReplicationStateCapture::Prepare(Read(lifecycle), registry, targets).Value();
        auto codec = MakeCodec(registry);
        const auto commit = [&](const std::uint64_t tick, const Math::Vec3 position) {
            Runtime::SceneCommandBuffer commands;
            Math::Transform transform;
            transform.translation = position;
            commands.SetLocalTransform(entity, transform);
            REQUIRE(source->CommitSimulationTick(commands, tick).HasValue());
            REQUIRE(source->CaptureAfterCommit(lifecycle, *capture, Runtime::RuntimePhase::NetworkFlush).HasValue());
        };
        commit(1, {1, 2, 3});
        const auto first = capture->Latest(Object().object).Value();
        const auto full = codec->Encode(first).Value();
        auto decoded = codec->Decode(full, Object()).Value();
        commit(2, {1, 9, 3});
        const auto current = capture->Latest(Object().object).Value();
        const auto delta = codec->Encode(current, Ack(first, *codec)).Value();
        REQUIRE(delta.size() == WireHeaderBytes + 24);
        auto updated = codec->Decode(delta, Object(), &decoded);
        REQUIRE(updated.HasValue());
        REQUIRE(updated.Value().Fields().size() == 3);
        REQUIRE(std::get<double>(updated.Value().Fields()[0].value) == 1);
        REQUIRE(std::get<double>(updated.Value().Fields()[1].value) == 9);
        REQUIRE(std::get<double>(updated.Value().Fields()[2].value) == 3);
        REQUIRE(scene->View().Get(entity).Value().localTransform->translation == Math::Vec3{1, 9, 3});
        // Malformed suffix is rejected by the complete framing pass before any comparison/encoder callback.
        const auto calls = serializer->encodeCalls;
        auto malformed = full;
        Put(malformed, WireHeaderBytes + 48, 1, 4);  // Duplicate final field ID.
        REQUIRE(codec->Decode(malformed, Object()).HasError());
        REQUIRE(serializer->encodeCalls == calls);
        REQUIRE(scene->View().Get(entity).Value().localTransform->translation == Math::Vec3{1, 9, 3});
        capture->Shutdown();
        REQUIRE(codec->Encode(current).HasError());
        codec->Shutdown();
        REQUIRE_FALSE(decoded.IsCurrent());
    }
}  // namespace Horo::Network
