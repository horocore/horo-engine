#include "../packages/PackageArchiveTestSupport.h"
#include "Horo/Application/PrefabSceneCookHost.h"
#include "Horo/Packages/PackageRequest.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Scene/SceneSource.h"
#include "ReleaseTestFixtures.h"
#include "assets/AssetCookServiceFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Assets;
    using namespace Horo::Assets::ServiceTestSupport;

    /** @brief Injects an action at real staged-selector write, or fails its native replacement before commit. */
    class HostPublicationFiles final : public Horo::TestSupport::NativePublicationFiles {
    public:
        std::function<void()> staged;
        bool failReplacement{};
        bool observed{};

        Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            auto written = NativePublicationFiles::WriteDurable(path, bytes);
            if (written.HasValue() && path.filename() == "current.json" && !observed) {
                observed = true;
                if (staged)
                    staged();
            }
            return written;
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            if (failReplacement && destination.filename() == "current.json")
                return Result<void>::Failure(Error{ErrorCode{"test.host.publication_failed"}});
            return native.AtomicReplaceTracked(prepared, destination, receipt);
        }
    };

    /** @brief Owns real project transactions, sources, source registry and native generation publication. */
    struct HostFixture final {
        TestProject project;
        TempDir cache;
        TempDir cooked;
        NativeDurableFileSystem files;
        SystemWallClock clock;
        JobSystem jobs{JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 8}};
        AssetRegistry registry;
        const HoroVersion version = ParseHoroVersion("0.0.1").Value();
        RejectingCompatibilityProofVerifier verifier;
        ReleaseCompatibilityRegistry compatibility = Compatibility();
        ProjectCompatibilityInspector inspector{compatibility, {version}, verifier};
        Editor::ProjectMutationCoordinator mutations{files};
        Editor::ProjectMigrationTransactionService migrations{files, clock, mutations, jobs};
        PrefabSceneCookRequest request;
        const AssetId prefabId = Id("00000000-0000-0000-0000-0000000000b1");
        const AssetId sceneId = Id("00000000-0000-0000-0000-0000000000c1");

        HostFixture() {
            std::filesystem::create_directories(project.dir.path / ".horo/local");
            WriteText(
                ".horo/project.json",
                R"({"horoVersion":"0.0.1","persistentContract":"sha256:0101010101010101010101010101010101010101010101010101010101010101","projectId":"p1","name":"Test","projectVersion":"0.1.0","createdAt":"2026-07-18T00:00:00Z","settings":{"renderBackend":"opengl"}})");
            WritePrefab();
            const std::vector<SceneSource::SceneObjectSnapshot> objects{{.id = {900}, .name = "Authoring name"}};
            const std::vector<SceneSource::ScenePrefabInstance> placements{
                {.instanceId = Prefab::PrefabInstanceId::Create(7).Value(),
                 .sourcePrefab = Prefab::PrefabAssetReference::Create(prefabId).Value(),
                 .parent = SceneSource::SceneObjectId{900}}};
            WriteText("assets/level.scene", SceneSource::EncodeSceneSource({objects, placements}));
            WriteText("assets/hierarchy.prefab.horo", SidecarJson(prefabId.ToString(), "core.prefab"));
            WriteText("assets/level.scene.horo", SidecarJson(sceneId.ToString(), "core.scene"));
            PublishRegistry();
            request.assets.sourceRoot = project.dir.path;
            request.assets.cacheRoot = cache.path;
            request.assets.cookedRoot = cooked.path;
            request.assets.target = Target("headless-null");
            CookPublicationTestSupport::ConfigureNativeCookPublication(request.assets);
        }

        void WriteText(const std::string_view relative, const std::string_view text) const {
            WriteFile(project.dir.path / relative, {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
        }

        void WritePrefab(const float offset = 4) const {
            const auto limits = Prefab::PrefabLimitProfile::Create({});
            REQUIRE(limits.HasValue());
            auto document = Prefab::PrefabDocument::Create({.projectVersion = version,
                                                            .assetId = prefabId,
                                                            .objects = {{.localId = {0}, .name = "Prefab root"},
                                                                        {.localId = {3},
                                                                         .parentLocalId = Prefab::LocalObjectId{0},
                                                                         .localTransform = {.translation = {0, offset, 0}}}}},
                                                           limits.Value());
            REQUIRE(document.HasValue());
            auto canonical = document.Value().SerializeCanonical();
            REQUIRE(canonical.HasValue());
            WriteText("assets/hierarchy.prefab", canonical.Value());
        }

        void PublishRegistry() {
            REQUIRE(registry
                        .Publish({TestMeshRecord(), Record(prefabId, "core.prefab", "assets/hierarchy.prefab"),
                                  Record(sceneId, "core.scene", "assets/level.scene")})
                        .status == AssetRegistryBuildStatus::Complete);
        }

        PrefabSceneCookHost MakeHost() {
            CookerCatalog catalog;
            REQUIRE(catalog
                        .Register({.contributionId = "test.mesh",
                                   .assetType = Type("core.mesh"),
                                   .targets = {request.assets.target},
                                   .strategy = std::make_shared<const SettingsCooker>(1)})
                        .HasValue());
            auto sealed = catalog.Publish();
            REQUIRE(sealed.HasValue());
            return PrefabSceneCookHost{jobs, sealed.Value(), registry, inspector, mutations, migrations};
        }

        Result<AssetCookReport> Cook(const CancellationToken &token = {}) {
            return MakeHost().Cook(request, token);
        }

        void AssertRetained(const AssetCookGeneration &previous) const {
            const auto active = ResolveCurrentCookGeneration(cooked.path);
            REQUIRE(active.HasValue());
            CHECK(active.Value().manifestDigest == previous.manifestDigest);
            CHECK(active.Value().generationRoot == previous.generationRoot);
            CHECK(ReadCookGenerationContents(previous, 1024U * 1024U).HasValue());
        }

    private:
        ReleaseCompatibilityRegistry Compatibility() const {
            PersistentContractHash contract;
            contract.bytes.fill(1);
            CompatibilityDecisionHash decision;
            decision.bytes.fill(1);
            const std::array decisions{
                ReleaseCompatibilityDecision{{version}, {version}, contract, decision, CompatibilityDecisionKind::EstablishBaseline}};
            auto result = ReleaseCompatibilityRegistry::Create(decisions);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        static AssetRecord Record(const AssetId id, const std::string_view type, const std::string_view path) {
            return {.id = id,
                    .type = Type(type),
                    .sourcePath = ProjectPath::Parse(path).Value(),
                    .metadataPath = ProjectPath::Parse(std::string(path) + ".horo").Value()};
        }
    };

    /** @brief Real release-executor composition whose Cook stage uses the concrete host and whose Package pins that output. */
    class HostReleaseStages final : public Release::IReleasePipelineStages {
    public:
        HostReleaseStages(HostFixture &fixture, const Release::ReleaseExecutionPlan &plan, Release::IReleasePreflightFactsProvider &facts)
            : fixture_(fixture), host_(fixture.MakeHost()), plan_(plan), facts_(facts) {}

        bool packagedPinnedGeneration{};

        Result<void> Validate(const Release::ReleaseStageContext &) override {
            return Result<void>::Success();
        }

        Result<Release::ReleaseConfiguredTarget> Configure(const Release::ReleaseStageContext &) override {
            return Result<Release::ReleaseConfiguredTarget>::Success({fixture_.project.dir.path, Digest()});
        }

        Result<Release::ReleaseBuiltPayload> Build(const Release::ReleaseStageContext &,
                                                   const Release::ReleaseConfiguredTarget &) override {
            return Result<Release::ReleaseBuiltPayload>::Success({fixture_.project.dir.path, Digest()});
        }

        Result<Release::ReleaseCookedPayload> Cook(const Release::ReleaseStageContext &context, const Release::ReleaseConfiguredTarget &,
                                                   const Release::ReleaseBuiltPayload &) override {
            return host_.CookForRelease(plan_, facts_, fixture_.request, context.cancellation);
        }

        Result<Release::ReleaseStagedPayload> Package(const Release::ReleaseStageContext &, const Release::ReleaseBuiltPayload &,
                                                      const Release::ReleaseCookedPayload &cooked) override {
            const auto active = ResolveCurrentCookGeneration(fixture_.cooked.path);
            if (active.HasError() || active.Value().generationRoot != cooked.root || active.Value().manifestDigest != cooked.bytesDigest)
                return Result<Release::ReleaseStagedPayload>::Failure(MakeError(PrefabSceneCookErrors::Stale));
            const auto inventory = ReadCookGenerationContents(active.Value(), 1024U * 1024U);
            if (inventory.HasError())
                return Result<Release::ReleaseStagedPayload>::Failure(inventory.ErrorValue());
            packagedPinnedGeneration = inventory.Value().entries.size() == 2 && cooked.root != fixture_.cache.path;
            return Result<Release::ReleaseStagedPayload>::Success({cooked.root, cooked.bytesDigest});
        }

        Result<void> PreSignVerify(const Release::ReleaseStageContext &, const Release::ReleaseStagedPayload &) override {
            return Result<void>::Success();
        }

        Result<Release::ReleaseSignedPayload> Sign(const Release::ReleaseStageContext &,
                                                   const Release::ReleasePreSignVerifiedPayload &) override {
            return Result<Release::ReleaseSignedPayload>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        }

        Result<Sha256Digest> FinalizeMetadata(const Release::ReleaseStageContext &, Release::ReleaseCandidateId,
                                              const Release::ReleaseFinalBytes &) override {
            return Result<Sha256Digest>::Success(Digest());
        }

        Result<void> FinalVerify(const Release::ReleaseStageContext &, const Release::ReleaseFinalizedCandidate &) override {
            return packagedPinnedGeneration ? Result<void>::Success() : Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        }

        Result<void> Publish(const Release::ReleaseStageContext &, const Release::ReleaseFinalVerifiedCandidate &) override {
            return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        }

    private:
        static Sha256Digest Digest() {
            return ReleaseTestFixtures::Digest("test.host.release-stage");
        }

        HostFixture &fixture_;
        PrefabSceneCookHost host_;
        const Release::ReleaseExecutionPlan &plan_;
        Release::IReleasePreflightFactsProvider &facts_;
    };
}  // namespace

TEST_CASE("Host prefab scene cook publishes expanded source-free scenes and validates cache reuse", "[native][prefab-cook][host]") {
    HostFixture fixture;
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    CHECK(first.Value().generation.artifactCount == 2);
    const auto inventory = ReadCookGenerationContents(first.Value().generation, 1024U * 1024U);
    REQUIRE(inventory.HasValue());
    bool foundScene{};
    for (const auto &encoded : inventory.Value().artifacts) {
        const auto artifact = DecodeCookedArtifact(encoded);
        REQUIRE(artifact.HasValue());
        CHECK(artifact.Value().id != fixture.prefabId);
        if (artifact.Value().id == fixture.sceneId) {
            foundScene = true;
            const auto &payload = artifact.Value().payload;
            const std::string_view text{reinterpret_cast<const char *>(payload.data()), payload.size()};
            CHECK(text.find("prefabInstances") == std::string_view::npos);
            CHECK(text.find("sourcePrefab") == std::string_view::npos);
            CHECK(text.find("Authoring name") == std::string_view::npos);
            CHECK(text.find("assets/") == std::string_view::npos);
        }
    }
    CHECK(foundScene);
    const auto reused = fixture.Cook();
    REQUIRE(reused.HasValue());
    CHECK(reused.Value().cacheHits == 2);
    CHECK(reused.Value().generation.manifestDigest == first.Value().generation.manifestDigest);
    fixture.WritePrefab(9);
    const auto changed = fixture.Cook();
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().cacheHits == 0);
    CHECK(changed.Value().generation.manifestDigest != first.Value().generation.manifestDigest);
}

