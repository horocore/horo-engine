#include "Horo/Navigation/NavMeshAssetLoading.h"
#include "navigation/NavigationProjectBakeNativeFixture.h"
#include "navigation/NavigationRegionTestFixture.h"

namespace Horo::Application {
    namespace {
        namespace Nav = Navigation;
        namespace Inputs = NavigationInputTestSupport;
        using NavigationInputNativeTestSupport::Config;
        using NavigationInputNativeTestSupport::Execute;
        using NavigationSourceTestSupport::Asset;

        /** @brief Verifies real durable HNS4 content and exact per-Scene runtime byte residency. */
        void VerifyScopedRuntime(const std::shared_ptr<const NavigationBakePublication> &publication,
                                 const NavigationBakeServiceConfig &config) {
            const auto durable = ResolveNavigationBakePublication(config);
            REQUIRE(durable.HasValue());
            CHECK(durable.Value()->generation.manifestDigest == publication->generation.manifestDigest);
            const auto contents = Assets::ReadCookGenerationContents(publication->generation, config.maximumCandidateBytes);
            REQUIRE(contents.HasValue());
            auto byteCache = Assets::AssetPayloadCache::Create(8, 64U * 1024U).Value();
            for (const auto scene : {Asset(2), Asset(3)}) {
                const auto scope = Nav::NavigationBakeScopeId::ForScene(scene.Bytes()).Value();
                const auto loaded = Nav::LoadNavMeshAsset({config.definition, config.artifactType}, {1}, contents.Value().artifacts.front(),
                                                          config.target, *byteCache, {.scope = scope});
                REQUIRE(loaded.HasValue());
                REQUIRE(loaded.Value().partitions.size() == 1);
                CHECK(loaded.Value().partitions.front().scope == scope);
                CHECK(loaded.Value().tileBytes.size() == 1);
            }
        }

        /** @brief Publishes source removal through the same real service while old readers remain retained by the caller. */
        void VerifySourceRemoval(const Inputs::Project &project, NavigationBakeService &service, const NavigationBakeServiceConfig &config,
                                 const OperationStore &operations, const std::shared_ptr<BakeTestSupport::ControlledBuilder> &builder) {
            auto object = Inputs::Project::Object();
            object.components.navigationSources.clear();
            project.Scenes(object);
            auto revisions = Inputs::Project::Revisions();
            revisions.requestGeneration = Nav::TestSupport::Id<Nav::NavigationBakeRequestGeneration>(3);
            revisions.scene = Nav::TestSupport::Id<Nav::NavigationSceneDocumentRevision>(3);
            revisions.geometry = Nav::TestSupport::Id<Nav::NavigationSourceSnapshotRevision>(3);
            const auto removed = CaptureNavigationProjectBakeInput(project.Capture(), Asset(1), revisions);
            REQUIRE(removed.HasValue());
            const auto empty = Execute(removed.Value(), service, config, operations);
            REQUIRE(empty);
            CHECK(empty->tiles.tiles.empty());
            CHECK(builder->builds.load() == 3);
            CHECK(ResolveNavigationBakePublication(config).Value()->tiles.tiles.empty());
        }

    }  // namespace

