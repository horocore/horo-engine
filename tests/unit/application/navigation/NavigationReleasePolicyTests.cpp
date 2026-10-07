#include "Horo/Application/NavigationContentIntegration.h"
#include "Horo/Navigation/NavigationAssetSceneActivation.h"
#include "navigation/NavigationContentPolicyFixture.h"
#include "navigation/NavigationReleaseFixture.h"
#include "navigation/NavigationReleaseSceneFixture.h"

#include <nlohmann/json.hpp>
#include <thread>

namespace Horo::Application {
    using namespace ContentTestSupport;

    namespace {
        [[nodiscard]] Assets::AssetId Asset() {
            auto id = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff");
            REQUIRE(id.HasValue());
            return id.Value();
        }

        [[nodiscard]] AssetCookTargetId Target(const std::string_view text = "headless-null") {
            auto target = AssetCookTargetId::Parse(text);
            REQUIRE(target.HasValue());
            return target.Value();
        }

        [[nodiscard]] PreparedNavigationReleaseContent Prepare(const std::filesystem::path &root, const AssetCookTargetId &target) {
            const auto set = Navigation::TestSupport::EmptyContent();
            auto generation = Publish(root, target, Asset(), Navigation::TestSupport::ContentEnvelope(set, Asset(), target));
            auto prepared = PrepareNavigationReleaseContent(generation, Plan(Asset(), Release::DistributionProductKind::GameRuntime),
                                                            Release::DistributionProductKind::GameRuntime);
            INFO((prepared.HasError() ? ErrorText(prepared.ErrorValue()) : std::string{}));
            REQUIRE(prepared.HasValue());
            return std::move(prepared).Value();
        }

        [[nodiscard]] Runtime::RuntimeSceneDefinition EmptyScene() {
            Runtime::SceneDefinitionBuilder builder{{4}, {1}};
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            return std::move(definition).Value();
        }

        /** @brief Test-owned signature boundary binds exact canonical bytes; this is no production key or cryptographic claim. */
        class RejectingSignature final : public Release::IReleaseCandidateSignatureVerifier {
        public:
            Result<void> Verify(const std::filesystem::path &, const Release::ReleaseArtifactManifest &) override {
                ++calls;
                return Result<void>::Failure(MakeError(Release::ReleaseErrors::PipelineOutputInvalid));
            }

            std::size_t calls{};
        };
    }  // namespace

    TEST_CASE("Required release rejects legacy and absent HNS2 project authority before output publication",
              "[unit][navigation][content][release][malformed]") {
        Directory directory;
        auto set = Navigation::TestSupport::EmptyContent();
        SECTION("explicit legacy HNS1") {
            set.provenance.reset();
        }
        SECTION("complete compatibility but omitted optional project authority") {
            set.provenance->projectProfile.reset();
        }
        const auto generation =
            Publish(directory.root / "cook", Target(), Asset(), Navigation::TestSupport::ContentEnvelope(set, Asset(), Target()));
        const auto prepared = PrepareNavigationReleaseContent(generation, Plan(Asset(), Release::DistributionProductKind::GameRuntime),
                                                              Release::DistributionProductKind::GameRuntime);
        Navigation::TestSupport::RequireError(prepared, Navigation::NavigationErrors::NavMeshArtifactCorrupt);
        REQUIRE_FALSE(std::filesystem::exists(directory.root / "stage"));
        REQUIRE_FALSE(std::filesystem::exists(directory.root / "output"));
        const auto current = Assets::ResolveCurrentCookGeneration(directory.root / "cook");
        REQUIRE(current.HasValue());
        REQUIRE(current.Value().manifestDigest == generation.manifestDigest);
    }

