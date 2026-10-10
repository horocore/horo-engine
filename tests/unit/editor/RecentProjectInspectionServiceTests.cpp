#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Editor/RecentProjectInspectionService.h"
#include "Horo/Foundation/JobSystem.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Editor;

    class TemporaryProjects {
    public:
        TemporaryProjects()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-recent-inspection-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(root);
        }

        ~TemporaryProjects() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }

        std::filesystem::path WriteProject(const std::string &directory, const EngineReleaseVersion version,
                                           const PersistentContractHash &contract) {
            const auto project = root / directory;
            std::filesystem::create_directories(project / ".horo");
            std::ofstream(project / ".horo/project.json", std::ios::binary)
                << "{\"horoVersion\":\"" << FormatHoroVersion(version.value) << "\",\"persistentContract\":\""
                << FormatPersistentContractHash(contract) << "\",\"projectId\":\"" << directory
                << "\",\"name\":\"Inspection\",\"projectVersion\":\"0.1.0\","
                   "\"createdAt\":\"2026-07-19T00:00:00Z\",\"settings\":{\"renderBackend\":\"opengl\"}}\n";
            return project;
        }

        std::filesystem::path root;
    };

    std::vector<RecentProjectInspectionUpdate> WaitForUpdates(RecentProjectInspectionService &service, const std::size_t expected) {
        std::vector<RecentProjectInspectionUpdate> result;
        for (int attempt = 0; attempt < 1000 && result.size() < expected; ++attempt) {
            auto updates = service.DrainUpdates();
            result.insert(result.end(), std::make_move_iterator(updates.begin()), std::make_move_iterator(updates.end()));
            if (result.size() < expected)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return result;
    }
}  // namespace

TEST_CASE("Recent Project Inspection Service Tests", "[unit][editor]") {
    TemporaryProjects projects;
    const auto &compatibility = BuiltInReleaseCompatibilityRegistry();
    const auto *current = compatibility.Find(CurrentEngineReleaseVersion());
    const auto legacyVersion = ParseHoroVersion("0.0.1");
    REQUIRE((current != nullptr && legacyVersion.HasValue()));
    const auto *legacy = compatibility.Find(EngineReleaseVersion{legacyVersion.Value()});
    REQUIRE((legacy != nullptr));
    const auto currentRoot = projects.WriteProject("current", current->release, current->persistentContract);
    const auto legacyRoot = projects.WriteProject("legacy", legacy->release, legacy->persistentContract);
    const auto corruptRoot = projects.root / "corrupt";
    std::filesystem::create_directories(corruptRoot / ".horo");
    std::ofstream(corruptRoot / ".horo/project.json") << "{broken";

    JobSystem jobs{{.workerCount = 4, .maxQueuedJobs = 16, .ioWorkerCount = 1}};
    NativeDurableFileSystem files;
    SystemWallClock clock;
    ProjectMutationCoordinator mutations{files};
    ProjectMigrationTransactionService transactions{files, clock, mutations, jobs};
    ProjectOpenPreflightService preflight{transactions};
    RecentProjectInspectionService service{jobs, preflight};

    const std::vector entries{RecentProjectEntry{"Current", currentRoot.string(), {}, {}},
                              RecentProjectEntry{"Legacy", legacyRoot.string(), {}, {}},
                              RecentProjectEntry{"Corrupt", corruptRoot.string(), {}, {}}};
    const auto generation = service.Refresh(entries);
    REQUIRE((generation.HasValue()));
    const auto updates = WaitForUpdates(service, 3);
    REQUIRE((updates.size() == 3));
    bool sawCurrent = false;
    bool sawLegacy = false;
    bool sawCorrupt = false;
    for (const auto &update : updates) {
        REQUIRE((update.generation == generation.Value()));
        REQUIRE((update.projection.inspectionState == RecentProjectInspectionState::Fresh));
        sawCurrent |= update.projection.status == ProjectCompatibilityStatus::Current;
        sawLegacy |= update.projection.status == ProjectCompatibilityStatus::AutomaticMigrationRequired;
        sawCorrupt |= update.projection.status == ProjectCompatibilityStatus::Corrupt;
    }
    REQUIRE((sawCurrent && sawLegacy && sawCorrupt));

    REQUIRE((service.Refresh(entries).HasValue()));
    const std::vector newest{RecentProjectEntry{"Corrupt", corruptRoot.string(), {}, {}}};
    const auto newestGeneration = service.Refresh(newest);
    REQUIRE((newestGeneration.HasValue()));
    const auto newestUpdates = WaitForUpdates(service, 1);
    REQUIRE((newestUpdates.size() == 1));
    REQUIRE((newestUpdates.front().generation == newestGeneration.Value()));
    REQUIRE((newestUpdates.front().rootPath == corruptRoot.string()));

    service.Shutdown();
    service.Shutdown();
    jobs.Shutdown(ShutdownPolicy::Drain);
}
