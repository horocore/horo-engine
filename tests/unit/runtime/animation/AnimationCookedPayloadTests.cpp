#include "AnimationGraphInstanceTestFixtures.h"
#include "AnimationPoseCompositionTestFixtures.h"
#include "Horo/Animation/AnimationCookedAssets.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>

namespace Horo::Animation {
    namespace {
        template <typename T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "success"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        Assets::AssetDependency Role(Assets::AssetId id, std::string_view type) {
            return {id, Take(Assets::AssetTypeId::Parse(type))};
        }

        struct Payloads final {
            AnimationCookedPayload skeleton;
            std::vector<AnimationCookedPayload> clips;
            std::vector<AnimationCookedDependencyInput> inputs;

            Payloads(const SkeletonAsset &sourceSkeleton, std::span<const AnimationClipAsset> sourceClips)
                : skeleton(Take(EncodeSkeletonAnimationPayload(sourceSkeleton))) {
                clips.reserve(sourceClips.size());
                for (const auto &clip : sourceClips)
                    clips.push_back(Take(EncodeClipAnimationPayload(clip, skeleton.bytes)));
                inputs.push_back({Role(skeleton.asset, "core.animation.skeleton"), skeleton.bytes});
                for (const auto &clip : clips)
                    inputs.push_back({Role(clip.asset, "core.animation.clip"), clip.bytes});
                std::ranges::sort(inputs, {}, [](const auto &input) {
                    return input.metadata.id;
                });
            }
        };

        std::vector<AnimationClipAsset> RebindClips(const Payloads &payloads, const SkeletonAsset &skeleton,
                                                    SkeletonAssetGeneration skeletonGeneration) {
            std::vector<AnimationClipAsset> clips;
            std::uint64_t next{100};
            for (const auto &payload : payloads.clips) {
                auto portable = Take(DecodeClipAnimationPayload(payload.bytes, Take(AnimationClipId::Create(payload.asset))));
                AnimationClipData data;
                std::optional<AnimationReferencePoseBinding> reference;
                if (portable.referencePose)
                    reference = AnimationReferencePoseBinding{*portable.referencePose, Test::Id<AnimationReferencePoseGeneration>(next++)};
                data.descriptor = {portable.contractVersion, portable.id,        Test::Id<AnimationClipGeneration>(next++),
                                   portable.skeleton,        skeletonGeneration, portable.duration,
                                   portable.sampleRate,      portable.wrapMode,  portable.kind,
                                   portable.compression,     reference,          portable.hasRootMotion};
                data.tracks = std::move(portable.tracks);
                data.syncMarkers = std::move(portable.syncMarkers);
                data.eventSchemas = std::move(portable.eventSchemas);
                data.events = std::move(portable.events);
                clips.push_back(Take(AnimationClipAsset::Create(std::move(data), skeleton)));
                CHECK(Take(EncodeClipAnimationPayload(clips.back(), payloads.skeleton.bytes)).bytes == payload.bytes);
            }
            return clips;
        }

        void CheckCompiledDeclarations(const AnimationGraphCookedData &decoded, const AnimationGraphProgram &program) {
            CHECK(decoded.id == program.Id());
            CHECK(decoded.skeleton == program.Skeleton());
            CHECK(decoded.entry == program.Definitions()[program.EntryIndex()].id);
            CHECK(std::ranges::equal(decoded.parameters, program.Parameters()));
            CHECK(std::ranges::equal(decoded.actionSlots, program.ActionSlots()));
            CHECK(std::ranges::equal(decoded.actions, program.Actions()));
            // This includes every operand's nonzero source pin, not only instruction-level node identity.
            CHECK(std::ranges::equal(decoded.definitions, program.Definitions()));
        }

        AnimationGraphCompileContext Context(const SkeletonAsset &skeleton, std::span<const AnimationClipAsset> clips) {
            AnimationGraphCompileContext context;
            context.skeleton = &skeleton;
            context.skeletonGeneration = Test::Id<SkeletonAssetGeneration>(5);
            context.clips = clips;
            return context;
        }
    }  // namespace

