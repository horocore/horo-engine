#include "Horo/Network/SceneReplicationApplyOwner.h"
#include "ReplicationStateCodecTestSupport.h"

#include <catch2/catch_approx.hpp>

namespace Horo::Network {
    using namespace StateCodecTestSupport;

    namespace {
        std::shared_ptr<Runtime::RuntimeScene> MakeScene() {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, {1}};
            Runtime::RuntimeEntityDefinition definition;
            definition.object = Runtime::SceneObjectId{1};
            builder.Add(definition);
            return std::shared_ptr<Runtime::RuntimeScene>{
                Runtime::RuntimeScene::Create(std::move(builder).Build().Value(), Runtime::SceneRuntimeId{10}).Value()};
        }

        /** @brief Preserves valid scalar framing while replacing all quaternion payload values with zero. */
        void ZeroRotationValues(std::vector<std::byte> &wire, const std::size_t fields) {
            constexpr std::size_t fieldHeaderBytes = 16;
            constexpr std::size_t scalarBytes = 8;
            for (std::size_t index = 0; index < fields; ++index)
                Put(wire, WireHeaderBytes + index * (fieldHeaderBytes + scalarBytes) + fieldHeaderBytes, 0, scalarBytes);
        }

        /** @brief A decodable but invalid rotation leaves the actual replica unchanged. */
        void CheckRejectedRotation(SceneReplicationApplyOwner &owner, const Runtime::RuntimeScene &replica, ReplicationStateCodec &codec,
                                   std::span<const std::byte> wire) {
            auto decoded = codec.Decode(wire, Object());
            REQUIRE(decoded.HasValue());
            const std::array updates{ReplicationApplyUpdate{Object(), Recipient(), std::move(decoded).Value(), {}, {}}};
            REQUIRE(owner.Prepare(updates).HasError());
            CHECK(replica.View().Get(Object().entity).Value().localTransform->rotation == Math::Quaternion::Identity());
        }
    }  // namespace

    TEST_CASE("State encoding consumes actual committed Scene snapshots without reopening owner reads", "[network][state-codec][scene]") {
        const auto scene = MakeScene();
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

    TEST_CASE("Declared Scene rotation is captured and applied as one quaternion", "[network][state-codec][scene]") {
        using enum SceneReplicationProperty;
        const std::array fields{SceneReplicationFieldBinding{FieldIdValue(1), RotationX},
                                SceneReplicationFieldBinding{FieldIdValue(2), RotationY},
                                SceneReplicationFieldBinding{FieldIdValue(3), RotationZ},
                                SceneReplicationFieldBinding{FieldIdValue(4), RotationW}};
        const auto authority = MakeScene();
        const auto replica = MakeScene();
        const auto entity = Object().entity;
        const auto schema = Object().provenance;
        const auto partial = std::span{fields}.first(3);
        REQUIRE(SceneReplicationCommitSource::Create(authority, partial).HasError());
        REQUIRE(SceneReplicationApplyOwner::Create(replica, schema.schema, schema.schemaVersion, partial).HasError());
        const auto source = SceneReplicationCommitSource::Create(authority, fields).Value();
        const auto owner = SceneReplicationApplyOwner::Create(replica, schema.schema, schema.schemaVersion, fields).Value();

        auto world = Lifecycle();
        REQUIRE(world.RegisterObject(World().scene, World().session, Object()).HasValue());
        const auto registry = Registry(std::make_shared<CountingCodec>(), static_cast<std::uint32_t>(fields.size()));
        const std::array targets{ReplicationCaptureTarget{Object(), source}};
        auto capture = ReplicationStateCapture::Prepare(Read(world), registry, targets).Value();
        const auto codec = MakeCodec(registry);
        Runtime::SceneCommandBuffer commands;
        const Math::Transform rotated{.translation = {7, 8, 9}, .rotation = {0, 1, 0, 1}};
        commands.SetLocalTransform(entity, rotated);
        REQUIRE(source->CommitSimulationTick(commands, 1).HasValue());
        REQUIRE(source->CaptureAfterCommit(world, *capture, Runtime::RuntimePhase::NetworkFlush).HasValue());
        const auto wire = codec->Encode(capture->Latest(Object().object).Value()).Value();

        SECTION("A complete rotation preserves the receiver's undeclared translation") {
            const std::array updates{ReplicationApplyUpdate{Object(), Recipient(), codec->Decode(wire, Object()).Value(), {}, {}}};
            auto prepared = owner->Prepare(updates);
            REQUIRE(prepared.HasValue());
            const auto before = replica->View().Get(entity).Value().localTransform;
            CHECK(before->rotation == Math::Quaternion::Identity());
            REQUIRE(prepared.Value()->Commit({}).HasValue());
            const auto after = replica->View().Get(entity).Value().localTransform;
            CHECK(after->translation == Math::Vec3{});
            CHECK(after->rotation.y == Catch::Approx(0.70710678F));
            CHECK(after->rotation.w == Catch::Approx(0.70710678F));
        }

        SECTION("A valid scalar record cannot replace part of a quaternion") {
            Fixture partialCapture;
            partialCapture.Capture(1, 0.5);
            auto partialCodec = MakeCodec(partialCapture.registry);
            const auto partialWire = partialCodec->Encode(partialCapture.Pin()).Value();
            CheckRejectedRotation(*owner, *replica, *partialCodec, partialWire);
        }

        SECTION("A complete zero-length quaternion also leaves the replica unchanged") {
            auto zeroRotation = wire;
            ZeroRotationValues(zeroRotation, fields.size());
            CheckRejectedRotation(*owner, *replica, *codec, zeroRotation);
        }
    }
}  // namespace Horo::Network