    TEST_CASE("Release archive limits intersect the cook generation owner ceilings without raising them",
              "[unit][navigation][content][release][limits]") {
        Directory directory;
        const auto set = Navigation::TestSupport::EmptyContent();
        const auto generation =
            Publish(directory.root / "cook", Target(), Asset(), Navigation::TestSupport::ContentEnvelope(set, Asset(), Target()));
        const Assets::AssetCookLimits legalCook;
        const Assets::AssetArchiveLimits release;
        REQUIRE(release.maximumAssets > legalCook.maximumAssets);
        REQUIRE(Assets::ReadCookGenerationContents(generation, release.maximumArchiveBytes, legalCook).HasValue());
        REQUIRE(PrepareNavigationReleaseContent(generation, Plan(Asset(), Release::DistributionProductKind::GameRuntime),
                                                Release::DistributionProductKind::GameRuntime, release)
                    .HasValue());
        auto tooMany = legalCook;
        ++tooMany.maximumAssets;
        const auto countFailure = Assets::ReadCookGenerationContents(generation, release.maximumArchiveBytes, tooMany);
        REQUIRE(countFailure.HasError());
        REQUIRE(countFailure.ErrorValue().domain.Value() == "horo.asset");
        REQUIRE(countFailure.ErrorValue().code.Value() == "asset.cook.too_large");
        auto tooLarge = legalCook;
        ++tooLarge.maximumArtifactBytes;
        const auto byteFailure = Assets::ReadCookGenerationContents(generation, release.maximumArchiveBytes, tooLarge);
        REQUIRE(byteFailure.HasError());
        REQUIRE(byteFailure.ErrorValue().domain.Value() == "horo.asset");
        REQUIRE(byteFailure.ErrorValue().code.Value() == "asset.cook.too_large");
        auto restricted = release;
        restricted.maximumAssets = 1;
        REQUIRE(PrepareNavigationReleaseContent(generation, Plan(Asset(), Release::DistributionProductKind::GameRuntime),
                                                Release::DistributionProductKind::GameRuntime, restricted)
                    .HasValue());
        restricted.maximumAssets = 0;
        const auto noAssets = PrepareNavigationReleaseContent(generation, Plan(Asset(), Release::DistributionProductKind::GameRuntime),
                                                              Release::DistributionProductKind::GameRuntime, restricted);
        REQUIRE(noAssets.HasError());
        REQUIRE(noAssets.ErrorValue().domain.Value() == "horo.asset");
        REQUIRE(noAssets.ErrorValue().code.Value() == "asset.cook.too_large");
        const auto current = Assets::ResolveCurrentCookGeneration(directory.root / "cook");
        REQUIRE(current.HasValue());
        REQUIRE(current.Value().manifestDigest == generation.manifestDigest);
        REQUIRE_FALSE(std::filesystem::exists(directory.root / "output"));
    }

    TEST_CASE("Exact canonical extension ceiling admits16384 bytes and rejects16385 without JSON padding",
              "[unit][navigation][content][release][bounds]") {
        Directory directory;
        const auto baseline = Prepare(directory.root / "baseline", Target());
        REQUIRE(baseline.extension.canonicalJson.size() < 16384);
        const auto exactLength = Target().Value().size() + 16384 - baseline.extension.canonicalJson.size();
        REQUIRE(exactLength > 9);
        const auto exactTarget = Target("headless-" + std::string(exactLength - 9, 'a'));
        const auto exact = Prepare(directory.root / "exact", exactTarget);
        REQUIRE(exact.extension.canonicalJson.size() == 16384);
        auto package = Produce(directory.root / "package", exact, Release::DistributionProductKind::GameRuntime);
        const auto proof = Verify(package, exactTarget, Release::DistributionProductKind::GameRuntime);
        REQUIRE(AdmitNavigationReleaseContent(package.archive, package.manifest, proof, "assets.horo", exactTarget,
                                              Release::DistributionProductKind::GameRuntime)
                    .HasValue());
        const auto oversizeTarget = Target(exactTarget.Value() + 'a');
        const auto set = Navigation::TestSupport::EmptyContent();
        const auto generation = Publish(directory.root / "oversize", oversizeTarget, Asset(),
                                        Navigation::TestSupport::ContentEnvelope(set, Asset(), oversizeTarget));
        Navigation::TestSupport::RequireError(PrepareNavigationReleaseContent(generation,
                                                                              Plan(Asset(), Release::DistributionProductKind::GameRuntime),
                                                                              Release::DistributionProductKind::GameRuntime),
                                              Navigation::NavigationErrors::CapacityExceeded);
        REQUIRE_FALSE(std::filesystem::exists(directory.root / "oversize-output"));
    }

