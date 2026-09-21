#include "SceneDocumentPersistenceTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

using namespace Horo;
using namespace Horo::Editor;
using namespace Horo::Editor::PersistenceTestSupport;

TEST_CASE("Scene File Watch Reset Discards Completed Stale Generation", "[unit][editor][persistence][watch]") {
    TemporaryProject project;
    project.PrepareEmptyScene();

    JobSystem jobs(JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8});
    {
        SceneFileWatchService watcher(jobs);
        const auto staleGeneration = watcher.Request(project.Root(), project.ScenePath());
        REQUIRE((staleGeneration.HasValue()));
        for (std::size_t attempt = 0; attempt < 100'000 && watcher.HasPendingInspection(); ++attempt)
            std::this_thread::yield();
        REQUIRE((!watcher.HasPendingInspection()));

        watcher.Reset();
        REQUIRE((watcher.DrainUpdates().empty()));

        project.WriteScene("{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n");
        const auto currentGeneration = watcher.Request(project.Root(), project.ScenePath());
        REQUIRE((currentGeneration.HasValue()));
        REQUIRE((currentGeneration.Value() > staleGeneration.Value()));

        const SceneFileWatchUpdate update = WaitForWatchUpdate(watcher);
        REQUIRE((update.generation == currentGeneration.Value()));
    }
    jobs.Shutdown(ShutdownPolicy::Drain);
}