TEST_CASE("Host cook rejects malformed compatibility and prefab inputs without replacing the old generation",
          "[native][prefab-cook][host]") {
    HostFixture fixture;
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    SECTION("project corruption") {
        fixture.WriteText(".horo/project.json", "{");
    }
    SECTION("prefab corruption") {
        fixture.WriteText("assets/hierarchy.prefab", "{");
    }
    SECTION("foreign prefab sidecar") {
        fixture.WriteText("assets/hierarchy.prefab.horo", SidecarJson(fixture.sceneId.ToString(), "core.prefab"));
    }
    SECTION("expanded scene limit") {
        fixture.request.sceneLimits.maximumEntities = 2;
    }
    SECTION("missing restored package evidence") {
        fixture.WriteText(".horo/packages.lock", "{}");
    }
    REQUIRE(fixture.Cook().HasError());
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Host registry reload and cancellation fences preserve the prior generation", "[native][prefab-cook][host]") {
    HostFixture fixture;
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    CancellationSource cancellation;
    SECTION("cancelled before capture") {
        cancellation.RequestCancellation();
    }
    SECTION("registry reloaded during cook") {
        bool reloaded{};
        fixture.request.assets.validateHostInputs = [&fixture, &reloaded] {
            if (!reloaded) {
                fixture.PublishRegistry();
                reloaded = true;
            }
            return Result<void>::Success();
        };
        REQUIRE(fixture.Cook(cancellation.Token()).HasError());
        CHECK(reloaded);
        fixture.AssertRetained(first.Value().generation);
        return;
    }
    REQUIRE(fixture.Cook(cancellation.Token()).HasError());
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Host retains project mutation authority and checks final selector failures", "[native][prefab-cook][host]") {
    HostFixture fixture;
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    fixture.WritePrefab(8);
    auto files = std::make_shared<HostPublicationFiles>();
    CancellationSource cancellation;
    bool competingMutationRejected{};
    fixture.request.assets.validateHostInputs = [&fixture, &competingMutationRejected] {
        auto competing = fixture.mutations.TryAcquire({fixture.project.dir.path, Editor::ProjectMutationOwner::Asset, "test-competing"});
        competingMutationRejected = competing.HasError();
        return Result<void>::Success();
    };
    SECTION("cancellation after staging") {
        files->staged = [&cancellation] {
            cancellation.RequestCancellation();
        };
    }
    SECTION("project drift after staging") {
        files->staged = [&fixture] {
            fixture.WriteText(".horo/project.json", "{");
        };
    }
    SECTION("selector replacement failure") {
        files->failReplacement = true;
    }
    fixture.request.assets.publicationFiles = files;
    REQUIRE(fixture.Cook(cancellation.Token()).HasError());
    CHECK(competingMutationRejected);
    CHECK(files->observed);
    fixture.AssertRetained(first.Value().generation);
}

