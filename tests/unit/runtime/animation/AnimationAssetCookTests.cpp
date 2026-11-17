#include "animation/AnimationCookFixture.h"

namespace Horo::Animation {
    namespace {
        std::size_t Index(const Assets::AssetCookGenerationContents &contents, Assets::AssetId id) {
            const auto found = std::ranges::find(contents.entries, id, &Assets::AssetCookManifestEntry::assetId);
            REQUIRE(found != contents.entries.end());
            return static_cast<std::size_t>(found - contents.entries.begin());
        }

        AnimationClipAsset ChangedClip(const Test::Cook::Fixture &fixture) {
            auto data = fixture.native.compileClips.front().Data();
            data.tracks.front().keys.back().transform.translation.x += 100;
            return Test::Cook::Take(AnimationClipAsset::Create(std::move(data), *fixture.native.skeleton));
        }
    }  // namespace

    TEST_CASE("Actual AST prepares Animation from real source capture and verifies miss then hit complete emitted HCAF set",
              "[animation][cook][native][cache]") {
        Test::Cook::Fixture fixture;
        CHECK(fixture.prepared->UsesCapture(*fixture.request.pinnedInputs));
        auto first = Test::Cook::Take(fixture.Cook());
        CHECK(first.cookedAssets == 3);
        CHECK(first.cacheHits == 0);
        const auto initial = fixture.Contents(first);
        const auto views = Test::Cook::Fixture::Views(initial);
        REQUIRE(fixture.prepared->ValidateEmitted(views, {}).HasValue());
        const std::array roots{
            Assets::AssetDependency{fixture.native.program->Id().Asset(), Assets::CookTestValues::Type("core.animation.graph")}};
        const auto closure = Test::Cook::Take(fixture.prepared->ExpandDependencies(roots, views, 3, {}));
        REQUIRE(closure.size() == 3);
        CHECK(std::ranges::find(closure, fixture.native.skeleton->Data().skeleton.Asset(), &Assets::AssetDependency::id) != closure.end());
        CHECK(std::ranges::find(closure, fixture.native.compileClips.front().Data().descriptor.id.Asset(), &Assets::AssetDependency::id) !=
              closure.end());
        CHECK(fixture.prepared->ExpandDependencies(roots, views, 2, {}).HasError());
        const auto second = Test::Cook::Take(fixture.Cook());
        CHECK(second.cookedAssets == 0);
        CHECK(second.cacheHits == 3);
        CHECK(second.generation.manifestDigest == first.generation.manifestDigest);
        const auto cached = fixture.Contents(second);
        REQUIRE(fixture.prepared->ValidateEmitted(Test::Cook::Fixture::Views(cached), {}).HasValue());
        CHECK(cached.artifacts == initial.artifacts);
    }

    TEST_CASE("Actual AST source drift preserves old publication and fresh capture invalidates affected dependency keys",
              "[animation][cook][native][stale]") {
        Test::Cook::Fixture fixture;
        const auto first = Test::Cook::Take(fixture.Cook());
        const auto oldCapture = fixture.request.pinnedInputs;
        const auto oldPrepared = fixture.prepared;
        const auto changed = ChangedClip(fixture);
        Test::Cook::Write(fixture.source.path / "assets/clip.json", Test::Cook::Take(SerializeClipAnimationSource(changed)));
        REQUIRE(fixture.Cook().HasError());
        const auto retained =
            Test::Cook::Take(Assets::ResolveCurrentCookGeneration(*fixture.request.publicationFiles, fixture.cooked.path));
        CHECK(retained.manifestDigest == first.generation.manifestDigest);
        fixture.Capture();
        CHECK_FALSE(oldPrepared->UsesCapture(*fixture.request.pinnedInputs));
        CHECK(fixture.prepared->UsesCapture(*fixture.request.pinnedInputs));
        const auto updated = Test::Cook::Take(fixture.Cook());
        CHECK(updated.cookedAssets == 2);
        CHECK(updated.cacheHits == 1);  // Skeleton stays an actual source-local cache hit; Graph binds changed actual Clip content.
        CHECK(updated.generation.manifestDigest != first.generation.manifestDigest);
        const auto contents = fixture.Contents(updated);
        REQUIRE(fixture.prepared->ValidateEmitted(Test::Cook::Fixture::Views(contents), {}).HasValue());
        CHECK(oldPrepared->ValidateEmitted(Test::Cook::Fixture::Views(contents), {}).HasError());
        CHECK(oldCapture->VerifyUnchanged().HasError());
    }

