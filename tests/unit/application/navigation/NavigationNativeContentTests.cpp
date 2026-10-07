#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Application/NavigationContentIntegration.h"
#include "PublicationOperationId.h"
#include "navigation/NavMeshAssetTestFixtures.h"
#include "navigation/NavigationContentPolicyFixture.h"
#include "navigation/NavigationMixedContentFixture.h"
#include "navigation/NavigationReleaseFixture.h"
#include "navigation/NavigationReleaseSceneFixture.h"

#include <atomic>
#include <cstdlib>
#include <iomanip>
#include <thread>

namespace Horo::Application {
    using namespace ContentTestSupport;

    namespace {
        /** @brief Counter observes real native builder calls; immutable promoted packaging never invokes this owner. */
        class CountingBuilder final : public Navigation::INavigationMeshBuilder {
        public:
            explicit CountingBuilder(std::unique_ptr<Navigation::INavigationMeshBuilder> native) : native_(std::move(native)) {}

            Result<Navigation::NavigationTileBuildResult> BuildTile(const Navigation::NavigationTileBuildRequest &request,
                                                                    const CancellationToken &cancellation) const override {
                calls.fetch_add(1);
                return native_->BuildTile(request, cancellation);
            }

            mutable std::atomic<std::size_t> calls{};

        private:
            std::unique_ptr<Navigation::INavigationMeshBuilder> native_;
        };

        [[nodiscard]] AssetCookTargetId PackageTarget() {
            const auto parsed = AssetCookTargetId::Parse("headless-null");
            REQUIRE(parsed.HasValue());
            return parsed.Value();
        }

        [[nodiscard]] Navigation::NavigationProjectProfile Policy(const std::uint64_t identity = 17) {
            Navigation::NavigationProjectProfileInput
                input{.id = Navigation::TestSupport::Id<Navigation::NavigationProjectProfileId>(identity),
                      .revision = Navigation::TestSupport::Id<Navigation::NavigationProjectProfileRevision>(1),
                      .capacities = {64, 4, 8, 2, 1024U * 1024U, 8U * 1024U * 1024U, 2048},
                      .maximumQuery = {.query = Navigation::NavigationQueryKind::Path,
                                       .quality = Navigation::NavigationQualityLevel::Balanced,
                                       .limits = {32, 8, 100}}};
            input.capabilities.fill(Navigation::NavigationCapabilityRequirement::Optional);
            input.capabilities[static_cast<std::size_t>(Navigation::NavigationCapability::GroundedQueries)] =
                Navigation::NavigationCapabilityRequirement::Required;
            auto policy = Navigation::NavigationProjectProfile::Create(input);
            REQUIRE(policy.HasValue());
            return std::move(policy).Value();
        }

        /** @brief Actual native BakeService owns capture, scheduling, cache and durable promotion. */
        [[nodiscard]] Assets::AssetCookGeneration Bake(const std::filesystem::path &root, const std::shared_ptr<CountingBuilder> &builder) {
            Navigation::TestSupport::IncrementalBakeFixture input;
            OperationStore operations{8, 16};
            JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32}};
            NavigationBakeServiceConfig config{.definition = Navigation::AssetTestSupport::Asset(),
                                               .artifactType = Navigation::AssetTestSupport::Type(),
                                               .target = PackageTarget(),
                                               .cacheRoot = root / "cache",
                                               .targetRoot = root / "cook",
                                               .builder = builder,
                                               .files = std::make_shared<NativeDurableFileSystem>(),
                                               .budget = {1, 1024ULL * 1024ULL * 1024ULL, 128U * 1024U * 1024U, 8,
                                                          1024ULL * 1024ULL * 1024ULL, Duration::FromMilliseconds(2000)},
                                               .sourceAuthority = std::make_shared<NavigationBakeSourceAuthority>(),
                                               .newOperationId = Horo::TestSupport::NewPublicationOperationId};
            REQUIRE(config.sourceAuthority->UpdateCurrent(input.revisions, input.Observations()).HasValue());
            auto created = NavigationBakeService::Create(config, operations, jobs);
            REQUIRE(created.HasValue());
            auto service = std::move(created).Value();
            const auto submitted = service->Submit({.input = input.Input(),
                                                    .compatibility = input.compatibility,
                                                    .tiles = input.Tiles(),
                                                    .sources = input.Observations(),
                                                    .projectProfile = Policy()});
            INFO((submitted.HasError() ? submitted.ErrorValue().code.Value() : std::string_view{}));
            REQUIRE(submitted.HasValue());
            for (std::size_t iteration = 0; iteration < 5000 && !service->Published(); ++iteration) {
                service->Pump();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            REQUIRE(service->Published());
            REQUIRE(builder->calls.load() > 0);
            return service->Published()->generation;
        }

