#include "../packages/PackageArchiveTestSupport.h"
#include "Horo/Application/PrefabSceneCookHost.h"
#include "Horo/Assets/AssetArchive.h"
#include "Horo/Packages/PackageRequest.h"
#include "Horo/Prefab/CookedPrefab.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Release/ReleaseCandidatePublisher.h"
#include "Horo/Release/ReleaseCandidateVerification.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "Horo/Scene/SceneSource.h"
#include "PrefabSceneCookHostFixture.h"
#include "ReleaseTestFixtures.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Assets;
    using namespace Horo::Assets::ServiceTestSupport;
    using namespace Horo::Application::PrefabCookTestSupport;

    /** @brief Real release-executor composition whose Cook stage uses the concrete host and whose Package pins that output. */
    class HostReleaseStages final : public Release::IReleasePipelineStages {
    public:
        HostReleaseStages(HostFixture &fixture, const Release::ReleaseExecutionPlan &plan, Release::IReleasePreflightFactsProvider &facts)
            : fixture_(fixture), host_(fixture.MakeHost()), plan_(plan), facts_(facts) {}

        bool PackagedPinnedGeneration() const noexcept {
            return packagedPinnedGeneration_;
        }

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
            AssetChunkDefinition base{.id = AssetChunkId::Parse("base").Value(),
                                      .kind = AssetChunkKind::Base,
                                      .assets = {fixture_.sceneId, TestMeshRecord().id}};
            base.assets.insert(base.assets.end(), fixture_.request.runtimePrefabRoots.begin(), fixture_.request.runtimePrefabRoots.end());
            const auto chunks = AssetChunkPlan::Create(std::span{&base, 1});
            if (chunks.HasError())
                return Result<Release::ReleaseStagedPayload>::Failure(chunks.ErrorValue());
            const auto content = PreparePrefabReleaseContent(fixture_.files, cooked, fixture_.request.assets.target, chunks.Value(),
                                                             fixture_.request.runtimePrefabRoots);
            if (content.HasError())
                return Result<Release::ReleaseStagedPayload>::Failure(content.ErrorValue());
            packagedPinnedGeneration_ = !content.Value().archive.empty() && cooked.root != fixture_.cache.path;
            return Result<Release::ReleaseStagedPayload>::Success(
                {cooked.root, ComputeSha256(std::as_bytes(std::span{content.Value().archive}))});
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
            return packagedPinnedGeneration_ ? Result<void>::Success() : Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        }

        Result<void> Publish(const Release::ReleaseStageContext &, const Release::ReleaseFinalVerifiedCandidate &) override {
            return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        }

    private:
        static Sha256Digest Digest() {
            return ReleaseTestFixtures::Digest("test.host.release-stage");
        }

        bool packagedPinnedGeneration_{};
        HostFixture &fixture_;
        PrefabSceneCookHost host_;
        const Release::ReleaseExecutionPlan &plan_;
        Release::IReleasePreflightFactsProvider &facts_;
    };

}  // namespace

TEST_CASE("Host prefab cook exposes redacted preparation failures before AssetCook publication", "[native][prefab-cook][host]") {
    HostFixture fixture;
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    BuildOutputStore output{16};
    OperationStore operations{1, 4};
    fixture.request.assets.buildOutputStore = &output;
    fixture.request.assets.operationStore = &operations;
    fixture.WriteText("assets/hierarchy.prefab", "malformed prefab");

    const auto failed = fixture.Cook();
    REQUIRE(failed.HasError());
    const auto records = output.SnapshotIfChanged(0);
    REQUIRE(records);
    REQUIRE(records->records.size() == 1);
    const auto &finding = records->records.front();
    CHECK(finding.result == BuildOutputResult::Failed);
    CHECK(finding.code.Value() == failed.ErrorValue().code.Value());
    CHECK(finding.stage == "prefab.validate_sources");
    CHECK(finding.message.find(fixture.prefabId.ToString()) != std::string::npos);
    CHECK(finding.message.find("assets/hierarchy.prefab") != std::string::npos);
    CHECK(finding.message.find(fixture.project.dir.path.string()) == std::string::npos);
    CHECK(finding.message.find("malformed prefab") == std::string::npos);
    CHECK_FALSE(finding.source.has_value());
    CHECK(finding.sessionId.has_value());
    const auto progress = operations.SnapshotIfChanged(0);
    REQUIRE(progress);
    REQUIRE(progress->operations.size() == 1);
    CHECK(progress->operations.front().state == OperationState::Failed);
    CHECK(progress->operations.front().id == finding.operationId);
    fixture.AssertRetained(first.Value().generation);

    fixture.WritePrefab();
    REQUIRE(fixture.Cook().HasValue());
    const auto recovered = operations.SnapshotIfChanged(progress->revision);
    REQUIRE(recovered);
    REQUIRE(recovered->operations.size() == 2);
    CHECK(std::ranges::count(recovered->operations, OperationState::Succeeded, &OperationRecord::state) == 1);
}