    TEST_CASE("Valid but different actual emitted payload cannot satisfy canonical prepared Animation source or prefab closure",
              "[animation][cook][native][integrity]") {
        Test::Cook::Fixture fixture;
        const auto first = Test::Cook::Take(fixture.Cook());
        auto contents = fixture.Contents(first);
        const auto skeletonIndex = Index(contents, fixture.native.skeleton->Data().skeleton.Asset());
        const auto clipId = fixture.native.compileClips.front().Data().descriptor.id;
        const auto clipIndex = Index(contents, clipId.Asset());
        const auto skeleton = Test::Cook::Take(Assets::DecodeCookedArtifact(contents.artifacts[skeletonIndex]));
        auto clip = Test::Cook::Take(Assets::DecodeCookedArtifact(contents.artifacts[clipIndex]));
        const auto changed = ChangedClip(fixture);
        clip.payload = Test::Cook::Take(EncodeClipAnimationPayload(changed, skeleton.payload)).bytes;
        clip.payloadDigest = ComputeSha256(std::as_bytes(std::span{clip.payload}));
        contents.artifacts[clipIndex] = Test::Cook::Take(Assets::EncodeCookedArtifact(clip));
        contents.entries[clipIndex].artifactHash = ComputeSha256(std::as_bytes(std::span{contents.artifacts[clipIndex]}));
        REQUIRE(Assets::DecodeCookedArtifact(contents.artifacts[clipIndex]).HasValue());
        REQUIRE(DecodeClipAnimationPayload(clip.payload, clipId).HasValue());
        const auto *source = fixture.request.pinnedInputs->Find(clipId.Asset());
        Assets::CookSourceView view{source->record.id,    source->record.type, fixture.request.target,
                                    source->sourceDigest, source->bytes,       source->record.sourcePath.String()};
        REQUIRE(fixture.catalog->Find(source->record.type, fixture.request.target));
        CHECK(fixture.catalog->Find(source->record.type, fixture.request.target)->ValidateCookedPayload(view, clip.payload).HasError());
        const auto emitted = Test::Cook::Fixture::Views(contents);
        CHECK(fixture.prepared->ValidateEmitted(emitted, {}).HasError());
        const std::array roots{
            Assets::AssetDependency{fixture.native.program->Id().Asset(), Assets::CookTestValues::Type("core.animation.graph")}};
        CHECK(fixture.prepared->ExpandDependencies(roots, emitted, 3, {}).HasError());
        CHECK(
            Test::Cook::Take(Assets::ResolveCurrentCookGeneration(*fixture.request.publicationFiles, fixture.cooked.path)).manifestDigest ==
            first.generation.manifestDigest);
    }

    TEST_CASE("Animation prepared emitted inventory rejects missing duplicate and wrong target", "[animation][cook][native][inventory]") {
        Test::Cook::Fixture fixture;
        const auto report = Test::Cook::Take(fixture.Cook());
        auto contents = fixture.Contents(report);
        auto views = Test::Cook::Fixture::Views(contents);
        SECTION("missing actual emitted asset") {
            views.pop_back();
        }
        SECTION("duplicate actual emitted asset") {
            views.push_back(views.front());
        }
        SECTION("incorrect envelope target") {
            auto artifact = Test::Cook::Take(Assets::DecodeCookedArtifact(contents.artifacts.front()));
            artifact.target = Test::Cook::Take(AssetCookTargetId::Parse("foreign-runtime"));
            contents.artifacts.front() = Test::Cook::Take(Assets::EncodeCookedArtifact(artifact));
            contents.entries.front().artifactHash = ComputeSha256(std::as_bytes(std::span{contents.artifacts.front()}));
            views = Test::Cook::Fixture::Views(contents);
        }
        CHECK(fixture.prepared->ValidateEmitted(views, {}).HasError());
    }
}  // namespace Horo::Animation

namespace Horo::Animation {
    TEST_CASE("Actual Animation role projection deduplicates transitive roots but rejects type conflicts and lower capacity",
              "[animation][cook][native][closure]") {
        Test::Cook::Fixture fixture;
        const auto report = Test::Cook::Take(fixture.Cook());
        const auto contents = fixture.Contents(report);
        const auto views = Test::Cook::Fixture::Views(contents);
        const auto graph = fixture.native.program->Id().Asset();
        const auto graphType = Test::Cook::Take(Assets::AssetTypeId::Parse("core.animation.graph"));
        std::vector<Assets::AssetDependency> roots{{graph, graphType}, {graph, graphType}};
        std::size_t maximum = 3;
        bool rejected{};
        SECTION("complete actual transitive roles deduplicate repeated root") {}
        SECTION("wrong exact root type") {
            roots.back().expectedType = Test::Cook::Take(Assets::AssetTypeId::Parse("core.animation.clip"));
            rejected = true;
        }
        SECTION("actual closure exceeds lower host distinct budget") {
            maximum = 2;
            rejected = true;
        }
        const auto projected = fixture.prepared->ExpandDependencies(roots, views, maximum, {});
        if (rejected) {
            REQUIRE(projected.HasError());
        } else {
            REQUIRE(projected.HasValue());
            REQUIRE(projected.Value().size() == 3);
            CHECK(std::ranges::find(projected.Value(), fixture.native.compileClips.front().Data().descriptor.id.Asset(),
                                    &Assets::AssetDependency::id) != projected.Value().end());
        }
    }
}  // namespace Horo::Animation