        [[nodiscard]] std::shared_ptr<CountingBuilder> Builder() {
            auto native = Navigation::CreateRecastDetourNavigationMeshBuilder();
            REQUIRE(native.HasValue());
            return std::make_shared<CountingBuilder>(std::move(native).Value());
        }

        /** @brief Explicit validation-job capture exports real promoted producer bytes as a fixed text fixture, never recooking. */
        void CapturePortableFixture(const Assets::AssetCookGeneration &generation) {
            const auto *outputPath = std::getenv("HORO_NAVIGATION_CONTENT_CAPTURE");
            if (!outputPath)
                return;
            const std::filesystem::path path{outputPath};
            REQUIRE(path.is_absolute());
            REQUIRE_FALSE(std::filesystem::exists(path));
            const auto contents = Assets::ReadCookGenerationContents(generation, 8U * 1024U * 1024U);
            REQUIRE(contents.HasValue());
            REQUIRE(contents.Value().artifacts.size() == 1);
            const auto &bytes = contents.Value().artifacts.front();
            const auto envelope = Assets::DecodeCookedArtifact(bytes);
            REQUIRE(envelope.HasValue());
            const auto set = Navigation::DecodeNavigationCookedTileSet(envelope.Value().payload, 8U * 1024U * 1024U);
            REQUIRE(set.HasValue());
            REQUIRE(set.Value().provenance.has_value());
            REQUIRE(set.Value().provenance->projectProfile.has_value());
            REQUIRE(set.Value().provenance->projectProfile->MatchesAuthority(Policy()));
            std::ofstream output{path};
            REQUIRE(output.is_open());
            output << "#pragma once\n#include <array>\n#include <cstdint>\n\n";
            output << "// Actual NavigationBakeService + native Recast producer; captured promoted HNS2, not a portable builder.\n";
            output << "// Envelope " << FormatSha256(ComputeSha256(std::as_bytes(std::span{bytes}))) << "\n";
            output << "// Source " << FormatSha256(set.Value().inputFingerprint) << "\n";
            output << "namespace Horo::Application::ContentTestSupport {\n";
            output << "inline constexpr std::array<std::uint8_t, " << bytes.size() << "> NativeHns2Envelope{\n";
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                if (index % 16 == 0)
                    output << "    ";
                output << "0x" << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned int>(bytes[index]) << ",";
                output << (index % 16 == 15 ? "\n" : " ");
            }
            output << "\n};\n}\n";
            output.close();
            REQUIRE_FALSE(output.fail());
        }

        /** @brief Actual aggregate Scene owner and real Detour provider consume verified ZIP content before safe publication. */
        void RequireNativeActivation(const AdmittedNavigationReleaseContent &content) {
            using namespace Navigation::AssetTestSupport;
            SceneHarness harness{content, PackageTarget(), Asset(), NativeFactory};
            auto *navigation = harness.navigation;
            auto definition = Definition(1, 1, {}, true, Navigation::TestSupport::Id<Navigation::SurfaceId>(1),
                                         Navigation::TestSupport::Id<Navigation::NavigationAgentProfileId>(1));
            REQUIRE_FALSE(harness.Activate(std::move(definition)).has_value());
            const auto lease = navigation->Acquire();
            REQUIRE(lease.HasValue());
            const auto path = lease.Value().Backend().FindPath(PathRequest(lease.Value()), lease.Value().Cancellation());
            REQUIRE(path.HasValue());
            REQUIRE_FALSE(path.Value().points.empty());
            REQUIRE(navigation->ActiveAssetProvenance().size() == 1);
            REQUIRE(content.expectations.size() == 1);
            REQUIRE(navigation->ActiveAssetProvenance().front().cookedContentDigest == content.expectations.front().cookedContentDigest);
            REQUIRE(harness.scenes.ActiveScene());
            const auto active = harness.scenes.ActiveScene()->RuntimeId();
            const auto missing = harness.Activate(Definition(2, 2));
            REQUIRE(missing.has_value());
            REQUIRE(missing->code.Value() == Navigation::NavigationErrors::StaleSnapshot.code.Value());
            REQUIRE(harness.scenes.ActiveScene()->RuntimeId() == active);
            REQUIRE_FALSE(lease.Value().IsRevoked());
            harness.scenes.Shutdown();
            REQUIRE(lease.Value().IsRevoked());
        }
    }  // namespace

    TEST_CASE("Native headless client and server packages preserve promoted HNS2 bytes and activate declared content",
              "[unit][navigation][content][native][package]") {
        Directory directory;
        const auto builder = Builder();
        const auto generation = Bake(directory.root, builder);
        const auto built = builder->calls.load();
        const auto audio = CookSharedAudio(directory.root / "audio", PackageTarget());
        const auto mixed = PublishMixed(directory.root / "mixed", generation, audio);
        CapturePortableFixture(generation);
        const std::array products{Release::DistributionProductKind::GameRuntime, Release::DistributionProductKind::GameDedicatedServer};
        for (std::size_t index = 0; index < products.size(); ++index) {
            const auto product = products[index];
            const auto omitted = PrepareNavigationReleaseContent(mixed, Plan(Navigation::AssetTestSupport::Asset(), product), product);
            REQUIRE(omitted.HasError());
            REQUIRE(omitted.ErrorValue().code.Value() == "asset.archive.invalid");
            const auto plan = MixedPlan(Navigation::AssetTestSupport::Asset(), product);
            Assets::AssetArchiveLimits restricted;
            restricted.maximumAssets = 1;
            const auto excluded = PrepareNavigationReleaseContent(mixed, plan, product, restricted);
            REQUIRE(excluded.HasError());
            REQUIRE(excluded.ErrorValue().code.Value() == "asset.cook.malformed_artifact");
            const auto prepared = PrepareNavigationReleaseContent(mixed, plan, product);
            INFO((prepared.HasError() ? prepared.ErrorValue().code.Value() : std::string_view{}));
            REQUIRE(prepared.HasValue());
            const auto repeated =
                PrepareNavigationReleaseContent(mixed, MixedPlan(Navigation::AssetTestSupport::Asset(), product), product);
            REQUIRE(repeated.HasValue());
            REQUIRE(repeated.Value().archive == prepared.Value().archive);
            REQUIRE(repeated.Value().extension == prepared.Value().extension);
            REQUIRE(prepared.Value().expectations.size() == 1);
            REQUIRE(prepared.Value().expectations.front().projectProfile.MatchesAuthority(Policy()));
            auto package = Produce(directory.root / std::to_string(index), prepared.Value(), product);
            const auto verified = Verify(package, PackageTarget(), product);
            auto admitted =
                AdmitNavigationReleaseContent(package.archive, package.manifest, verified, "assets.horo", PackageTarget(), product);
            INFO((admitted.HasError() ? admitted.ErrorValue().code.Value() : std::string_view{}));
            REQUIRE(admitted.HasValue());
            RequireSharedAudio(admitted.Value().provider, PackageTarget());
            RequireNativeActivation(admitted.Value());
            REQUIRE(builder->calls.load() == built);
        }
    }
}  // namespace Horo::Application