    TEST_CASE("Runtime release admission requires exact generic verification proof and unmodified archive evidence",
              "[unit][navigation][content][release][proof]") {
        Directory directory;
        const auto prepared = Prepare(directory.root / "cook", Target());
        auto package = Produce(directory.root / "package", prepared, Release::DistributionProductKind::GameRuntime);
        const auto proof = Verify(package, Target(), Release::DistributionProductKind::GameRuntime);
        auto changed = package.manifest.Data();
        changed.build.value += "_changed";
        auto altered = Release::ReleaseArtifactManifest::Create(std::move(changed));
        REQUIRE(altered.HasValue());
        Navigation::TestSupport::RequireError(AdmitNavigationReleaseContent(package.archive, altered.Value(), proof, "assets.horo",
                                                                            Target(), Release::DistributionProductKind::GameRuntime),
                                              Navigation::NavigationErrors::StaleSnapshot);
        auto corrupt = package.archive;
        REQUIRE_FALSE(corrupt.empty());
        corrupt.back() ^= 1;
        Navigation::TestSupport::RequireError(AdmitNavigationReleaseContent(corrupt, package.manifest, proof, "assets.horo", Target(),
                                                                            Release::DistributionProductKind::GameRuntime),
                                              Navigation::NavigationErrors::NavMeshArtifactCorrupt);
        REQUIRE_FALSE(AdmitNavigationReleaseContent(package.archive, package.manifest, proof, "wrong.horo", Target(),
                                                    Release::DistributionProductKind::GameRuntime)
                          .HasValue());
    }

    TEST_CASE("Declared signing failure in the genuine generic verifier cannot issue a runtime candidate proof",
              "[unit][navigation][content][release][signature]") {
        Directory directory;
        const auto prepared = Prepare(directory.root / "cook", Target());
        auto package = Produce(directory.root / "package", prepared, Release::DistributionProductKind::GameRuntime);
        auto data = package.manifest.Data();
        data.signing = Release::ReleaseManifestSigning{"fixture_policy", "test_host", "test_key", package.manifest.Digest()};
        auto signedClaim = Release::ReleaseArtifactManifest::Create(std::move(data));
        REQUIRE(signedClaim.HasValue());
        const auto &json = signedClaim.Value().CanonicalJson();
        Write(package.stage / "manifest.json", {reinterpret_cast<const std::uint8_t *>(json.data()), json.size()});
        RejectingSignature signature;
        NavigationReleaseContentSmokeProbe smoke{"assets.horo", Target(), Release::DistributionProductKind::GameRuntime};
        Release::IReleaseCandidateSmokeProbe *probes[]{&smoke};
        const std::array required{Release::ReleaseCandidateSmokeKind::RuntimeAssets};
        auto verified = Release::VerifyReleaseCandidate(package.stage, signedClaim.Value(), required, &signature, probes);
        REQUIRE(verified.HasError());
        REQUIRE(signature.calls == 1);
    }

    TEST_CASE("Genuine release verification rejects omitted duplicate and unknown navigation evidence",
              "[unit][navigation][content][release][closure]") {
        Directory directory;
        auto prepared = Prepare(directory.root / "cook", Target());
        auto evidence = nlohmann::ordered_json::parse(prepared.extension.canonicalJson);
        const ErrorCodeDescriptor *expected = &Navigation::NavigationErrors::NavMeshArtifactCorrupt;
        SECTION("packaged navigation member omitted from exact evidence") {
            evidence["assets"] = nlohmann::ordered_json::array();
        }
        SECTION("duplicate navigation member evidence") {
            REQUIRE(evidence["assets"].size() == 1);
            evidence["assets"].push_back(evidence["assets"].front());
        }
        SECTION("unknown required extension version") {
            prepared.extension.version = 2;
            expected = &Navigation::NavigationErrors::UnsupportedCookedVersion;
        }
        SECTION("unknown extension cannot substitute for required evidence") {
            prepared.extension.name = "horo.navigation.unknown";
            expected = &Navigation::NavigationErrors::UnsupportedCookedVersion;
        }
        prepared.extension.canonicalJson = evidence.dump();
        auto package = Produce(directory.root / "package", prepared, Release::DistributionProductKind::GameRuntime);
        NavigationReleaseContentSmokeProbe smoke{"assets.horo", Target(), Release::DistributionProductKind::GameRuntime};
        Release::IReleaseCandidateSmokeProbe *probes[]{&smoke};
        const std::array required{Release::ReleaseCandidateSmokeKind::RuntimeAssets};
        const auto verified = Release::VerifyReleaseCandidate(package.stage, package.manifest, required, nullptr, probes);
        Navigation::TestSupport::RequireError(verified, *expected);
    }

