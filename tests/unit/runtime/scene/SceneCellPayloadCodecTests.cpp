#include "../world_streaming/StreamingCellCandidateTestSupport.h"
#include "Horo/Runtime/Scene/SceneCellPayloadCodec.h"
#include "TriggerVolumeCellTestValues.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime {
    namespace {
        namespace W = WorldStreaming;
        using W::TestSupport::RequireError;

        /** @brief Host-captured durable publication independent of every wire field. */
        SceneCellPayloadIdentity Identity(const std::uint64_t revision = 1) {
            return {W::TestSupport::World(), W::CandidateTestSupport::Cell(), {77}, {revision}};
        }

        /** @brief Independent wire, logical allocation and semantic limits for real owner admission. */
        SceneCellPayloadCodecLimits Limits() {
            return {1024 * 1024, 1024 * 1024, {8, 8, 1024 * 1024}};
        }

        /** @brief Real validated payload with hierarchy, typed transforms/camera and approved preserved opaque components. */
        struct Fixture final {
            W::CookedWorldIndexManifest manifest{W::CandidateTestSupport::Manifest()};
            Gameplay::ComponentTypeId type{Type()};
            std::array<SceneCellComponentSchema, 1> schemas{{{type, 1}}};
            std::array<RuntimeEntityDefinition, 2> entities{Entities(type)};

            static Gameplay::ComponentTypeId Type() {
                auto parsed = Gameplay::ComponentTypeId::Parse("game.test.inventory");
                REQUIRE(parsed.HasValue());
                return std::move(parsed).Value();
            }

            static std::array<RuntimeEntityDefinition, 2> Entities(const Gameplay::ComponentTypeId &type) {
                RuntimeEntityDefinition parent{.object = {1}};
                parent.localTransform.translation = {-1.25F, 2.5F, 3.75F};
                parent.components.camera = CameraComponent{};
                parent.components.light = LightComponent{};
                parent.components.gameplayComponents.push_back(
                    {type, 1, Gameplay::ComponentPayloadEncoding::CanonicalJson, {std::byte{'{'}, std::byte{'}'}}});
                RuntimeEntityDefinition child{.object = {2}, .parent = SceneObjectId{1}};
                child.localTransform.scale = {2.0F, 3.0F, 4.0F};
                return {std::move(parent), std::move(child)};
            }

            RuntimeSceneCellPayload Payload() const {
                auto cooked =
                    CookRuntimeSceneCellPayload(manifest.Descriptor(), {Identity(), entities, {}, schemas}, Identity(), Limits().semantic);
                if (cooked.HasError())
                    INFO(cooked.ErrorValue().message);
                REQUIRE(cooked.HasValue());
                return std::move(cooked).Value();
            }

            std::vector<std::byte> Bytes() const {
                auto bytes = EncodeSceneCellPayload(Payload(), Limits());
                REQUIRE(bytes.HasValue());
                return std::move(bytes).Value();
            }
        };
    }  // namespace

    TEST_CASE("Scene CoreEcs codec round trips actual typed hierarchy and approved opaque fields", "[scene][world_artifact][cell_codec]") {
        const Fixture fixture;
        const auto bytes = fixture.Bytes();
        REQUIRE(bytes.size() > 16);
        CHECK(bytes[0] == std::byte{'H'});
        CHECK(bytes[7] == std::byte{'S'});
        CHECK(bytes[8] == std::byte{1});
        auto decoded = DecodeSceneCellPayload(fixture.manifest.Descriptor(), bytes, Identity(), fixture.schemas, {}, Limits());
        if (decoded.HasError())
            INFO(decoded.ErrorValue().message);
        REQUIRE(decoded.HasValue());
        const auto &definition = decoded.Value().Definition();
        REQUIRE(definition.Entities().size() == 2);
        CHECK(definition.Entities()[0].localTransform.translation.x == -1.25F);
        CHECK(definition.Entities()[1].parent == SceneObjectId{1});
        CHECK(definition.Entities()[1].localTransform.scale.z == 4.0F);
        REQUIRE(definition.Entities()[0].components.camera);
        REQUIRE(definition.Entities()[0].components.light);
        REQUIRE(definition.Entities()[0].components.gameplayComponents.size() == 1);
        CHECK(definition.Entities()[0].components.gameplayComponents[0].typeId == fixture.type);
        CHECK(definition.Entities()[0].components.gameplayComponents[0].payload ==
              fixture.entities[0].components.gameplayComponents[0].payload);
        const auto encodedAgain = EncodeSceneCellPayload(decoded.Value(), Limits());
        REQUIRE(encodedAgain.HasValue());
        CHECK(encodedAgain.Value() == bytes);
    }

    TEST_CASE("Scene CoreEcs wire cannot approve opaque schemas or replace captured revision authority",
              "[scene][world_artifact][cell_codec]") {
        const Fixture fixture;
        const auto bytes = fixture.Bytes();
        RequireError(DecodeSceneCellPayload(fixture.manifest.Descriptor(), bytes, Identity(), {}, {}, Limits()),
                     SceneCellPayloadErrors::Unsupported);
        RequireError(DecodeSceneCellPayload(fixture.manifest.Descriptor(), bytes, Identity(2), fixture.schemas, {}, Limits()),
                     SceneCellPayloadErrors::Stale);
    }

    TEST_CASE("Scene CoreEcs codec rejects malformed truncated trailing and version-skewed bytes", "[scene][world_artifact][cell_codec]") {
        const Fixture fixture;
        auto bytes = fixture.Bytes();
        SECTION("Truncated header") {
            bytes.resize(15);
        }
        SECTION("Truncated body") {
            bytes.pop_back();
        }
        SECTION("Trailing bytes") {
            bytes.push_back(std::byte{0});
        }
        SECTION("Reserved field") {
            bytes[15] = std::byte{1};
        }
        SECTION("Impossible fixed GUID count") {
            bytes[16] = std::byte{255};
        }
        CHECK(DecodeSceneCellPayload(fixture.manifest.Descriptor(), bytes, Identity(), fixture.schemas, {}, Limits()).HasError());
        bytes = fixture.Bytes();
        bytes[8] = std::byte{1};
        RequireError(DecodeSceneCellPayload(fixture.manifest.Descriptor(), bytes, Identity(), fixture.schemas, {}, Limits()),
                     SceneCellPayloadErrors::Unsupported);
    }

    TEST_CASE("Scene CoreEcs decoder admits exact bounds and rejects narrower counts before vector allocation",
              "[scene][world_artifact][cell_codec]") {
        const Fixture fixture;
        const auto bytes = fixture.Bytes();
        auto limits = Limits();
        limits.maximumEncodedBytes = bytes.size();
        CHECK(DecodeSceneCellPayload(fixture.manifest.Descriptor(), bytes, Identity(), fixture.schemas, {}, limits).HasValue());
        --limits.maximumEncodedBytes;
        RequireError(DecodeSceneCellPayload(fixture.manifest.Descriptor(), bytes, Identity(), fixture.schemas, {}, limits),
                     SceneCellPayloadErrors::CapacityExceeded);
        limits = Limits();
        limits.semantic.maximumEntities = 1;
        RequireError(DecodeSceneCellPayload(fixture.manifest.Descriptor(), bytes, Identity(), fixture.schemas, {}, limits),
                     SceneCellPayloadErrors::CapacityExceeded);
        limits = Limits();
        limits.maximumDecodeAllocationBytes = 1;
        RequireError(DecodeSceneCellPayload(fixture.manifest.Descriptor(), bytes, Identity(), fixture.schemas, {}, limits),
                     SceneCellPayloadErrors::CapacityExceeded);
        RequireError(EncodeSceneCellPayload(fixture.Payload(), {Limits().maximumEncodedBytes, 1, {1, 8, 1024 * 1024}}),
                     SceneCellPayloadErrors::CapacityExceeded);
    }

    TEST_CASE("Scene CoreEcs cancellation exposes no decoded baseline and preserves owner source", "[scene][world_artifact][cell_codec]") {
        const Fixture fixture;
        const auto bytes = fixture.Bytes();
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(EncodeSceneCellPayload(fixture.Payload(), Limits(), cancellation.Token()), SceneCellPayloadErrors::Cancelled);
        RequireError(DecodeSceneCellPayload(fixture.manifest.Descriptor(), bytes, Identity(), fixture.schemas, {}, Limits(),
                                            cancellation.Token()),
                     SceneCellPayloadErrors::Cancelled);
        CHECK(fixture.entities[1].parent == SceneObjectId{1});
        CHECK(fixture.manifest.Cells().size() == 3);
    }

    TEST_CASE("CoreEcs version two preserves portable trigger proof and rejects old providers", "[scene][trigger][cell_codec]") {
        const auto manifest = W::CandidateTestSupport::Manifest();
        const Test::TriggerCellValues values;
        const SceneCellPayloadSource source{.identity = Identity(), .entities = values.entities, .triggerVolumeEvidence = values.proof};
        const auto payload = CookRuntimeSceneCellPayload(manifest.Descriptor(), source, Identity(), Limits().semantic);
        REQUIRE(payload.HasValue());
        const auto bytes = EncodeSceneCellPayload(payload.Value(), Limits());
        REQUIRE(bytes.HasValue());
        const auto decoded = DecodeSceneCellPayload(manifest.Descriptor(), bytes.Value(), Identity(), {}, {}, Limits());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().Definition().TriggerVolumeEvidence().front() == values.proof.front());
        REQUIRE(EncodeSceneCellPayload(decoded.Value(), Limits()).Value() == bytes.Value());
        auto stale = bytes.Value();
        stale[8] = std::byte{1};
        REQUIRE(DecodeSceneCellPayload(manifest.Descriptor(), stale, Identity(), {}, {}, Limits()).HasError());
    }
}  // namespace Horo::Runtime
