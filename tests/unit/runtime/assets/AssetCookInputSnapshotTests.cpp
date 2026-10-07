#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "assets/AssetCookServiceFixture.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;
    using namespace Horo::Assets::ServiceTestSupport;

    /** @brief Captures the real source and sidecar through the same validated registry used by AssetCook. */
    AssetCookInputSnapshot CaptureProject(const TestProject &project) {
        AssetRegistry registry;
        REQUIRE(registry.Publish({TestMeshRecord()}).status == AssetRegistryBuildStatus::Complete);
        auto captured = AssetCookInputSnapshot::Capture(project.dir.path, registry.Snapshot(), {}, 4096);
        REQUIRE(captured.HasValue());
        return std::move(captured).Value();
    }

    /** @brief Writes changed test metadata without introducing a second identity-sidecar encoder. */
    void WriteMetadata(const TestProject &project, const std::string_view text) {
        WriteFile(project.sourceFile.string() + ".horo", {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
    }

    /** @brief Owns a real full cook over a selected output with a larger captured source dependency closure. */
    struct PinnedCookFixture final {
        TestProject project;
        TempDir cache;
        TempDir cooked;
        JobSystem jobs;
        AssetRegistry registry;
        AssetCookRequest request{.sourceRoot = project.dir.path,
                                 .cacheRoot = cache.path,
                                 .cookedRoot = cooked.path,
                                 .target = Target("headless-null")};

        PinnedCookFixture() {
            REQUIRE(registry.Publish({TestMeshRecord()}).status == AssetRegistryBuildStatus::Complete);
            request.registry = registry.Snapshot();
            CookPublicationTestSupport::ConfigureNativeCookPublication(request);
            Capture();
        }

        void Capture() {
            auto inputs = AssetCookInputSnapshot::Capture(project.dir.path, registry.Snapshot(), {}, 4096);
            REQUIRE(inputs.HasValue());
            request.pinnedInputs = std::make_shared<const AssetCookInputSnapshot>(std::move(inputs).Value());
        }

        Result<AssetCookReport> Cook(const std::uint8_t setting = 1, const CancellationToken &token = {}) {
            CookerCatalog catalog;
            REQUIRE(catalog
                        .Register({.contributionId = "test.pinned",
                                   .assetType = Type("core.mesh"),
                                   .targets = {request.target},
                                   .strategy = std::make_shared<const SettingsCooker>(setting)})
                        .HasValue());
            auto snapshot = catalog.Publish();
            REQUIRE(snapshot.HasValue());
            AssetCookService service{jobs, snapshot.Value()};
            return service.Cook(request, token);
        }
    };

    /** @brief Injects deterministic input drift after staging but before the actual selector commit barrier. */
    class DriftingCookFiles final : public Horo::TestSupport::NativePublicationFiles {
    public:
        std::filesystem::path changedSource;
        bool injected{};

        Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            auto result = native.WriteDurable(path, bytes);
            if (result.HasValue() && path.filename() == "current.json" && !injected) {
                WriteFile(changedSource, std::vector<std::uint8_t>{9});
                injected = true;
            }
            return result;
        }
    };
}  // namespace

TEST_CASE("Cook input capture retains actual bytes and canonical identity metadata", "[native][prefab-cook]") {
    TestProject project;
    const auto capture = CaptureProject(project);
    const auto copy = capture;
    REQUIRE(copy.Registry().Records().size() == 1);
    REQUIRE(copy.Sources().size() == 1);
    const auto *source = copy.Find(TestMeshRecord().id);
    REQUIRE(source != nullptr);
    REQUIRE(source->bytes == std::vector<std::uint8_t>{1, 2, 3, 4, 5});
    REQUIRE(source->sourceDigest == ComputeSha256(std::as_bytes(std::span{source->bytes})));
    // Preserve the schema-v1 cache identity for the fixed source/metadata fixture.
    REQUIRE(FormatSha256(copy.ClosureDigest()) == "sha256:8e99de71f9a56dacc1dbd4354f408777ec19876220adfc6d4b399459a04c2cc6");
    REQUIRE(copy.VerifyUnchanged().HasValue());

    WriteFile(project.sourceFile, std::vector<std::uint8_t>{9});
    REQUIRE(copy.VerifyUnchanged().HasError());
    REQUIRE(source->bytes == std::vector<std::uint8_t>{1, 2, 3, 4, 5});
    const auto changed = CaptureProject(project);
    REQUIRE(changed.Find(source->record.id)->sourceDigest != source->sourceDigest);
    REQUIRE(changed.Find(source->record.id)->metadataDigest == source->metadataDigest);
}

