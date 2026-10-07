#pragma once

/** @file PrefabSceneCookHostFixture.h @brief Target-private project and publication fixture shared by static and runtime host tests. */
#include "Horo/Application/PrefabSceneCookHost.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Scene/SceneSource.h"
#include "ReleaseTestFixtures.h"
#include "assets/AssetCookServiceFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Application::PrefabCookTestSupport {
    using Assets::AssetCookGeneration;
    using Assets::AssetCookReport;
    using Assets::AssetId;
    using Assets::AssetRecord;
    using Assets::AssetRegistry;
    using Assets::AssetRegistryBuildStatus;
    using Assets::CookerCatalog;
    using Assets::ReadCookGenerationContents;
    using Assets::ResolveCurrentCookGeneration;
    namespace CookPublicationTestSupport = Assets::CookPublicationTestSupport;
    using Assets::ServiceTestSupport::Id;
    using Assets::ServiceTestSupport::SettingsCooker;
    using Assets::ServiceTestSupport::SidecarJson;
    using Assets::ServiceTestSupport::Target;
    using Assets::ServiceTestSupport::TempDir;
    using Assets::ServiceTestSupport::TestMeshRecord;
    using Assets::ServiceTestSupport::TestProject;
    using Assets::ServiceTestSupport::Type;
    using Assets::ServiceTestSupport::WriteFile;

    /** @brief Injects an action at real staged-selector write, or fails its native replacement before commit. */
    class HostPublicationFiles final : public Horo::TestSupport::NativePublicationFiles {
    public:
        std::function<void()> staged;
        bool failReplacement{};
        bool observed{};
        std::size_t selectorWrites{};

        Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            auto written = NativePublicationFiles::WriteDurable(path, bytes);
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

}  // namespace Horo::Application::PrefabCookTestSupport
