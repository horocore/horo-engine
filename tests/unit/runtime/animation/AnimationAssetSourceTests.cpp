#include "AnimationGraphInstanceTestFixtures.h"
#include "Horo/Animation/AnimationAssetSource.h"

#include <nlohmann/json.hpp>

namespace Horo::Animation {
    namespace {
        template <typename T> T TakeSource(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "success"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    }  // namespace

    TEST_CASE("Offline Skeleton and Clip source preserve complete semantics without sidecar or runtime generations",
              "[animation][source][assets]") {
        Test::Instance::Prepared fixture;
        const auto source = TakeSource(SerializeSkeletonAnimationSource(*fixture.skeleton));
        CHECK(source.starts_with("{\"schemaVersion\":1,\"assetType\":\"core.animation.skeleton\",\"contractVersion\":"));
        CHECK(source.ends_with("\n"));
        const auto decoded = TakeSource(DeserializeSkeletonAnimationSource(source, fixture.skeleton->Data().skeleton));
        CHECK(decoded.Data() == fixture.skeleton->Data());
        CHECK(TakeSource(SerializeSkeletonAnimationSource(decoded)) == source);
        const auto json = nlohmann::json::parse(source);
        CHECK_FALSE(json.at("payload").contains("skeleton"));
        CHECK(json.at("dependencies").empty());
        const auto &clip = fixture.compileClips.front();
        const auto clipSource = TakeSource(SerializeClipAnimationSource(clip));
        CHECK(clipSource.starts_with("{\"schemaVersion\":1,\"assetType\":\"core.animation.clip\",\"contractVersion\":"));
        const auto reorderedInput = nlohmann::json::parse(clipSource).dump();
        REQUIRE(DeserializeClipAnimationSource(reorderedInput, clip.Data().descriptor.id).HasValue());
        const auto portable = TakeSource(DeserializeClipAnimationSource(clipSource, clip.Data().descriptor.id));
        CHECK(portable.id == clip.Data().descriptor.id);
        CHECK(portable.skeleton == clip.Data().descriptor.skeleton);
        CHECK(portable.duration == clip.Data().descriptor.duration);
        CHECK(portable.tracks == clip.Data().tracks);
        CHECK(portable.events == clip.Data().events);
        CHECK(portable.eventSchemas == clip.Data().eventSchemas);
        const auto clipJson = nlohmann::json::parse(clipSource);
        CHECK_FALSE(clipJson.at("payload").contains("id"));
        CHECK_FALSE(clipJson.at("payload").contains("generation"));
        CHECK_FALSE(clipJson.at("payload").contains("skeletonGeneration"));
    }

    TEST_CASE("Offline asset source rejects unknown fields duplicate keys and dependency drift", "[animation][source][assets][malformed]") {
        Test::Instance::Prepared fixture;
        auto source = TakeSource(SerializeClipAnimationSource(fixture.compileClips.front()));
        const auto identity = fixture.compileClips.front().Data().descriptor.id;
        SECTION("unknown root") {
            auto value = nlohmann::json::parse(source);
            value["extra"] = true;
            source = value.dump();
        }
        SECTION("unknown payload") {
            auto value = nlohmann::json::parse(source);
            value["payload"]["generation"] = 4;
            source = value.dump();
        }
        SECTION("wrong dependency") {
            auto value = nlohmann::json::parse(source);
            value["dependencies"][0]["id"] = Test::Asset<SkeletonId>(99).Asset().ToString();
            source = value.dump();
        }
        SECTION("duplicate root key") {
            source.insert(1, "\"schemaVersion\":1,");
        }
        SECTION("unknown schema") {
            auto value = nlohmann::json::parse(source);
            value["schemaVersion"] = 2;
            source = value.dump();
        }
        SECTION("closed enum") {
            auto value = nlohmann::json::parse(source);
            value["payload"]["wrapMode"] = "future";
            source = value.dump();
        }
        SECTION("trailing content") {
            source += "{}";
        }
        CHECK(DeserializeClipAnimationSource(source, identity).HasError());
    }

    TEST_CASE("Offline asset source applies cold cancellation and bounded parser policy before native publication",
              "[animation][source][assets][limits]") {
        Test::Instance::Prepared fixture;
        const auto source = TakeSource(SerializeSkeletonAnimationSource(*fixture.skeleton));
        AnimationCookedContext context;
        AnimationAssetSourceLimits limits;
        CancellationSource cancellation;
        SECTION("closed") {
            context.accepting = false;
        }
        SECTION("cancelled") {
            cancellation.RequestCancellation();
            context.cancellation = cancellation.Token();
        }
        SECTION("byte bound") {
            limits.bytes = 1;
        }
        SECTION("depth bound") {
            limits.depth = 1;
        }
        SECTION("value bound") {
            limits.values = 1;
        }
        SECTION("above hard byte ceiling") {
            limits.bytes = 16U * 1024U * 1024U + 1;
        }
        CHECK(DeserializeSkeletonAnimationSource(source, fixture.skeleton->Data().skeleton, context, limits).HasError());
    }
}  // namespace Horo::Animation