TEST_CASE("Cook input capture rejects malformed or foreign identity metadata", "[native][prefab-cook]") {
    TestProject project;
    AssetRegistry registry;
    REQUIRE(registry.Publish({TestMeshRecord()}).status == AssetRegistryBuildStatus::Complete);
    SECTION("malformed") {
        WriteMetadata(project, "{");
    }
    SECTION("foreign identity") {
        WriteMetadata(project, SidecarJson("00000000-0000-0000-0000-0000000000a2", "core.mesh"));
    }
    SECTION("future schema") {
        WriteMetadata(project, R"({"schemaVersion":2,"assetId":"00000000-0000-0000-0000-0000000000a1","assetType":"core.mesh"})");
    }
    REQUIRE(AssetCookInputSnapshot::Capture(project.dir.path, registry.Snapshot(), {}, 4096).HasError());
}

TEST_CASE("Cook input capture excludes bookkeeping from metadata identity but fences actual sidecar drift", "[native][prefab-cook]") {
    TestProject project;
    const auto first = CaptureProject(project);
    WriteMetadata(
        project, R"({"assetType":"core.mesh","assetId":"00000000-0000-0000-0000-0000000000a1","schemaVersion":1,"importedAtUtc":"later"})");
    REQUIRE(first.VerifyUnchanged().HasError());
    const auto second = CaptureProject(project);
    REQUIRE(first.Sources().front().metadataDigest == second.Sources().front().metadataDigest);
    REQUIRE(first.Sources().front().sourceDigest == second.Sources().front().sourceDigest);
}

TEST_CASE("Cook input capture honors aggregate and per-source bounds before allocation", "[native][prefab-cook]") {
    TestProject project;
    AssetRegistry registry;
    REQUIRE(registry.Publish({TestMeshRecord()}).status == AssetRegistryBuildStatus::Complete);
    const auto metadataBytes = std::filesystem::file_size(project.sourceFile.string() + ".horo");
    const auto total = metadataBytes + 5;
    REQUIRE(AssetCookInputSnapshot::Capture(project.dir.path, registry.Snapshot(), {}, total).HasValue());
    REQUIRE(AssetCookInputSnapshot::Capture(project.dir.path, registry.Snapshot(), {}, total - 1).HasError());
    const AssetCookLimits limits{.maximumSourceBytes = 4};
    REQUIRE(AssetCookInputSnapshot::Capture(project.dir.path, registry.Snapshot(), limits, total).HasError());
}

TEST_CASE("Cook input capture and revalidation honor cancellation without changing sources", "[native][prefab-cook]") {
    TestProject project;
    const auto captured = CaptureProject(project);
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    REQUIRE(captured.VerifyUnchanged(cancellation.Token()).HasError());
    REQUIRE(AssetCookInputSnapshot::Capture(project.dir.path, captured.Registry(), {}, 4096, cancellation.Token()).HasError());
    REQUIRE(captured.VerifyUnchanged().HasValue());
}

TEST_CASE("Pinned cook validates fresh and reused envelopes identically and invalidates transitive source changes",
          "[native][prefab-cook]") {
    PinnedCookFixture fixture;
    const auto second = AddSecondMesh(fixture.project);
    REQUIRE(fixture.registry.Publish({TestMeshRecord(), second}).status == AssetRegistryBuildStatus::Complete);
    fixture.Capture();
    const auto fresh = fixture.Cook();
    REQUIRE(fresh.HasValue());
    REQUIRE(fresh.Value().cookedAssets == 1);
    const auto reused = fixture.Cook();
    REQUIRE(reused.HasValue());
    REQUIRE(reused.Value().cacheHits == 1);
    REQUIRE(reused.Value().generation.manifestDigest == fresh.Value().generation.manifestDigest);
    WriteFile(fixture.project.dir.path / second.sourcePath.String(), std::vector<std::uint8_t>{8});
    REQUIRE(fixture.Cook().HasError());
    const auto retained = ResolveCurrentCookGeneration(fixture.request.cookedRoot, fixture.request.limits);
    REQUIRE(retained.HasValue());
    REQUIRE(retained.Value().manifestDigest == fresh.Value().generation.manifestDigest);
    fixture.Capture();
    const auto changed = fixture.Cook();
    REQUIRE(changed.HasValue());
    REQUIRE(changed.Value().cacheHits == 0);
    REQUIRE(changed.Value().generation.manifestDigest != fresh.Value().generation.manifestDigest);
}