TEST_CASE("Host prefab preparation and generation publication share one operation and output session", "[native][prefab-cook][host]") {
    HostFixture fixture;
    BuildOutputStore output{16};
    OperationStore operations{1, 4};
    fixture.request.assets.buildOutputStore = &output;
    fixture.request.assets.operationStore = &operations;

    struct Progress final {
        OperationId id{};
        float completion{};
        bool singleOperation{true};
        bool monotonic{true};
        bool began{};
        bool captured{};
        bool publishing{};
    } observed;

    fixture.request.assets.progress = {&observed, [](void *context, const OperationId id, const std::string_view phase,
                                                     const float completion) noexcept {
        auto &evidence = *static_cast<Progress *>(context);
        if (evidence.id == 0)
            evidence.id = id;
        evidence.singleOperation = evidence.singleOperation && evidence.id == id;
        evidence.monotonic = evidence.monotonic && evidence.completion <= completion;
        evidence.completion = completion;
        evidence.began |= phase == "prefab.preflight";
        evidence.captured |= phase == "prefab.validate_sources";
        evidence.publishing |= phase == "publish";
    }};
    std::optional<OperationId> initial;
    const auto nextPublication = fixture.request.assets.newPublicationOperationId;
    fixture.request.assets.newPublicationOperationId = [&] {
        const auto snapshot = operations.SnapshotIfChanged(0);
        REQUIRE(snapshot);
        REQUIRE(snapshot->operations.size() == 1);
        if (!initial) {
            CHECK(snapshot->operations.front().phase == "prefab.preflight");
            initial = snapshot->operations.front().id;
        }
        CHECK(snapshot->operations.front().id == initial);
        return nextPublication();
    };

    REQUIRE(fixture.Cook().HasValue());
    REQUIRE(initial);
    CHECK(observed.id == initial);
    CHECK(observed.singleOperation);
    CHECK(observed.monotonic);
    CHECK(observed.began);
    CHECK(observed.captured);
    CHECK(observed.publishing);
    const auto progress = operations.SnapshotIfChanged(0);
    REQUIRE(progress);
    REQUIRE(progress->operations.size() == 1);
    CHECK(progress->operations.front().id == *initial);
    CHECK(progress->operations.front().state == OperationState::Succeeded);
    CHECK(progress->operations.front().progress == 1.0F);
    const auto records = output.SnapshotIfChanged(0);
    REQUIRE(records);
    REQUIRE(records->records.size() > 1);
    const auto session = records->records.front().sessionId;
    REQUIRE(session);
    for (const auto &entry : records->records) {
        CHECK(entry.operationId == initial);
        CHECK(entry.sessionId == session);
    }
    CHECK(records->records.back().result == BuildOutputResult::Succeeded);
}