    TEST_CASE("Real project sources flow through validation native build selected Scene reuse and empty durable replacement",
              "[navigation][project][bake_input][native]") {
        Inputs::Project project;
        auto captured = CaptureNavigationProjectBakeInput(project.Capture(), Asset(1), Inputs::Project::Revisions());
        REQUIRE(captured.HasValue());
        auto builder = std::make_shared<BakeTestSupport::ControlledBuilder>();
        const auto config = Config(project, builder);
        OperationStore operations{8, 16};
        JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32}};
        auto service = NavigationBakeService::Create(config, operations, jobs).Value();
        CHECK_FALSE(Execute(captured.Value(), *service, config, operations, Nav::NavigationBakeExecutionMode::ValidateOnly));
        CHECK(builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(config.cacheRoot));
        CHECK_FALSE(std::filesystem::exists(config.targetRoot));
        const auto first = Execute(captured.Value(), *service, config, operations);
        REQUIRE(first);
        REQUIRE(first->tiles.tiles.size() == 2);
        CHECK(first->rebuiltTiles == 2);
        CHECK(builder->builds.load() == 2);
        for (const auto &tile : first->tiles.tiles)
            CHECK_FALSE(tile->Topology().IsEmpty());
        VerifyScopedRuntime(first, config);

        auto object = Inputs::Project::Object();
        object.components.colliders.front().localPose.translation.y += 1;
        const std::array objects{object};
        project.source.Write("Assets/first.horo", SceneSource::EncodeSceneSource({objects, {}}));
        auto revisions = Inputs::Project::Revisions();
        revisions.requestGeneration = Nav::TestSupport::Id<Nav::NavigationBakeRequestGeneration>(2);
        revisions.scene = Nav::TestSupport::Id<Nav::NavigationSceneDocumentRevision>(2);
        revisions.geometry = Nav::TestSupport::Id<Nav::NavigationSourceSnapshotRevision>(2);
        const auto changed = CaptureNavigationProjectBakeInput(project.Capture(), Asset(1), revisions);
        REQUIRE(changed.HasValue());
        const auto scope = Nav::NavigationBakeScopeId::ForScene(Asset(2).Bytes()).Value();
        const auto selected = Execute(changed.Value(), *service, config, operations, Nav::NavigationBakeExecutionMode::BuildAndPublish,
                                      std::vector{Nav::NavigationBakeSurfaceKey{Nav::TestSupport::Id<Nav::SurfaceId>(10), scope}});
        REQUIRE(selected);
        CHECK(selected->rebuiltTiles == 1);
        CHECK(selected->reusedTiles == 1);
        CHECK(builder->builds.load() == 3);
        REQUIRE(selected->tiles.tiles.size() == 2);
        CHECK(selected->tiles.tiles[1]->ContentIdentity() == first->tiles.tiles[1]->ContentIdentity());
        CHECK_FALSE(first->tiles.tiles.front()->Topology().IsEmpty());

        VerifySourceRemoval(project, *service, config, operations, builder);
        CHECK(first->tiles.tiles.size() == 2);
    }

    TEST_CASE("Complete region exclusion publishes genuine empty scoped tiles without losing the authored grid",
              "[navigation][project][region][native]") {
        Inputs::Project project;
        const std::array objects{Inputs::Project::Object(),
                                 RegionTestSupport::Region(1, Runtime::NavigationRegionMode::Exclude, {4, 0, 4}, {4, 2, 4})};
        RegionTestSupport::Scenes(project, objects);
        const auto captured = CaptureNavigationProjectBakeInput(project.Capture(), Asset(1), Inputs::Project::Revisions());
        REQUIRE(captured.HasValue());
        REQUIRE(captured.Value().tiles.size() == 2);
        CHECK(captured.Value().input->Triangles().empty());
        auto builder = std::make_shared<BakeTestSupport::ControlledBuilder>();
        const auto config = Config(project, builder);
        OperationStore operations{8, 16};
        JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32}};
        auto service = NavigationBakeService::Create(config, operations, jobs).Value();
        const auto publication = Execute(captured.Value(), *service, config, operations);
        REQUIRE(publication);
        REQUIRE(publication->tiles.tiles.size() == 2);
        for (const auto &tile : publication->tiles.tiles) {
            CHECK(tile->Topology().IsEmpty());
            CHECK(tile->Key().scope.IsSceneScoped());
        }
        const auto durable = ResolveNavigationBakePublication(config);
        REQUIRE(durable.HasValue());
        CHECK(durable.Value()->tiles.tiles.size() == 2);
        CHECK(builder->builds.load() == 2);
    }

    TEST_CASE("Real project selected source removal or disable retains the other Scene without rebuilding unselected topology",
              "[navigation][project][selection][empty][native]") {
        Inputs::Project project;
        const auto firstInput = CaptureNavigationProjectBakeInput(project.Capture(), Asset(1), Inputs::Project::Revisions());
        REQUIRE(firstInput.HasValue());
        auto builder = std::make_shared<BakeTestSupport::ControlledBuilder>();
        const auto config = Config(project, builder);
        OperationStore operations{8, 16};
        JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32}};
        auto service = NavigationBakeService::Create(config, operations, jobs).Value();
        const auto previous = Execute(firstInput.Value(), *service, config, operations);
        REQUIRE(previous->tiles.tiles.size() == 2);
        auto removed = Inputs::Project::Object();
        if (GENERATE(false, true))
            removed.components.navigationSurface->enabled = false;
        else
            removed.components.navigationSources.clear();
        const std::array objects{removed};
        project.source.Write("Assets/first.horo", SceneSource::EncodeSceneSource({objects, {}}));
        auto revisions = Inputs::Project::Revisions();
        revisions.requestGeneration = Nav::TestSupport::Id<Nav::NavigationBakeRequestGeneration>(2);
        revisions.scene = Nav::TestSupport::Id<Nav::NavigationSceneDocumentRevision>(2);
        revisions.geometry = Nav::TestSupport::Id<Nav::NavigationSourceSnapshotRevision>(2);
        const auto current = CaptureNavigationProjectBakeInput(project.Capture(), Asset(1), revisions);
        REQUIRE(current.HasValue());
        REQUIRE(current.Value().surfaces.size() == 2);
        REQUIRE(current.Value().tiles.size() == 1);
        const auto scope = Nav::NavigationBakeScopeId::ForScene(Asset(2).Bytes()).Value();
        const auto selected = Execute(current.Value(), *service, config, operations, Nav::NavigationBakeExecutionMode::BuildAndPublish,
                                      std::vector{Nav::NavigationBakeSurfaceKey{Nav::TestSupport::Id<Nav::SurfaceId>(10), scope}});
        REQUIRE(selected->tiles.tiles.size() == 1);
        CHECK(selected->tiles.tiles.front()->Key().scope == Nav::NavigationBakeScopeId::ForScene(Asset(3).Bytes()).Value());
        CHECK(selected->rebuiltTiles == 0);
        CHECK(selected->reusedTiles == 1);
        CHECK(builder->builds.load() == 2);
        CHECK(previous->tiles.tiles.size() == 2);
        CHECK(ResolveNavigationBakePublication(config).Value()->tiles.tiles.size() == 1);
    }

}  // namespace Horo::Application