TEST_CASE("Pinned cook rechecks source drift at the real selector barrier and retains the previous generation", "[native][prefab-cook]") {
    PinnedCookFixture fixture;
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    auto files = std::make_shared<DriftingCookFiles>();
    files->changedSource = fixture.project.sourceFile;
    fixture.request.publicationFiles = files;
    REQUIRE(fixture.Cook(2).HasError());
    REQUIRE(files->injected);
    const auto retained = ResolveCurrentCookGeneration(fixture.request.cookedRoot, fixture.request.limits);
    REQUIRE(retained.HasValue());
    REQUIRE(retained.Value().manifestDigest == first.Value().generation.manifestDigest);
}

TEST_CASE("Dependent cook uses exact unpublished resource envelopes and one combined selector", "[native][prefab-cook][candidate]") {
    PinnedCookFixture fixture;
    const auto second = AddSecondMesh(fixture.project);
    REQUIRE(fixture.registry.Publish({TestMeshRecord(), second}).status == AssetRegistryBuildStatus::Complete);
    fixture.request.registry = fixture.registry.Snapshot();
    fixture.Capture();
    auto phase = std::make_shared<AssetCookDependentPhase>();
    phase->resourceIds = {TestMeshRecord().id};
    std::optional<AssetCookDependencyIdentity> resource;
    phase->makeCatalog = [&resource, &fixture](const std::span<const AssetCookCandidateArtifactView> views, const CancellationToken &) {
        REQUIRE(views.size() == 1);
        REQUIRE(DecodeCookedArtifact(views.front().envelope).HasValue());
        CHECK(views.front().identity.artifactDigest == ComputeSha256(std::as_bytes(views.front().envelope)));
        resource = views.front().identity;  // Only owned identity survives callback return, never a borrowed envelope view.
        CookerCatalog catalog;
        REQUIRE(catalog
                    .Register({.contributionId = "test.dependent",
                               .assetType = Type("core.mesh"),
                               .targets = {fixture.request.target},
                               .strategy = std::make_shared<const SettingsCooker>(2)})
                    .HasValue());
        return catalog.Publish();
    };
    fixture.request.dependentPhase = phase;
    const auto cooked = fixture.Cook();
    REQUIRE(cooked.HasValue());
    CHECK(cooked.Value().generation.artifactCount == 2);
    REQUIRE(resource.has_value());
    const auto inventory = ReadCookGenerationContents(cooked.Value().generation, 4096);
    REQUIRE(inventory.HasValue());
    const auto published = std::ranges::find(inventory.Value().entries, resource->id, &AssetCookManifestEntry::assetId);
    REQUIRE(published != inventory.Value().entries.end());
    CHECK(published->artifactHash == resource->artifactDigest);
    const auto reused = fixture.Cook();
    REQUIRE(reused.HasValue());
    CHECK(reused.Value().cacheHits == 2);
    CHECK(reused.Value().generation.manifestDigest == cooked.Value().generation.manifestDigest);
}

TEST_CASE("Dependent factory failure or cancellation never selects a resource-only generation", "[native][prefab-cook][candidate]") {
    PinnedCookFixture fixture;
    const auto previous = fixture.Cook();
    REQUIRE(previous.HasValue());
    CancellationSource cancellation;
    auto phase = std::make_shared<AssetCookDependentPhase>();
    phase->resourceIds = {TestMeshRecord().id};
    bool cancel{};
    SECTION("factory error") {}
    SECTION("factory cancellation") {
        cancel = true;
    }
    phase->makeCatalog = [&cancellation, cancel](const std::span<const AssetCookCandidateArtifactView>, const CancellationToken &) {
        if (cancel)
            cancellation.RequestCancellation();
        return Result<std::shared_ptr<const CookerCatalogSnapshot>>::Failure(Error{ErrorCode{"test.dependent.failed"}});
    };
    fixture.request.dependentPhase = phase;
    REQUIRE(fixture.Cook(1, cancellation.Token()).HasError());
    const auto retained = ResolveCurrentCookGeneration(fixture.request.cookedRoot);
    REQUIRE(retained.HasValue());
    CHECK(retained.Value().manifestDigest == previous.Value().generation.manifestDigest);
}