    TEST_CASE("Animation binary payload binds actual canonical dependency content and actual native program",
              "[animation][cooked][payload]") {
        Test::Instance::Prepared fixture(Test::Instance::Blended());
        Payloads payloads(*fixture.skeleton, fixture.compileClips);
        const auto graph = Take(EncodeGraphAnimationPayload(*fixture.program, payloads.inputs));
        const auto skeleton = Take(DecodeSkeletonAnimationPayload(payloads.skeleton.bytes, fixture.skeleton->Data().skeleton));
        CHECK(skeleton.Data() == fixture.skeleton->Data());
        const auto decoded = Take(DecodeGraphAnimationPayload(graph.bytes, fixture.program->Id()));
        CheckCompiledDeclarations(decoded, *fixture.program);
        auto program = Take(AnimationGraphProgram::FromCooked(decoded, Context(skeleton, fixture.compileClips)));
        CHECK(program == *fixture.program);
        const auto bindings = Take(InspectAnimationPayloadBindings(graph.bytes, AnimationCookedKind::Graph, graph.asset, 16));
        REQUIRE(bindings.size() == payloads.inputs.size());
        for (std::size_t i = 0; i < bindings.size(); ++i) {
            CHECK(bindings[i].metadata.id == payloads.inputs[i].metadata.id);
            CHECK(bindings[i].payloadDigest == ComputeSha256(std::as_bytes(payloads.inputs[i].payload)));
        }
        auto assets = fixture.Assets();
        assets.program = std::make_shared<const AnimationGraphProgram>(std::move(program));
        auto instance = Take(AnimationGraphInstance::Create(assets, fixture.context));
        const auto tick = Test::Id<AnimationTickId>(1);
        REQUIRE(instance.Evaluate({tick, {10}}).HasValue());
        const auto committed = instance.Commit(tick);
        REQUIRE(committed.HasValue());
        std::array<Math::Transform, 1> pose;
        REQUIRE(instance.CopyPose(committed.publication.currentPose, pose).HasValue());
        CHECK(pose[0].translation.x == 60.0F);
        const auto encodedAgain = Take(EncodeGraphAnimationPayload(*fixture.program, payloads.inputs));
        CHECK(encodedAgain.bytes == graph.bytes);
    }

    TEST_CASE("Animation binary payload rejects wrong identity corruption truncation and trailing bytes", "[animation][cooked][payload]") {
        Test::Instance::Prepared fixture;
        const auto encoded = Take(EncodeSkeletonAnimationPayload(*fixture.skeleton));
        auto bytes = encoded.bytes;
        SECTION("different exact outer identity") {
            CHECK(DecodeSkeletonAnimationPayload(bytes, Test::Asset<SkeletonId>(99)).HasError());
        }
        SECTION("every truncated prefix") {
            for (std::size_t size = 0; size < bytes.size(); ++size)
                CHECK(DecodeSkeletonAnimationPayload(std::span{bytes}.first(size), fixture.skeleton->Data().skeleton).HasError());
        }
        SECTION("checksum corruption") {
            bytes.back() ^= 1;
            CHECK(DecodeSkeletonAnimationPayload(bytes, fixture.skeleton->Data().skeleton).HasError());
        }
        SECTION("trailing byte") {
            bytes.push_back(0);
            CHECK(DecodeSkeletonAnimationPayload(bytes, fixture.skeleton->Data().skeleton).HasError());
        }
    }

    TEST_CASE("Animation domain payload finite policy and cancellation reject without changing source", "[animation][cooked][payload]") {
        Test::Instance::Prepared fixture;
        const auto before = fixture.skeleton->Data();
        AnimationCookedContext context;
        CancellationSource cancellation;
        SECTION("closed") {
            context.accepting = false;
        }
        SECTION("cancelled") {
            cancellation.RequestCancellation();
            context.cancellation = cancellation.Token();
        }
        SECTION("zero bytes") {
            context.limits.bytes = 0;
        }
        SECTION("small byte bound") {
            context.limits.bytes = 64;
        }
        SECTION("zero owned storage") {
            context.limits.ownedBytes = 0;
        }
        SECTION("zero work") {
            context.limits.values = 0;
        }
        CHECK(EncodeSkeletonAnimationPayload(*fixture.skeleton, context).HasError());
        CHECK(fixture.skeleton->Data() == before);
    }