TEST_CASE("Host prefab cook handles an operation-store cancellation before source capture", "[native][prefab-cook][host]") {
    HostFixture fixture;
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    BuildOutputStore output{8};
    OperationStore operations{1, 2};
    CancellationSource cancellation;
    fixture.request.assets.buildOutputStore = &output;
    fixture.request.assets.operationStore = &operations;
    fixture.request.assets.requestCancel = [&] {
        cancellation.RequestCancellation();
    };
    const auto nextPublication = fixture.request.assets.newPublicationOperationId;
    fixture.request.assets.newPublicationOperationId = [&] {
        const auto snapshot = operations.SnapshotIfChanged(0);
        REQUIRE(snapshot);
        REQUIRE(snapshot->operations.size() == 1);
        CHECK(snapshot->operations.front().cancellable);
        REQUIRE(operations.RequestCancel(snapshot->operations.front().id));
        return nextPublication();
    };
    REQUIRE(fixture.Cook(cancellation.Token()).HasError());
    const auto snapshot = operations.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    REQUIRE(snapshot->operations.size() == 1);
    CHECK(snapshot->operations.front().state == OperationState::Cancelled);
    const auto records = output.SnapshotIfChanged(0);
    REQUIRE(records);
    REQUIRE(records->records.size() == 1);
    CHECK(records->records.front().result == BuildOutputResult::Cancelled);
    CHECK(records->records.front().operationId == snapshot->operations.front().id);
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Host prefab cook preserves typed failure when its cancellation races admission", "[native][prefab-cook][host]") {
    HostFixture fixture;
    BuildOutputStore output{8};
    OperationStore operations{1, 2};
    CancellationSource cancellation;
    fixture.request.assets.buildOutputStore = &output;
    fixture.request.assets.operationStore = &operations;
    fixture.request.assets.newPublicationOperationId = [&] {
        cancellation.RequestCancellation();
        return Result<AssetId>::Failure(MakeError(PrefabSceneCookErrors::Invalid, fixture.project.dir.path.string()));
    };

    const auto failed = fixture.Cook(cancellation.Token());
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().code.Value() == PrefabSceneCookErrors::Invalid.code.Value());
    const auto snapshot = operations.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    REQUIRE(snapshot->operations.size() == 1);
    CHECK(snapshot->operations.front().state == OperationState::Failed);
    REQUIRE(snapshot->operations.front().error);
    CHECK(snapshot->operations.front().error->message == "Prefab cook preparation failed");
    const auto records = output.SnapshotIfChanged(0);
    REQUIRE(records);
    REQUIRE(records->records.size() == 1);
    CHECK(records->records.front().result == BuildOutputResult::Failed);
    CHECK(records->records.front().message == "Prefab cook preparation failed");
}

TEST_CASE("Host prefab scene cook publishes expanded source-free scenes and validates cache reuse", "[native][prefab-cook][host]") {
    HostFixture fixture;
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    CHECK(first.Value().generation.artifactCount == 2);
    const auto inventory = ReadCookGenerationContents(Horo::NativeDurableFileSystem{}, first.Value().generation, 1024U * 1024U);
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
    CHECK(changed.Value().cacheHits == 1);
    CHECK(changed.Value().generation.manifestDigest != first.Value().generation.manifestDigest);
}

