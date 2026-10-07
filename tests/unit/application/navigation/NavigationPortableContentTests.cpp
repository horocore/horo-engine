#include "Horo/Application/NavigationContentIntegration.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "navigation/NativeNavigationContentCorpus.h"
#include "navigation/NavMeshAssetTestFixtures.h"
#include "navigation/NavigationMixedContentFixture.h"
#include "navigation/NavigationReleaseSceneFixture.h"

namespace Horo::Application {
    using namespace ContentTestSupport;

    namespace {
        [[nodiscard]] AssetCookTargetId Target() {
            const auto parsed = AssetCookTargetId::Parse("headless-null");
            REQUIRE(parsed.HasValue());
            return parsed.Value();
        }

        /** @brief Independently hash the actual captured native envelope before any portable publication or decoding. */
        [[nodiscard]] Assets::AssetCookGeneration PromoteCorpus(const std::filesystem::path &root) {
            REQUIRE(FormatSha256(ComputeSha256(std::as_bytes(std::span{NativeHns2Envelope}))) ==
                    "sha256:de7fc3706baaa700ca2e3d0a721b972ae59bfcbc6ac169b5af41448d6e0bf1aa");
            const auto envelope = Assets::DecodeCookedArtifact(NativeHns2Envelope);
            REQUIRE(envelope.HasValue());
            const auto decoded = Navigation::DecodeNavigationCookedTileSet(envelope.Value().payload, 8U * 1024U * 1024U);
            REQUIRE(decoded.HasValue());
            REQUIRE(decoded.Value().provenance.has_value());
            REQUIRE(decoded.Value().provenance->projectProfile.has_value());
            REQUIRE_FALSE(decoded.Value().tiles.empty());
            REQUIRE(FormatSha256(decoded.Value().inputFingerprint) ==
                    "sha256:393e8c1026cf1132ff1ec802f636c38051f165dec12fff24e6094735a1c39ba9");
            REQUIRE(envelope.Value().sourceDigest == decoded.Value().inputFingerprint);
            return Publish(root, Target(), Navigation::AssetTestSupport::Asset(), {NativeHns2Envelope.begin(), NativeHns2Envelope.end()});
        }

        /** @brief Portable Scene keeps an actual last-good empty world when packaged navigation lacks its required provider. */
        void RequireUnavailableActivation(const AdmittedNavigationReleaseContent &content) {
            SceneHarness harness{content, Target(), Navigation::AssetTestSupport::Asset()};
            Runtime::SceneDefinitionBuilder empty{{1}, {1}};
            auto oldWorld = std::move(empty).Build();
            REQUIRE(oldWorld.HasValue());
            REQUIRE_FALSE(harness.Activate(std::move(oldWorld).Value()).has_value());
            REQUIRE(harness.scenes.ActiveScene());
            const auto active = harness.scenes.ActiveScene()->RuntimeId();
            auto definition =
                Navigation::AssetTestSupport::Definition(2, 2, {}, true, Navigation::TestSupport::Id<Navigation::SurfaceId>(1),
                                                         Navigation::TestSupport::Id<Navigation::NavigationAgentProfileId>(1));
            const auto error = harness.Activate(std::move(definition));
            REQUIRE(error.has_value());
            INFO(ErrorText(*error));
            REQUIRE(error->domain.Value() == Navigation::NavigationErrors::CapabilityUnavailable.domain.Value());
            REQUIRE(error->code.Value() == Navigation::NavigationErrors::CapabilityUnavailable.code.Value());
            REQUIRE(harness.scenes.ActiveScene()->RuntimeId() == active);
            REQUIRE(harness.navigation->ActiveAssetProvenance().empty());
        }
    }  // namespace

    TEST_CASE("Portable client and server packages retain genuine native corpus and shared content without rebuilding navigation",
              "[unit][navigation][content][portable][package]") {
        Directory directory;
        const auto generation = PromoteCorpus(directory.root / "promoted");
        const auto audio = CookSharedAudio(directory.root / "audio", Target());
        const auto mixed = PublishMixed(directory.root / "mixed", generation, audio);
        const std::array products{Release::DistributionProductKind::GameRuntime, Release::DistributionProductKind::GameDedicatedServer};
        for (std::size_t index = 0; index < products.size(); ++index) {
            const auto product = products[index];
            const auto plan = MixedPlan(Navigation::AssetTestSupport::Asset(), product);
            const auto prepared = PrepareNavigationReleaseContent(mixed, plan, product);
            INFO((prepared.HasError() ? ErrorText(prepared.ErrorValue()) : std::string{}));
            REQUIRE(prepared.HasValue());
            const auto repeated = PrepareNavigationReleaseContent(mixed, plan, product);
            REQUIRE(repeated.HasValue());
            REQUIRE(repeated.Value().archive == prepared.Value().archive);
            REQUIRE(repeated.Value().extension == prepared.Value().extension);
            auto package = Produce(directory.root / std::to_string(index), prepared.Value(), product);
            const auto proof = Verify(package, Target(), product);
            auto admitted = AdmitNavigationReleaseContent(package.archive, package.manifest, proof, "assets.horo", Target(), product);
            REQUIRE(admitted.HasValue());
            REQUIRE(admitted.Value().provider.Members().size() == 2);
            RequireSharedAudio(admitted.Value().provider, Target());
            RequireUnavailableActivation(admitted.Value());
        }
    }

    TEST_CASE("Portable composition explicitly rejects the omitted native mesh builder without a silent substitute",
              "[unit][navigation][content][portable][capability]") {
        const auto builder = Navigation::CreateRecastDetourNavigationMeshBuilder();
        REQUIRE(builder.HasError());
        REQUIRE(builder.ErrorValue().domain.Value() == Navigation::NavigationErrors::OperationUnsupported.domain.Value());
        REQUIRE(builder.ErrorValue().code.Value() == Navigation::NavigationErrors::OperationUnsupported.code.Value());
    }
}  // namespace Horo::Application
