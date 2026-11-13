#include "AnimationActionTestFixtures.h"
#include "AnimationClipSceneProgramAccess.h"
#include "AnimationGraphInstanceTestFixtures.h"

namespace Horo::Animation {
    namespace {
        /** @brief Pins actual immutable fixture publications; the bridge never receives a generated GraphId. */
        AnimationGraphCompileContext ClipContext(const Test::Instance::Prepared &fixture) {
            AnimationGraphCompileContext context;
            context.skeleton = fixture.skeleton.get();
            context.skeletonGeneration = Test::Id<SkeletonAssetGeneration>(5);
            context.clips = fixture.compileClips;
            return context;
        }

        /** @brief Owns real single-clip compiler bindings and no authoring parser. */
        std::shared_ptr<const AnimationGraphProgram> Program(const Test::Instance::Prepared &fixture) {
            auto prepared = Detail::ClipSceneProgramAccess::Prepare(Test::Asset<AnimationClipId>(3), ClipContext(fixture));
            REQUIRE(prepared.HasValue());
            return std::make_shared<const AnimationGraphProgram>(std::move(prepared).Value());
        }
    }  // namespace

    TEST_CASE("Direct Scene clip uses the actual executor without a synthetic persistent graph identity", "[animation][clip][scene]") {
        Test::Instance::Prepared fixture;
        auto assets = fixture.Assets();
        assets.program = Program(fixture);
        CHECK_FALSE(assets.program->Id().IsValid());
        CHECK(assets.program->Source().kind == AnimationSourceKind::Clip);
        CHECK(assets.program->Source().clip == Test::Asset<AnimationClipId>(3));
        CHECK_FALSE(assets.program->Source().graph.IsValid());
        auto created = AnimationGraphInstance::Create(assets, fixture.context);
        REQUIRE(created.HasValue());
        auto instance = std::move(created).Value();
        const auto tick = Test::Id<AnimationTickId>(1);
        std::array<Math::Transform, 1> pose;
        const auto before = HoroAnimationTestAllocationCount();
        const auto evaluated = instance.Evaluate({tick, {10}});
        const auto published = instance.Commit(tick);
        const auto copied = instance.CopyPose(published.publication.currentPose, pose);
        const auto after = HoroAnimationTestAllocationCount();
        REQUIRE(evaluated.HasValue());
        REQUIRE(published.HasValue());
        REQUIRE(copied.HasValue());
        CHECK(pose[0].translation.x == 10.0F);
        CHECK(after == before);
    }

    TEST_CASE("Direct clip reload preserves source identity and retires genuine prior pose handles", "[animation][clip][scene]") {
        Test::Instance::Prepared first;
        auto assets = first.Assets();
        assets.program = Program(first);
        auto created = AnimationGraphInstance::Create(assets, first.context);
        REQUIRE(created.HasValue());
        auto instance = std::move(created).Value();
        const auto tick = Test::Id<AnimationTickId>(1);
        REQUIRE(instance.Evaluate({tick, {10}}).HasValue());
        const auto published = instance.Commit(tick);
        REQUIRE(published.HasValue());
        Test::Instance::Prepared replacement(Test::Graph::Simple(), 7);
        auto next = replacement.Assets();
        next.program = Program(replacement);
        REQUIRE(instance.Replace(next, first.ReplacementContext()).HasValue());
        std::array<Math::Transform, 1> pose;
        CHECK(instance.CopyPose(published.publication.currentPose, pose).failure == GraphInstanceFailure::HandleStale);
        REQUIRE(instance.Evaluate({tick, {5}}).HasValue());
        CHECK(instance.Commit(tick).HasValue());
    }

    TEST_CASE("Different direct clips cannot alias through their absent GraphId during replacement", "[animation][clip][scene]") {
        Test::Instance::Prepared first;
        auto assets = first.Assets();
        assets.program = Program(first);
        auto created = AnimationGraphInstance::Create(assets, first.context);
        REQUIRE(created.HasValue());
        auto instance = std::move(created).Value();
        Test::Actions::Prepared other;
        auto context = other.CompileContext();
        context.clips = std::span<const AnimationClipAsset>{other.compileClips}.subspan(1, 1);
        auto program = Detail::ClipSceneProgramAccess::Prepare(Test::Asset<AnimationClipId>(4), context);
        REQUIRE(program.HasValue());
        std::array otherClips{other.clips[1]};
        auto replacement = other.Assets();
        replacement.clips = otherClips;
        replacement.program = std::make_shared<const AnimationGraphProgram>(std::move(program).Value());
        REQUIRE_FALSE(replacement.program->Id().IsValid());
        const auto rejected = instance.Replace(replacement, first.ReplacementContext());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == AnimationErrors::GraphReloadMismatch.code.Value());
    }

    TEST_CASE("Direct clip preparation rejects incomplete stale or over-budget actual bindings", "[animation][clip][scene]") {
        Test::Instance::Prepared fixture;
        auto context = ClipContext(fixture);
        CancellationSource cancellation;
        SECTION("missing skeleton") {
            context.skeleton = nullptr;
        }
        SECTION("missing clip") {
            context.clips = {};
        }
        SECTION("missing skeleton generation") {
            context.skeletonGeneration = {};
        }
        SECTION("node budget") {
            context.limits.nodes = 1;
        }
        SECTION("pin budget") {
            context.limits.pins = 1;
        }
        SECTION("operand budget") {
            context.limits.connections = 0;
        }
        SECTION("graph replacement namespace") {
            context.replacing = fixture.program->Id();
        }
        SECTION("cancelled") {
            cancellation.RequestCancellation();
            context.cancellation = cancellation.Token();
        }
        CHECK(Detail::ClipSceneProgramAccess::Prepare(Test::Asset<AnimationClipId>(3), context).HasError());
    }

    TEST_CASE("A standalone additive clip cannot silently acquire a fabricated base pose", "[animation][clip][scene]") {
        Test::Instance::Prepared fixture;
        auto data = fixture.compileClips[0].Data();
        data.descriptor.kind = AnimationClipKind::Additive;
        data.descriptor.referencePose =
            AnimationReferencePoseBinding{Test::Id<AnimationReferencePoseId>(8), Test::Id<AnimationReferencePoseGeneration>(9)};
        auto actual = AnimationClipAsset::Create(std::move(data), *fixture.skeleton);
        REQUIRE(actual.HasValue());
        const std::array clips{std::move(actual).Value()};
        auto context = ClipContext(fixture);
        context.clips = clips;
        const auto rejected = Detail::ClipSceneProgramAccess::Prepare(Test::Asset<AnimationClipId>(3), context);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == AnimationErrors::ClipUnsupported.code.Value());
    }
}  // namespace Horo::Animation