namespace {
    /** @brief Keeps the exact preflight plan and its observed input facts together for both real release consumers. */
    struct HostReleaseInputs final {
        Release::ReleasePreflightFacts facts;
        Release::ReleasePreflightOutcome preflight;
    };

    /** @brief Admits real project roots and empty dependency-lock identity once, preserving the matching facts for later checks. */
    HostReleaseInputs PrepareHostRelease(const HostFixture &fixture) {
        auto request = ReleaseTestFixtures::Request();
        request.projectRoot = fixture.project.dir.path;
        request.outputRoot = fixture.cooked.path;
        auto facts = ReleaseTestFixtures::Facts(request);
        facts.dependencyLockDigest = ComputeSha256(std::span<const std::byte>{});
        auto preflight = Release::PreflightRelease(request, facts);
        REQUIRE(preflight.plan.has_value());
        return {std::move(facts), std::move(preflight)};
    }
}  // namespace

TEST_CASE("Release cook handoff pins the actual generation and rejects changed frozen inputs", "[native][prefab-cook][host][release]") {
    HostFixture fixture;
    const auto release = PrepareHostRelease(fixture);
    const auto &preflight = release.preflight;
    ReleaseTestFixtures::FixedReleaseFacts current{release.facts};
    auto host = fixture.MakeHost();
    const auto handed = host.CookForRelease(*preflight.plan, current, fixture.request, {});
    REQUIRE(handed.HasValue());
    const auto active = ResolveCurrentCookGeneration(fixture.cooked.path);
    REQUIRE(active.HasValue());
    CHECK(handed.Value().root == active.Value().generationRoot);
    CHECK(handed.Value().bytesDigest == active.Value().manifestDigest);
    CHECK(handed.Value().root != fixture.cache.path);
    REQUIRE(ReadCookGenerationContents(active.Value(), 1024U * 1024U).HasValue());

    SECTION("host observations changed") {
        current.driftAt = current.captures + 1;
    }
    SECTION("actual package lock changed despite stable supplied observations") {
        fixture.WriteText(".horo/packages.lock", "{}");
    }
    REQUIRE(host.CookForRelease(*preflight.plan, current, fixture.request, {}).HasError());
    fixture.AssertRetained(active.Value());
}