    TEST_CASE("Animation binary graph dependency declarations reject missing duplicate foreign and malformed actual payloads",
              "[animation][cooked][payload]") {
        Test::Instance::Prepared fixture;
        Payloads payloads(*fixture.skeleton, fixture.compileClips);
        SECTION("missing") {
            payloads.inputs.pop_back();
        }
        SECTION("duplicate") {
            payloads.inputs.push_back(payloads.inputs.front());
        }
        SECTION("foreign identity") {
            payloads.inputs.front().metadata.id = Test::Asset<SkeletonId>(99).Asset();
        }
        SECTION("wrong role") {
            payloads.inputs.front().metadata.expectedType = Take(Assets::AssetTypeId::Parse("core.mesh"));
        }
        SECTION("corrupt actual dependency") {
            payloads.skeleton.bytes.back() ^= 1;
        }
        CHECK(EncodeGraphAnimationPayload(*fixture.program, payloads.inputs).HasError());
    }

    TEST_CASE("Cooked additive graph retains local reference identity and executes actual base and reference operands",
              "[animation][cooked][payload][additive]") {
        Test::PoseComposition::LayerFixture fixture(Test::PoseComposition::AdditiveClipGraph(), true);
        const auto compiled = Take(fixture.Compile());
        Payloads payloads(*fixture.skeleton, fixture.compileClips);
        const auto encoded = Take(EncodeGraphAnimationPayload(compiled, payloads.inputs));
        for (const auto &clip : payloads.clips) {
            const auto decoded = Take(DecodeClipAnimationPayload(clip.bytes, Take(AnimationClipId::Create(clip.asset))));
            if (decoded.kind == AnimationClipKind::Additive)
                CHECK(decoded.referencePose == Test::Id<AnimationReferencePoseId>(44));
        }
        auto skeleton = std::make_shared<const SkeletonAsset>(
            Take(DecodeSkeletonAnimationPayload(payloads.skeleton.bytes, fixture.skeleton->Data().skeleton)));
        const auto generation = Test::Id<SkeletonAssetGeneration>(90);
        const auto clips = RebindClips(payloads, *skeleton, generation);
        auto context = Context(*skeleton, clips);
        context.skeletonGeneration = generation;
        auto decoded = Take(DecodeGraphAnimationPayload(encoded.bytes, compiled.Id()));
        CheckCompiledDeclarations(decoded, compiled);
        auto loaded = Take(AnimationGraphProgram::FromCooked(decoded, context));
        std::vector<std::shared_ptr<const AnimationClipAsset>> pins;
        for (const auto &clip : clips)
            pins.push_back(std::make_shared<const AnimationClipAsset>(clip));
        GraphInstanceAssets assets{std::make_shared<const AnimationGraphProgram>(std::move(loaded)), skeleton, generation, pins};
        auto instance = Take(AnimationGraphInstance::Create(assets, fixture.create));
        const auto tick = Test::Id<AnimationTickId>(1);
        REQUIRE(instance.Evaluate({tick, {10}}).HasValue());
        const auto committed = instance.Commit(tick);
        REQUIRE(committed.HasValue());
        std::array<Math::Transform, 3> pose;
        REQUIRE(instance.CopyPose(committed.publication.currentPose, pose).HasValue());
        CHECK(pose[0].translation.x == 25.0F);
        CHECK(pose[0].scale.x == 8.0F);
        CHECK(pose[1].translation.x == 10.0F);
        CHECK(pose[2].translation.x == 10.0F);
    }
}  // namespace Horo::Animation