    TEST_CASE("Genuine release verification rejects an unsupported payload inside an intact generic archive",
              "[unit][navigation][content][release][version]") {
        Directory directory;
        auto prepared = Prepare(directory.root / "cook", Target());
        const auto set = Navigation::TestSupport::EmptyContent();
        const auto encoded = Navigation::TestSupport::ContentEnvelope(set, Asset(), Target());
        auto envelope = Assets::DecodeCookedArtifact(encoded);
        REQUIRE(envelope.HasValue());
        auto artifact = std::move(envelope).Value();
        REQUIRE(artifact.payload.size() >= 4);
        artifact.payload[3] = '3';
        artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
        artifact.cacheKeyDigest = artifact.payloadDigest;
        const auto skewed = Assets::EncodeCookedArtifact(artifact);
        REQUIRE(skewed.HasValue());
        const std::array inputs{Assets::AssetArchiveInput{Asset(), skewed.Value()}};
        auto archive = Assets::BuildAssetArchive(Plan(Asset(), Release::DistributionProductKind::GameRuntime), Target(), inputs);
        REQUIRE(archive.HasValue());
        prepared.archive = std::move(archive).Value();
        auto evidence = nlohmann::ordered_json::parse(prepared.extension.canonicalJson);
        REQUIRE(evidence["assets"].size() == 1);
        evidence["archive"] = FormatSha256(ComputeSha256(std::as_bytes(std::span{prepared.archive})));
        evidence["assets"][0]["envelope"] = FormatSha256(ComputeSha256(std::as_bytes(std::span{skewed.Value()})));
        prepared.extension.canonicalJson = evidence.dump();
        auto package = Produce(directory.root / "package", prepared, Release::DistributionProductKind::GameRuntime);
        NavigationReleaseContentSmokeProbe smoke{"assets.horo", Target(), Release::DistributionProductKind::GameRuntime};
        Release::IReleaseCandidateSmokeProbe *probes[]{&smoke};
        const std::array required{Release::ReleaseCandidateSmokeKind::RuntimeAssets};
        const auto verified = Release::VerifyReleaseCandidate(package.stage, package.manifest, required, nullptr, probes);
        Navigation::TestSupport::RequireError(verified, Navigation::NavigationErrors::UnsupportedCookedVersion);
    }

    TEST_CASE("Strict packaged missing provider and absent required surfaces preserve an actual empty Scene",
              "[unit][navigation][content][release][preactivation]") {
        Directory directory;
        const auto prepared = Prepare(directory.root / "cook", Target());
        auto package = Produce(directory.root / "package", prepared, Release::DistributionProductKind::GameRuntime);
        const auto proof = Verify(package, Target(), Release::DistributionProductKind::GameRuntime);
        auto content = AdmitNavigationReleaseContent(package.archive, package.manifest, proof, "assets.horo", Target(),
                                                     Release::DistributionProductKind::GameRuntime);
        REQUIRE(content.HasValue());
        SceneHarness harness{content.Value(), Target(), Asset()};
        REQUIRE_FALSE(harness.Activate(EmptyScene()).has_value());
        REQUIRE(harness.scenes.ActiveScene());
        const auto active = harness.scenes.ActiveScene()->RuntimeId();
        const auto type = Assets::AssetTypeId::Parse(Assets::NavMeshAssetTypeName);
        REQUIRE(type.HasValue());
        Runtime::SceneDefinitionBuilder builder{{4}, {2}};
        Runtime::RuntimeEntityDefinition entity;
        entity.object = {101};
        const ErrorCodeDescriptor *expected = &Navigation::NavigationErrors::CapabilityUnavailable;
        SECTION("enabled surface and absent explicit factory") {
            entity.components.navigationSurface =
                Runtime::NavigationSurfaceComponent{.id = Navigation::TestSupport::Id<Navigation::SurfaceId>(1),
                                                    .definition = Asset(),
                                                    .profiles = {Navigation::TestSupport::Id<Navigation::NavigationAgentProfileId>(1)}};
        }
        SECTION("enabled agent has no eligible surface") {
            entity.components.navigationAgent =
                Runtime::NavigationAgentComponent{.profile = Navigation::TestSupport::Id<Navigation::NavigationAgentProfileId>(1),
                                                  .filter = Navigation::TestSupport::Id<Navigation::NavigationFilterId>(1)};
            expected = &Navigation::NavigationErrors::SceneSurfaceMissing;
        }
        SECTION("explicit required NavMesh cannot bypass its missing surface") {
            REQUIRE(builder.RequireAsset({Asset(), type.Value()}).HasValue());
            expected = &Navigation::NavigationErrors::SceneSurfaceMissing;
        }
        builder.Add(std::move(entity));
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        const auto failure = harness.Activate(std::move(definition).Value());
        REQUIRE(failure.has_value());
        INFO(ErrorText(*failure));
        REQUIRE(failure->domain.Value() == expected->domain.Value());
        REQUIRE(failure->code.Value() == expected->code.Value());
        REQUIRE(harness.scenes.ActiveScene()->RuntimeId() == active);
        harness.scenes.Shutdown();
    }
}  // namespace Horo::Application
