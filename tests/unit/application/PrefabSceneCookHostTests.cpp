#include "../packages/PackageArchiveTestSupport.h"
#include "Horo/Application/PrefabSceneCookHost.h"
#include "Horo/Packages/PackageRequest.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Prefab/PrefabTemplateProvider.h"
#include "Horo/Scene/SceneSource.h"
#include "ReleaseTestFixtures.h"
#include "assets/AssetCookServiceFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

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
        std::size_t selectorWrites{};

        Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            auto written = native.WriteDurable(path, bytes);
            if (written.HasValue() && path.filename() == "current.json")
                ++selectorWrites;
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

        void WritePrefab(const float offset = 4, std::vector<AssetId> resources = {}) const {
            const auto limits = Prefab::PrefabLimitProfile::Create({});
            REQUIRE(limits.HasValue());
            auto document = Prefab::PrefabDocument::Create({.projectVersion = version,
                                                            .assetId = prefabId,
                                                            .objects = {{.localId = {0}, .name = "Prefab root"},
                                                                        {.localId = {3},
                                                                         .parentLocalId = Prefab::LocalObjectId{0},
                                                                         .localTransform = {.translation = {0, offset, 0}}}},
                                                            .referencedAssets = std::move(resources)},
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

        PrefabSceneCookHost MakeHost(const std::uint8_t resourceSetting = 1) {
            CookerCatalog catalog;
            REQUIRE(catalog
                        .Register({.contributionId = "test.mesh",
                                   .assetType = Type("core.mesh"),
                                   .targets = {request.assets.target},
                                   .strategy = std::make_shared<const SettingsCooker>(resourceSetting)})
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

        AssetId AddSpawnableRoot() {
            const auto id = Id("00000000-0000-0000-0000-0000000000b2");
            auto document = Prefab::PrefabDocument::Create({.projectVersion = version,
                                                            .assetId = id,
                                                            .objects = {{.localId = {0}, .name = "Second root"}}},
                                                           Prefab::PrefabLimitProfile::Create({}).Value());
            REQUIRE(document.HasValue());
            WriteText("assets/second.prefab", document.Value().SerializeCanonical().Value());
            WriteText("assets/second.prefab.horo", SidecarJson(id.ToString(), "core.prefab"));
            REQUIRE(registry
                        .Publish({TestMeshRecord(), Record(prefabId, "core.prefab", "assets/hierarchy.prefab"),
                                  Record(sceneId, "core.scene", "assets/level.scene"), Record(id, "core.prefab", "assets/second.prefab")})
                        .status == AssetRegistryBuildStatus::Complete);
            return id;
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
            packagedPinnedGeneration =
                inventory.Value().entries.size() == 2 + fixture_.request.runtimePrefabRoots.size() && cooked.root != fixture_.cache.path;
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

    /** @brief Reads actual published envelope bytes without treating a cache directory as authority. */
    Result<AssetCookArtifact> GenerationArtifact(const AssetCookGeneration &generation, const AssetId id) {
        auto inventory = ReadCookGenerationContents(generation, 1024U * 1024U);
        if (inventory.HasError())
            return Result<AssetCookArtifact>::Failure(inventory.ErrorValue());
        for (const auto &bytes : inventory.Value().artifacts) {
            auto artifact = DecodeCookedArtifact(bytes);
            if (artifact.HasError())
                return artifact;
            if (artifact.Value().id == id)
                return artifact;
        }
        return Result<AssetCookArtifact>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
    }

    /** @brief Produces another structurally valid template while retaining the original requested source/cache identity. */
    void ReplaceTemplatePayload(AssetCookArtifact &artifact) {
        const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
        auto decoded = Prefab::CookedPrefab::Parse(std::as_bytes(std::span{artifact.payload}), artifact.id, limits);
        REQUIRE(decoded.HasValue());
        auto data = decoded.Value().Data();
        data.entities[1].localTransform.translation.y = 99;
        auto replacement = Prefab::CookedPrefab::Create(std::move(data), limits);
        REQUIRE(replacement.HasValue());
        artifact.payload.clear();
        for (const auto byte : replacement.Value().Bytes())
            artifact.payload.push_back(std::to_integer<std::uint8_t>(byte));
    }

    /** @brief Prepares the host-produced template through the existing generation and asynchronous provider path. */
    void AssertProviderTemplate(HostFixture &fixture, const AssetCookGeneration &generation, const Sha256Digest &digest) {
        FilesystemAssetProvider bytes{generation.generationRoot};
        AssetLoadService loads{fixture.jobs, bytes};
        Runtime::RuntimeSceneService scenes{fixture.registry, loads};
        REQUIRE(scenes.Startup({}).HasValue());
        Runtime::SceneDefinitionBuilder builder{{7}, {1}};
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        REQUIRE(scenes.QueuePreparation(std::move(definition).Value()).HasValue());
        REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, {1, {}, 0.0, 0, {}, false, {}}).HasValue());
        Prefab::PrefabTemplateProvider provider{fixture.registry, loads, scenes, Prefab::PrefabLimitProfile::Create({}).Value()};
        REQUIRE(provider.Startup({}).HasValue());
        auto loading = provider.LoadAsync({fixture.prefabId, digest, fixture.request.assets.target});
        REQUIRE(loading.HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (loading.Value().State() == Prefab::PrefabTemplateLoadState::Loading && std::chrono::steady_clock::now() < deadline) {
            REQUIRE(provider.Advance().HasValue());
            std::this_thread::yield();
        }
        auto lease = provider.TakeResult(loading.Value());
        REQUIRE(lease.HasValue());
        CHECK(lease.Value().Template()->GetObjectCount() == 2);
        CHECK(lease.Value().Dependencies().size() == 1);
        provider.Shutdown();
        CHECK(lease.Value().Template()->Data().entities[1].localTransform.translation.y == 4);
    }
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
    CHECK(files->selectorWrites == 1);
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Release cook handoff pins the actual generation and rejects changed frozen inputs", "[native][prefab-cook][host][release]") {
    HostFixture fixture;
    auto request = ReleaseTestFixtures::Request();
    request.projectRoot = fixture.project.dir.path;
    request.outputRoot = fixture.cooked.path;
    auto facts = ReleaseTestFixtures::Facts(request);
    facts.dependencyLockDigest = ComputeSha256(std::span<const std::byte>{});
    const auto preflight = Release::PreflightRelease(request, facts);
    REQUIRE(preflight.plan.has_value());
    ReleaseTestFixtures::FixedReleaseFacts current{facts};
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
    SECTION("explicit dynamic template shares the release handoff") {
        fixture.request.runtimePrefabRoots = {fixture.prefabId};
    }
    SECTION("static only retains the existing inventory") {}
    auto request = ReleaseTestFixtures::Request();
    request.projectRoot = fixture.project.dir.path;
    request.outputRoot = fixture.cooked.path;
    auto facts = ReleaseTestFixtures::Facts(request);
    facts.dependencyLockDigest = ComputeSha256(std::span<const std::byte>{});
    const auto preflight = Release::PreflightRelease(request, facts);
    REQUIRE(preflight.plan.has_value());
    ReleaseTestFixtures::FixedReleaseFacts current{facts};
    HostReleaseStages stages{fixture, *preflight.plan, current};
    Release::ReleaseJobTracker tracker{{1}, {2}, 3, {false, false}};
    const auto result = Release::ReleasePipelineExecutor{}.Execute(tracker, {7}, *preflight.plan, current, stages, {});
    CHECK(result.state == Release::ReleaseJobState::Succeeded);
    CHECK(stages.packagedPinnedGeneration);
    REQUIRE(result.candidate.has_value());
    CHECK(result.candidate->state == Release::ReleaseCandidateState::FinalVerified);
}

TEST_CASE("Host publishes dynamic templates resources and expanded scenes once and loads the generation provider",
          "[native][prefab-cook][host][template]") {
    HostFixture fixture;
    fixture.WritePrefab(4, {TestMeshRecord().id});
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    auto files = std::make_shared<HostPublicationFiles>();
    fixture.request.assets.publicationFiles = files;
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    CHECK(first.Value().generation.artifactCount == 3);
    CHECK(files->observed);
    CHECK(files->selectorWrites == 1);
    const auto artifact = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(artifact.HasValue());
    auto cooked = Prefab::CookedPrefab::Parse(std::as_bytes(std::span{artifact.Value().payload}), fixture.prefabId,
                                              Prefab::PrefabLimitProfile::Create({}).Value());
    REQUIRE(cooked.HasValue());
    REQUIRE(cooked.Value().Data().dependencies.size() == 1);
    const auto inventory = ReadCookGenerationContents(first.Value().generation, 1024U * 1024U);
    REQUIRE(inventory.HasValue());
    const auto resource = std::ranges::find(inventory.Value().entries, TestMeshRecord().id, &AssetCookManifestEntry::assetId);
    REQUIRE(resource != inventory.Value().entries.end());
    CHECK(cooked.Value().Data().dependencies[0].artifactDigest == resource->artifactHash);
    const auto root = std::ranges::find(inventory.Value().entries, fixture.prefabId, &AssetCookManifestEntry::assetId);
    REQUIRE(root != inventory.Value().entries.end());
    AssertProviderTemplate(fixture, first.Value().generation, root->artifactHash);
    const auto repeated = fixture.Cook();
    REQUIRE(repeated.HasValue());
    CHECK(repeated.Value().cacheHits == 3);
    CHECK(repeated.Value().generation.manifestDigest == first.Value().generation.manifestDigest);
}

TEST_CASE("Dynamic root selection is canonical and excludes unselected templates under the same registry revision",
          "[native][prefab-cook][host][template]") {
    HostFixture fixture;
    const auto other = fixture.AddSpawnableRoot();
    const auto revision = fixture.registry.Snapshot().Revision();
    fixture.request.runtimePrefabRoots = {other, fixture.prefabId};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    CHECK(first.Value().generation.artifactCount == 4);
    std::ranges::reverse(fixture.request.runtimePrefabRoots);
    const auto reordered = fixture.Cook();
    REQUIRE(reordered.HasValue());
    CHECK(reordered.Value().cacheHits == 4);
    CHECK(reordered.Value().generation.manifestDigest == first.Value().generation.manifestDigest);
    fixture.request.runtimePrefabRoots = {other};
    const auto selected = fixture.Cook();
    REQUIRE(selected.HasValue());
    CHECK(selected.Value().generation.artifactCount == 3);
    CHECK(GenerationArtifact(selected.Value().generation, fixture.prefabId).HasError());
    CHECK(GenerationArtifact(selected.Value().generation, other).HasValue());
    CHECK(fixture.registry.Snapshot().Revision() == revision);
}

TEST_CASE("Invalid dynamic root selection preserves the current complete generation", "[native][prefab-cook][host][template]") {
    HostFixture fixture;
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    SECTION("duplicate") {
        fixture.request.runtimePrefabRoots.push_back(fixture.prefabId);
    }
    SECTION("missing") {
        fixture.request.runtimePrefabRoots = {Id("00000000-0000-0000-0000-000000000099")};
    }
    SECTION("wrong type") {
        fixture.request.runtimePrefabRoots = {TestMeshRecord().id};
    }
    SECTION("too many roots") {
        fixture.request.runtimePrefabRoots.assign(fixture.request.assets.limits.maximumAssets + 1, fixture.prefabId);
    }
    REQUIRE(fixture.Cook().HasError());
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Dynamic template cache binds actual changed resource envelopes without a registry replacement",
          "[native][prefab-cook][host][template]") {
    HostFixture fixture;
    fixture.WritePrefab(4, {TestMeshRecord().id});
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto revision = fixture.registry.Snapshot().Revision();
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    const auto oldArtifact = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(oldArtifact.HasValue());
    WriteFile(fixture.project.sourceFile, std::array<std::uint8_t, 3>{7, 8, 9});
    const auto changed = fixture.Cook();
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().cacheHits == 0);
    const auto newArtifact = GenerationArtifact(changed.Value().generation, fixture.prefabId);
    REQUIRE(newArtifact.HasValue());
    CHECK(oldArtifact.Value().payloadDigest != newArtifact.Value().payloadDigest);
    CHECK(oldArtifact.Value().sourceDigest == newArtifact.Value().sourceDigest);
    CHECK(oldArtifact.Value().cacheKeyDigest != newArtifact.Value().cacheKeyDigest);
    CHECK(fixture.registry.Snapshot().Revision() == revision);
}

TEST_CASE("Dynamic template keys bind changed resource cooker settings with identical source and registry inputs",
          "[native][prefab-cook][host][template][cache]") {
    HostFixture fixture;
    fixture.WritePrefab(4, {TestMeshRecord().id});
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto revision = fixture.registry.Snapshot().Revision();
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    const auto oldArtifact = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(oldArtifact.HasValue());
    const auto changed = fixture.MakeHost(2).Cook(fixture.request);
    REQUIRE(changed.HasValue());
    const auto newArtifact = GenerationArtifact(changed.Value().generation, fixture.prefabId);
    REQUIRE(newArtifact.HasValue());
    CHECK(oldArtifact.Value().sourceDigest == newArtifact.Value().sourceDigest);
    CHECK(oldArtifact.Value().cacheKeyDigest != newArtifact.Value().cacheKeyDigest);
    CHECK(oldArtifact.Value().payloadDigest != newArtifact.Value().payloadDigest);
    CHECK(fixture.registry.Snapshot().Revision() == revision);
}

TEST_CASE("Dynamic candidate failures retain the prior generation at the common selector fence", "[native][prefab-cook][host][template]") {
    HostFixture fixture;
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    fixture.WritePrefab(8);
    auto files = std::make_shared<HostPublicationFiles>();
    CancellationSource cancelled;
    SECTION("cancelled after candidate staging") {
        files->staged = [&cancelled] {
            cancelled.RequestCancellation();
        };
    }
    SECTION("source drift after staging") {
        files->staged = [&fixture] {
            fixture.WritePrefab(9);
        };
    }
    SECTION("selector replacement failure") {
        files->failReplacement = true;
    }
    fixture.request.assets.publicationFiles = files;
    REQUIRE(fixture.Cook(cancelled.Token()).HasError());
    CHECK(files->observed);
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Dynamic cache admission rejects corrupt and valid but different HPFB payloads under the requested key",
          "[native][prefab-cook][host][template][cache]") {
    HostFixture fixture;
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    auto original = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(original.HasValue());
    auto poisoned = std::move(original).Value();
    SECTION("HPFB integrity differs despite a valid outer envelope") {
        poisoned.payload.back() ^= 1U;
    }
    SECTION("another valid template is not the expected output") {
        ReplaceTemplatePayload(poisoned);
    }
    poisoned.payloadDigest = ComputeSha256(std::as_bytes(std::span{poisoned.payload}));
    const auto envelope = EncodeCookedArtifact(poisoned);
    REQUIRE(envelope.HasValue());
    TempDir poisonedCache;
    AssetCookCache cache{poisonedCache.path};
    REQUIRE(cache.Store({poisoned.cacheKeyDigest}, envelope.Value(), {}).HasValue());
    fixture.request.assets.cacheRoot = poisonedCache.path;
    REQUIRE(fixture.Cook().HasError());
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Dynamic host captures required portable schemas and rejects their absence without replacing the generation",
          "[native][prefab-cook][host][template][schema]") {
    HostFixture fixture;
    const auto type = Gameplay::ComponentTypeId::Parse("game.tests.data").Value();
    Gameplay::ComponentRegistry components;
    REQUIRE(components.Register({.typeId = type, .schemaVersion = 1, .displayName = "Data", .category = "Tests"}).HasValue());
    REQUIRE(components.Freeze().HasValue());
    auto schemas = PrefabCookSchemaContext::Capture(components, {});
    REQUIRE(schemas.HasValue());
    fixture.request.schemas = schemas.Value();
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    Prefab::PrefabObjectNode root{.localId = {0}, .name = "Portable root"};
    root.components.push_back({.instance = Prefab::PrefabComponentInstanceId::Create(1).Value(),
                               .component = {.typeId = type, .schemaVersion = 1, .payload = {std::byte{'{'}, std::byte{'}'}}}});
    auto document = Prefab::PrefabDocument::Create({.projectVersion = fixture.version, .assetId = fixture.prefabId, .objects = {root}},
                                                   Prefab::PrefabLimitProfile::Create({}).Value());
    REQUIRE(document.HasValue());
    fixture.WriteText("assets/hierarchy.prefab", document.Value().SerializeCanonical().Value());
    fixture.WriteText("assets/level.scene", SceneSource::EncodeSceneSource({{}, {}}));
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    const auto artifact = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(artifact.HasValue());
    const auto cooked = Prefab::CookedPrefab::Parse(std::as_bytes(std::span{artifact.Value().payload}), fixture.prefabId,
                                                    Prefab::PrefabLimitProfile::Create({}).Value());
    REQUIRE(cooked.HasValue());
    CHECK(std::get<Prefab::RawComponentPayload>(cooked.Value().Data().entities[0].members[0]).component.typeId == type);
    fixture.request.schemas.reset();
    REQUIRE(fixture.Cook().HasError());
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Dynamic template payload ceilings accept the exact encoded boundary and reject the next byte",
          "[native][prefab-cook][host][template][limits]") {
    HostFixture fixture;
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    const auto artifact = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(artifact.HasValue());
    fixture.request.prefabPolicy.maximumCookedPayloadBytes = artifact.Value().payload.size() - Prefab::CookedPrefabHeaderBytes;
    const auto exact = fixture.Cook();
    REQUIRE(exact.HasValue());
    --fixture.request.prefabPolicy.maximumCookedPayloadBytes;
    REQUIRE(fixture.Cook().HasError());
    fixture.AssertRetained(exact.Value().generation);
}

TEST_CASE("Host package capture binds current intent lock and verified archive", "[native][prefab-cook][host][packages]") {
    HostFixture fixture;
    const std::string bytes =
        R"({"sources":{"horo.public":{"kind":"public-registry","registry":"official"}},"dependencies":{"com.horo.assets":{"source":"horo.public","version":"1.0.0"}}})";
    const auto intent = Packages::ValidatedPackageRequest::Parse(bytes);
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
    const auto lock =
        Packages::ValidatedPackageLockfileV1::Generate(plan, std::span{&package, 1U}, intent.Value().Digest(), std::span{&evidence, 1U});
    REQUIRE(lock.HasValue());
    auto graph = std::make_shared<Packages::PackageRestoreGraph>();
    graph->requestHash = intent.Value().Digest();
    graph->platform = {"linux", "x64", "horo-sdk-2"};
    graph->packages.push_back({lock.Value().Packages().front(),
                               std::make_shared<const Packages::ValidatedPackageArchive>(std::move(archive).Value()),
                               false,
                               {}});
    fixture.request.restoredPackages = graph;
    fixture.WriteText(".horo/packages.json", bytes);
    fixture.WriteText(".horo/packages.lock", lock.Value().SerializeCanonical());
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    SECTION("intent changed") {
        auto changed = nlohmann::json::parse(bytes);
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
