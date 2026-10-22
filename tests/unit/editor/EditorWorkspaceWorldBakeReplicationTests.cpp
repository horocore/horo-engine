#include "../../helpers/ProjectReplicationTestSupport.h"
#include "Horo/Network/NetworkErrors.h"
#include "OwnedTestDirectory.h"
#include "PartitionedWorldResidencyTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

namespace Horo::Editor {
    TEST_CASE("Actual workspace cook gates changed authored replication before scheduling and preserves prior output",
              "[editor][world_bake][controller][replication]") {
        Tests::OwnedTestDirectory project{"world bake replication publication"};
        Network::NetworkReplicationInventory complete{Network::NetworkReplicationInventoryCompleteness::Complete, {}};
        Tests::WriteReplicationProject(project.Path(), complete);
        JobSystem jobs{{.workerCount = 1}};
        OperationStore operations{8, 16};
        BuildOutputStore output{32};
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{project.Path(),
                                             runtime,
                                             {},
                                             EditorWorkspaceDependencies{.jobs = &jobs,
                                                                         .worldBakeOperations = &operations,
                                                                         .worldBakeOutput = &output,
                                                                         .documentSessionOwner = WorldResidencyTests::Asset(9)}};
        auto &world = controller.WorldDocuments();
        REQUIRE(world.Open(WorldResidencyTests::Document()).HasValue());
        for (const auto &page : WorldResidencyTests::Document().pages)
            REQUIRE(world.LoadPage(page.head, WorldResidencyTests::Source(), WorldResidencyTests::Current(world)->fence).HasValue());
        REQUIRE(controller.StartWorldBake(Application::WorldBakeMode::Bake, world.Snapshot().Value()->fence).HasValue());
        auto marker = jobs.SubmitResult({}, [](const CancellationToken &) {
            return Result<void>::Success();
        });
        REQUIRE(marker.HasValue());
        REQUIRE(marker.Value()
                    .Wait({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed, .timeout = Duration::FromMilliseconds(5000)})
                    .HasValue());
        controller.UpdateWorldBake();
        REQUIRE(controller.ViewModel().worldBakeCurrent);
        const auto published = output.SnapshotIfChanged(0);
        REQUIRE(published);
        {
            std::ofstream invalid{project.Path() / ".horo/project.json", std::ios::trunc};
            invalid << "{invalid}";
        }
        controller.UpdateGameplaySources(0.5F);
        controller.UpdateWorldBake();
        CHECK_FALSE(controller.ViewModel().worldBakeCurrent);
        CHECK(controller.StartWorldBake(Application::WorldBakeMode::Bake, world.Snapshot().Value()->fence).HasError());
        CHECK_FALSE(output.SnapshotIfChanged(published->revision));
        Tests::WriteReplicationProject(project.Path(), complete);
        controller.UpdateGameplaySources(0.5F);
        controller.UpdateWorldBake();
        CHECK(controller.ViewModel().worldBakeCurrent);
        CHECK(controller.StartWorldBake(Application::WorldBakeMode::Validate, world.Snapshot().Value()->fence).HasValue());
    }

}  // namespace Horo::Editor