TEST_CASE("Persistent cook host closes detached source attempts before replacement and teardown", "[native][prefab-cook][host][jobs]") {
    HostFixture fixture;
    {
        auto host = fixture.MakeHost();
        const auto first = host.Cook(fixture.request);
        REQUIRE(first.HasValue());
        CHECK(fixture.jobs.AdmissionSnapshot().waitingProducers == 0);
        CHECK(fixture.jobs.AdmissionSnapshot().queued == std::array<std::size_t, 3>{});
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CHECK(host.Cook(fixture.request, cancelled.Token()).HasError());
        fixture.AssertRetained(first.Value().generation);
        fixture.WritePrefab(9);
        const auto replacement = host.Cook(fixture.request);
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value().generation.manifestDigest != first.Value().generation.manifestDigest);
        CHECK(fixture.jobs.AdmissionSnapshot().queued == std::array<std::size_t, 3>{});
    }
    CHECK(fixture.jobs.AdmissionSnapshot().waitingProducers == 0);
    CHECK(fixture.jobs.AdmissionSnapshot().queued == std::array<std::size_t, 3>{});
    // Teardown drains only this host's work, not the injected process scheduler.
    const auto remaining = fixture.jobs.Submit({}, [](const CancellationToken &) {
    });
    REQUIRE(remaining.HasValue());
    REQUIRE(remaining.Value().Wait({WaitPolicy::MainThreadPumpAllowed, Duration::FromMilliseconds(5'000)}).HasValue());
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

    /** @brief Captures a standard player release with distinct cook and final output roots. */
    HostReleaseInputs PreparePlayerRelease(const HostFixture &fixture, const std::filesystem::path &output) {
        using namespace Release;
        const ReleaseCapabilityId capability{"release.packaging"};
        ReleaseProfilePreset preset{.id = {"player"},
                                    .product = DistributionProductIdentity{DistributionProductKind::GameRuntime, {}},
                                    .artifactClass = DistributionArtifactClass::InstallableProduct,
                                    .platform = DistributionPlatform::Linux,
                                    .packageFormat = DistributionPackageFormat::TarGzip,
                                    .content = ReleaseContentPolicy{true, true, ReleaseAssetPolicy::SinglePackage, false, false},
                                    .symbols = ReleaseSymbolPolicy::Omit,
                                    .signing = ReleaseSigningPolicy::Disabled,
                                    .notarizationRequired = false,
                                    .includeLicensesAndNotices = true,
                                    .includeReleaseNotes = true,
                                    .updateEligible = false,
                                    .patchEligible = false,
                                    .eligibleDestinations = std::vector<ReleaseDestinationId>{},
                                    .requiredCapabilities = std::vector{capability}};
        const auto catalog = ReleaseProfileCatalog::Create({std::move(preset)});
        REQUIRE(catalog.HasValue());
        auto profile = catalog.Value().Resolve({"player"}, std::span{&capability, 1});
        REQUIRE(profile.HasValue());
        auto request = ReleaseTestFixtures::Request();
        request.projectId = "horo-player";
        request.version.productVersion = GameProductVersion{ParseReleaseVersion("0.4.2").Value()};
        request.profile = std::move(profile).Value();
        request.projectRoot = fixture.project.dir.path;
        request.outputRoot = output;
        auto facts = ReleaseTestFixtures::Facts(request);
        facts.releaseNotesSnapshot = ReleaseTestFixtures::Notes("0.4.2", request.projectId);
        facts.dependencyLockDigest = ComputeSha256(std::span<const std::byte>{});
        auto preflight = PreflightRelease(request, facts);
        REQUIRE(preflight.issues.empty());
        REQUIRE(preflight.plan);
        return {std::move(facts), std::move(preflight)};
    }

    constexpr std::string_view PlayerArchivePath{"assets.horo"};

    /** @brief Opens the actual final archive through the production player provider during release verification. */
    class PlayerArchiveSmoke final : public Release::IReleaseCandidateSmokeProbe {
    public:
        PlayerArchiveSmoke(const DurableFileSystem &files, const AssetCookTargetId &target) : files_(files), target_(target) {}

        Release::ReleaseCandidateSmokeKind Kind() const noexcept override {
            return Release::ReleaseCandidateSmokeKind::ArchiveReadable;
        }

        Result<void> Check(const std::filesystem::path &root, const Release::ReleaseArtifactManifest &manifest) override {
            const auto inventory = manifest.Artifacts();
            const auto archive = std::ranges::find(inventory, PlayerArchivePath, &Release::ReleaseArtifactRecord::path);
            if (archive == inventory.end() || archive->role != Release::ReleaseArtifactRole::AssetArchive)
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            const auto bytes = files_.ReadFileBounded(root / PlayerArchivePath, 1024U * 1024U);
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            const auto player = AssetArchiveProvider::Open(bytes.Value(), target_);
            return player.HasValue() ? Result<void>::Success() : Result<void>::Failure(player.ErrorValue());
        }

    private:
        const DurableFileSystem &files_;
        const AssetCookTargetId &target_;
    };

    /** @brief Reads an exact verified player envelope and verifies its commitment against the pinned cook manifest. */
    AssetCookArtifact RequireArchivedArtifact(const AssetArchiveProvider &player, std::span<const AssetCookManifestEntry> entries,
                                              const AssetId asset) {
        const auto expected = std::ranges::find(entries, asset, &AssetCookManifestEntry::assetId);
        REQUIRE(expected != entries.end());
        const auto bytes = player.Load(asset, {});
        REQUIRE(bytes.HasValue());
        CHECK(ComputeSha256(std::as_bytes(std::span{bytes.Value()})) == expected->artifactHash);
        auto artifact = DecodeCookedArtifact(bytes.Value());
        REQUIRE(artifact.HasValue());
        CHECK(artifact.Value().id == expected->assetId);
        CHECK(artifact.Value().type == expected->assetType);
        return std::move(artifact).Value();
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
    const auto active = ResolveCurrentCookGeneration(Horo::NativeDurableFileSystem{}, fixture.cooked.path);
    REQUIRE(active.HasValue());
    CHECK(handed.Value().root == active.Value().generationRoot);
    CHECK(handed.Value().bytesDigest == active.Value().manifestDigest);
    CHECK(handed.Value().root != fixture.cache.path);
    REQUIRE(ReadCookGenerationContents(Horo::NativeDurableFileSystem{}, active.Value(), 1024U * 1024U).HasValue());

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
    SECTION("static only retains the existing inventory") {
        // Keep the default empty runtime-root selection to exercise static-only publication.
    }
    const auto release = PrepareHostRelease(fixture);
    const auto &preflight = release.preflight;
    ReleaseTestFixtures::FixedReleaseFacts current{release.facts};
    HostReleaseStages stages{fixture, *preflight.plan, current};
    Release::ReleaseJobTracker tracker{{1}, {2}, 3, {false, false}};
    const auto result = Release::ReleasePipelineExecutor{}.Execute(tracker, {7}, *preflight.plan, current, stages, {});
    CHECK(result.state == Release::ReleaseJobState::Succeeded);
    CHECK(stages.PackagedPinnedGeneration());
    REQUIRE(result.candidate.has_value());
    CHECK(result.candidate->state == Release::ReleaseCandidateState::FinalVerified);
}

TEST_CASE("Player release verifies prefab archive digests and promotes only the staged bytes", "[native][prefab-cook][host][release]") {
    using namespace Release;
    HostFixture fixture;
    TempDir releaseRoot;
    const auto sourceOnly = fixture.AddSpawnableRoot();
    const auto dependency = TestMeshRecord().id;
    fixture.WritePrefab(4, {dependency});
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto release = PreparePlayerRelease(fixture, releaseRoot.path);
    const auto &plan = *release.preflight.plan;
    ReleaseTestFixtures::FixedReleaseFacts facts{release.facts};

    auto cookHost = fixture.MakeHost();
    const auto cooked = cookHost.CookForRelease(plan, facts, fixture.request, {});
    REQUIRE(cooked.HasValue());
    const auto frozen = ResolveCurrentCookGeneration(fixture.files, fixture.cooked.path);
    REQUIRE(frozen.HasValue());
    REQUIRE(frozen.Value().generationRoot == cooked.Value().root);
    REQUIRE(frozen.Value().manifestDigest == cooked.Value().bytesDigest);
    const auto inventory = ReadCookGenerationContents(fixture.files, frozen.Value(), 1024U * 1024U);
    REQUIRE(inventory.HasValue());

    const AssetChunkDefinition chunk{.id = AssetChunkId::Parse("base").Value(),
                                     .kind = AssetChunkKind::Base,
                                     .assets = {fixture.sceneId, fixture.prefabId, dependency}};
    const auto chunks = AssetChunkPlan::Create(std::span{&chunk, 1});
    REQUIRE(chunks.HasValue());
    auto incompleteChunk = chunk;
    std::erase(incompleteChunk.assets, fixture.prefabId);
    const auto incomplete = AssetChunkPlan::Create(std::span{&incompleteChunk, 1});
    REQUIRE(incomplete.HasValue());
    const auto prepare = [&](const AssetChunkPlan &selected, std::span<const AssetId> roots, const CancellationToken &cancellation = {}) {
        return PreparePrefabReleaseContent(fixture.files, cooked.Value(), frozen.Value().target, selected, roots, {}, cancellation);
    };
    CHECK(prepare(incomplete.Value(), fixture.request.runtimePrefabRoots).HasError());
    CHECK(prepare(chunks.Value(), {}).HasError());
    CancellationSource stop;
    stop.RequestCancellation();
    CHECK(prepare(chunks.Value(), fixture.request.runtimePrefabRoots, stop.Token()).HasError());

    const auto prepared = prepare(chunks.Value(), fixture.request.runtimePrefabRoots);
    REQUIRE(prepared.HasValue());
    const auto &archive = prepared.Value().archive;
    const auto player = AssetArchiveProvider::Open(archive, frozen.Value().target);
    REQUIRE(player.HasValue());
    REQUIRE(player.Value().Members().size() == chunk.assets.size());
    CHECK_FALSE(player.Value().Exists(sourceOnly, {}).Value());
    const auto sceneArtifact = RequireArchivedArtifact(player.Value(), inventory.Value().entries, fixture.sceneId);
    const auto templateArtifact = RequireArchivedArtifact(player.Value(), inventory.Value().entries, fixture.prefabId);
    const auto resource = RequireArchivedArtifact(player.Value(), inventory.Value().entries, dependency);
    const auto scene =
        SceneCook::DecodeCookedSceneWithEmbeddedRevision(sceneArtifact.payload, SceneCook::SceneDefinitionIdForAsset(fixture.sceneId));
    REQUIRE(scene.HasValue());
    CHECK(scene.Value().Entities().size() == 3);
    CHECK(std::ranges::any_of(scene.Value().Entities(), [](const auto &entity) {
        return entity.localTransform.translation.y == 4;
    }));
    const auto runtimeTemplate = Prefab::CookedPrefab::Parse(std::as_bytes(std::span{templateArtifact.payload}), fixture.prefabId,
                                                             Prefab::PrefabLimitProfile::Create({}).Value());
    REQUIRE(runtimeTemplate.HasValue());
    CHECK(runtimeTemplate.Value().GetObjectCount() == 2);
    REQUIRE(runtimeTemplate.Value().Data().dependencies.size() == 1);
    const auto resourceEntry = std::ranges::find(inventory.Value().entries, resource.id, &AssetCookManifestEntry::assetId);
    REQUIRE(resourceEntry != inventory.Value().entries.end());
    CHECK(runtimeTemplate.Value().Data().dependencies.front().artifactDigest == resourceEntry->artifactHash);

    NativeReleaseCandidatePublisher publisher{fixture.files};
    const auto stage = publisher.Begin(plan, ReleaseCandidateId{42});
    REQUIRE(stage.HasValue());
    const auto stagedArchive = stage.Value().StageRoot() / PlayerArchivePath;
    REQUIRE(fixture.files.WriteDurable(stagedArchive, std::as_bytes(std::span{archive})).HasValue());
    const ReleaseArtifactRecord content{std::string{PlayerArchivePath}, ReleaseArtifactRole::AssetArchive, archive.size(),
                                        ComputeSha256(std::as_bytes(std::span{archive}))};
    const auto stagedInventory = ReleasePreSignInventory::Create(stage.Value().Candidate(), {content});
    REQUIRE(stagedInventory.HasValue());
    REQUIRE(VerifyReleaseStagedTree(stage.Value().StageRoot(), stagedInventory.Value()).HasValue());

    const auto &request = plan.Request();
    const auto manifest = ReleaseArtifactManifest::Create({.candidate = stage.Value().Candidate(),
                                                           .product = request.profile.Product(),
                                                           .version = request.version.productVersion,
                                                           .sourceRevision = request.version.sourceRevision,
                                                           .platform = request.profile.Platform(),
                                                           .architecture = request.architecture,
                                                           .configuration = request.configuration,
                                                           .build = {"prefab_release"},
                                                           .toolchainId = request.toolchainId,
                                                           .frozen = plan.Identities(),
                                                           .artifacts = {content},
                                                           .extensions = {prepared.Value().extension}});
    REQUIRE(manifest.HasValue());
    REQUIRE(manifest.Value().Artifacts().size() == 1);
    CHECK(manifest.Value().Artifacts().front().digest == content.digest);
    REQUIRE(manifest.Value().Data().extensions.size() == 1);
    const auto &extension = manifest.Value().Data().extensions.front();
    CHECK(extension.name == "horo.prefab.content");
    const auto evidence = nlohmann::json::parse(extension.canonicalJson);
    CHECK(evidence.at("archive") == FormatSha256(content.digest));
    CHECK(evidence.at("cookManifest") == FormatSha256(cooked.Value().bytesDigest));
    REQUIRE(evidence.at("assets").size() == 2);
    for (const auto &entry : inventory.Value().entries) {
        if (entry.assetId == dependency)
            continue;
        const auto &records = evidence.at("assets");
        const auto record = std::ranges::find_if(records, [&](const auto &item) {
            return item.at("id") == entry.assetId.ToString();
        });
        REQUIRE(record != records.end());
        CHECK(record->at("type") == entry.assetType.Value());
        CHECK(record->at("envelope") == FormatSha256(entry.artifactHash));
    }

    SECTION("promotion consumes the verified bytes after the cook selector and source change") {
        fixture.WritePrefab(99, {dependency});
        const auto replacement = fixture.Cook();
        REQUIRE(replacement.HasValue());
        REQUIRE(replacement.Value().generation.manifestDigest != frozen.Value().manifestDigest);
        fixture.WriteText("assets/hierarchy.prefab", "source is intentionally unavailable for recook");
        const auto stillPinned = prepare(chunks.Value(), fixture.request.runtimePrefabRoots);
        REQUIRE(stillPinned.HasValue());
        CHECK(stillPinned.Value().archive == archive);
        CHECK(stillPinned.Value().extension == prepared.Value().extension);
        REQUIRE(publisher.Promote(plan, stage.Value(), manifest.Value()).HasValue());
        CHECK_FALSE(std::filesystem::exists(stage.Value().StageRoot()));
        PlayerArchiveSmoke smoke{fixture.files, frozen.Value().target};
        const std::array smokeKinds{smoke.Kind()};
        const std::array<IReleaseCandidateSmokeProbe *, 1> probes{&smoke};
        const auto verified = VerifyReleaseCandidate(stage.Value().FinalRoot(), manifest.Value(), smokeKinds, nullptr, probes);
        REQUIRE(verified.HasValue());
        CHECK(verified.Value().ManifestDigest() == manifest.Value().Digest());
        const auto promoted = fixture.files.ReadFileBounded(stage.Value().FinalRoot() / PlayerArchivePath, archive.size());
        REQUIRE(promoted.HasValue());
        CHECK(promoted.Value() == archive);
        REQUIRE(publisher.Promote(plan, stage.Value(), manifest.Value()).HasValue());
        fixture.AssertRetained(replacement.Value().generation);
    }
    SECTION("changed staged bytes are rejected even after pre-sign verification") {
        auto changed = archive;
        changed.back() ^= 1;
        REQUIRE(fixture.files.WriteDurable(stagedArchive, std::as_bytes(std::span{changed})).HasValue());
        CHECK(publisher.Promote(plan, stage.Value(), manifest.Value()).HasError());
        CHECK_FALSE(std::filesystem::exists(stage.Value().FinalRoot()));
        fixture.AssertRetained(frozen.Value());
    }
    SECTION("raw prefab sources and sidecars cannot enter a verified player stage") {
        WriteFile(stage.Value().StageRoot() / "hierarchy.prefab", std::array<std::uint8_t, 1>{1});
        WriteFile(stage.Value().StageRoot() / "hierarchy.prefab.horo", std::array<std::uint8_t, 1>{1});
        CHECK(VerifyReleaseStagedTree(stage.Value().StageRoot(), stagedInventory.Value()).HasError());
        CHECK(publisher.Promote(plan, stage.Value(), manifest.Value()).HasError());
        CHECK_FALSE(std::filesystem::exists(stage.Value().FinalRoot()));
        fixture.AssertRetained(frozen.Value());
    }
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