TEST_CASE("Release executor packages the concrete static prefab cook generation", "[native][prefab-cook][host][release]") {
    HostFixture fixture;
    const auto release = PrepareHostRelease(fixture);
    const auto &preflight = release.preflight;
    ReleaseTestFixtures::FixedReleaseFacts current{release.facts};
    HostReleaseStages stages{fixture, *preflight.plan, current};
    Release::ReleaseJobTracker tracker{{1}, {2}, 3, {false, false}};
    const auto result = Release::ReleasePipelineExecutor{}.Execute(tracker, {7}, *preflight.plan, current, stages, {});
    CHECK(result.state == Release::ReleaseJobState::Succeeded);
    CHECK(stages.packagedPinnedGeneration);
    REQUIRE(result.candidate.has_value());
    CHECK(result.candidate->state == Release::ReleaseCandidateState::FinalVerified);
}

namespace {
    constexpr std::string_view packageIntentBytes =
        R"({"sources":{"horo.public":{"kind":"public-registry","registry":"official"}},"dependencies":{"com.horo.assets":{"source":"horo.public","version":"1.0.0"}}})";

    /** @brief Installs current canonical intent/lock plus independently verified immutable archive evidence for host capture. */
    std::shared_ptr<Packages::PackageRestoreGraph> InstallVerifiedPackage(HostFixture &fixture) {
        const auto intent = Packages::ValidatedPackageRequest::Parse(packageIntentBytes);
        REQUIRE(intent.HasValue());
        auto archive = Packages::ValidatedPackageArchive::Verify(Horo::Tests::Packages::ValidPackageArchiveBytes());
        REQUIRE(archive.HasValue());
        const auto package = Packages::HoroPackageId::Parse("com.horo.assets").Value();
        const auto version = Packages::PackageVersion::Parse("1.0.0").Value();
        const auto source = Packages::HoroPackageSourceId::Parse("horo.public").Value();
        const Packages::PackageResolutionPlan plan{{{package, version, source, archive.Value().Digest(), {}}}};
        const Packages::PackageLockArtifact evidence{package,
                                                     version,
                                                     source,
                                                     archive.Value().Digest(),
                                                     archive.Value().PackageManifestDigest(),
                                                     archive.Value().Manifest().Digest(),
                                                     1,
                                                     {},
                                                     {"assets"}};
        const auto lock = Packages::ValidatedPackageLockfileV1::Generate(plan, std::span{&package, 1U}, intent.Value().Digest(),
                                                                         std::span{&evidence, 1U});
        REQUIRE(lock.HasValue());
        auto graph = std::make_shared<Packages::PackageRestoreGraph>();
        graph->requestHash = intent.Value().Digest();
        graph->platform = {"linux", "x64", "horo-sdk-2"};
        graph->packages.push_back({lock.Value().Packages().front(),
                                   std::make_shared<const Packages::ValidatedPackageArchive>(std::move(archive).Value()),
                                   false,
                                   {}});
        fixture.request.restoredPackages = graph;
        fixture.WriteText(".horo/packages.json", packageIntentBytes);
        fixture.WriteText(".horo/packages.lock", lock.Value().SerializeCanonical());
        return graph;
    }
}  // namespace

TEST_CASE("Host package capture binds current intent lock and verified archive", "[native][prefab-cook][host][packages]") {
    HostFixture fixture;
    auto graph = InstallVerifiedPackage(fixture);
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    SECTION("intent changed") {
        auto changed = nlohmann::json::parse(packageIntentBytes);
        changed["dependencies"]["com.horo.assets"]["version"] = "2.0.0";
        fixture.WriteText(".horo/packages.json", changed.dump());
    }
    SECTION("archive unavailable") {
        graph->packages.front().archive.reset();
    }
    SECTION("restored source changed") {
        graph->packages.front().lock.source = Packages::HoroPackageSourceId::Parse("other.source").Value();
    }
    SECTION("intent changed at selector barrier") {
        auto files = std::make_shared<HostPublicationFiles>();
        files->staged = [&fixture] {
            fixture.WriteText(".horo/packages.json", "{}");
        };
        fixture.request.assets.publicationFiles = files;
        fixture.WritePrefab(8);
    }
    REQUIRE(fixture.Cook().HasError());
    fixture.AssertRetained(first.Value().generation);
}
