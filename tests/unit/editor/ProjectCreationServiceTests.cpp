#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {
    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() / ("horo-project-service-" + std::to_string(stamp));
            std::filesystem::create_directories(path_);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        const std::filesystem::path &Path() const noexcept {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    std::string Read(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    Horo::Editor::ProjectCreationRequest ValidRequest(const std::filesystem::path &root) {
        return {
            .templateId = "3d-starter",
            .projectName = "MyGame",
            .projectRoot = root,
            .projectVersion = "0.1.0",
            .defaultScene = "Assets/Scenes/main.horo",
            .renderBackend = "opengl",
            .physicsEnabled = true,
            .targetFrameRate = 60,
            .buildProfile = "desktop-debug",
            .assetCompression = "lz4",
            .textureCompression = "bc7",
            .targetPlatform = "host",
            .compilerFamily = "default",
            .minimumCxxStandard = 20,
            .initializeGit = true,
            .restorePackages = true,
            .includeStarterContent = true,
            .generateCMakeProject = true,
        };
    }

    Horo::Editor::ProjectCreationSnapshot WaitForTerminal(Horo::Editor::ProjectCreationService &service,
                                                          Horo::Editor::ProjectCreationOperationId id) {
        for (int attempt = 0; attempt < 2000; ++attempt) {
            const auto snapshot = service.Query(id);
            REQUIRE((snapshot.has_value()));
            if (snapshot->state == Horo::Editor::ProjectCreationOperationState::Succeeded ||
                snapshot->state == Horo::Editor::ProjectCreationOperationState::Failed ||
                snapshot->state == Horo::Editor::ProjectCreationOperationState::Cancelled) {
                return *snapshot;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE((false && "project creation did not finish"));
        return {};
    }

    TEST_CASE("Creates Portable Project In An Atomic Promotion", "[unit][editor]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        Horo::Editor::ProjectCreationService service{jobs, bus};
        const auto root = temporary.Path() / "MyGame";

        const auto started = service.StartCreate(ValidRequest(root));
        REQUIRE((started.HasValue()));
        const auto snapshot = WaitForTerminal(service, started.Value().id);
        REQUIRE((snapshot.state == Horo::Editor::ProjectCreationOperationState::Succeeded));
        REQUIRE((snapshot.phase == Horo::Editor::ProjectCreationOperationPhase::Completed));
        REQUIRE((std::filesystem::is_directory(root / ".horo")));
        REQUIRE((std::filesystem::is_directory(root / "Assets/Models")));
        REQUIRE((std::filesystem::is_directory(root / "Assets/Textures")));
        REQUIRE((std::filesystem::is_directory(root / "Assets/Materials")));
        REQUIRE((std::filesystem::is_directory(root / "Assets/Shaders")));
        REQUIRE((std::filesystem::is_regular_file(root / "Assets/Scenes/main.horo")));
        REQUIRE((std::filesystem::is_regular_file(root / "CMakeLists.txt")));
        REQUIRE((std::filesystem::is_directory(root / "source/gameplay")));
        REQUIRE_FALSE((std::filesystem::exists(root / "source/gameplay/GameModule.cpp")));
        REQUIRE((Read(root / "CMakeLists.txt").find("horo_add_gameplay_module") != std::string::npos));
        REQUIRE((Read(root / "CMakeLists.txt").find("MODULE_ID game.mygame") != std::string::npos));

        const std::string project = Read(root / ".horo/project.json");
        REQUIRE((project.find("\"horoVersion\": \"0.1.0\"") != std::string::npos));
        REQUIRE((project.find("\"persistentContract\": \"sha256:") != std::string::npos));
        REQUIRE((project.find("\"name\": \"MyGame\"") != std::string::npos));
        REQUIRE((project.find("\"projectVersion\": \"0.1.0\"") != std::string::npos));
        REQUIRE((project.find("\"defaultScene\": \"Assets/Scenes/main.horo\"") != std::string::npos));
        REQUIRE((project.find("\"minimumCxxStandard\": 20") != std::string::npos));
        REQUIRE((project.find("\"assetCompression\": \"lz4\"") != std::string::npos));
        REQUIRE((project.find("\"textureCompression\": \"bc7\"") != std::string::npos));
        const auto compatibility = Horo::Application::InspectProjectCompatibility(root);
        REQUIRE((compatibility.status == Horo::Application::ProjectCompatibilityStatus::Current));
        REQUIRE((compatibility.metadata.has_value()));
        REQUIRE((compatibility.metadata->projectVersion == "0.1.0"));
        REQUIRE((Read(root / ".horo/plugins.json") == "{\n  \"schemaVersion\": 1,\n  \"requestedPlugins\": []\n}\n"));
        REQUIRE((Read(root / ".horo/input.json") ==
                 "{\n  \"schemaVersion\": 1,\n  \"profileId\": \"project-default\",\n  \"overrides\": []\n}\n"));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Creates An Empty Project Without A Default Scene", "[unit][editor][project-creation]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        Horo::Editor::ProjectCreationService service{jobs, bus};
        const auto root = temporary.Path() / "EmptyGame";
        auto request = ValidRequest(root);
        request.templateId = "empty";
        request.defaultScene.clear();
        request.includeStarterContent = false;

        const auto started = service.StartCreate(std::move(request));
        REQUIRE((started.HasValue()));
        const auto snapshot = WaitForTerminal(service, started.Value().id);
        REQUIRE((snapshot.state == Horo::Editor::ProjectCreationOperationState::Succeeded));
        REQUIRE((std::filesystem::is_directory(root / "Assets/Scenes")));
        REQUIRE((std::filesystem::is_empty(root / "Assets/Scenes")));
        REQUIRE((Read(root / ".horo/project.json").find("\"defaultScene\": \"\"") != std::string::npos));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Rejects A Default Scene Without Starter Content", "[unit][editor][project-creation]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        Horo::Editor::ProjectCreationService service{jobs, bus};
        auto request = ValidRequest(temporary.Path() / "IncoherentGame");
        request.includeStarterContent = false;

        const auto started = service.StartCreate(std::move(request));
        REQUIRE((started.HasError()));
        REQUIRE((started.ErrorValue().code.Value() == "project_creation.invalid_request"));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Refuses Occupied Destination Without Overwriting It", "[unit][editor]") {
        TemporaryDirectory temporary;
        const auto root = temporary.Path() / "occupied";
        std::filesystem::create_directories(root);
        std::ofstream(root / "keep.txt") << "keep";
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        Horo::Editor::ProjectCreationService service{jobs, bus};

        const auto started = service.StartCreate(ValidRequest(root));
        REQUIRE((started.HasError()));
        REQUIRE((started.ErrorValue().code.Value() == "project_creation.destination_occupied"));
        REQUIRE((Read(root / "keep.txt") == "keep"));
        REQUIRE((!std::filesystem::exists(root / ".horo")));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Cancellation Leaves No Destination Or Staging Directory", "[unit][editor]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 0, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        Horo::Editor::ProjectCreationService service{jobs, bus};
        std::atomic<int> createdEvents{0};
        const auto subscription = bus.Subscribe<Horo::Editor::ProjectCreatedEvent>([&](const auto &) {
            ++createdEvents;
        });
        const auto root = temporary.Path() / "Cancelled";

        const auto started = service.StartCreate(ValidRequest(root));
        REQUIRE((started.HasValue()));
        REQUIRE((service.RequestCancel(started.Value().id).HasValue()));
        const auto snapshot = WaitForTerminal(service, started.Value().id);
        REQUIRE((snapshot.state == Horo::Editor::ProjectCreationOperationState::Cancelled));
        REQUIRE((!std::filesystem::exists(root)));
        for (const auto &entry : std::filesystem::directory_iterator(temporary.Path())) {
            REQUIRE((entry.path().filename().string().find(".horo-create-") == std::string::npos));
        }
        service.PumpMainThread();
        REQUIRE((createdEvents.load() == 0));
        jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
    }

    TEST_CASE("Publishes Committed Events Only After Main Thread Dispatch", "[unit][editor]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        Horo::Editor::ProjectCreationService service{jobs, bus};
        std::atomic<int> createdEvents{0};
        std::atomic<int> revisionEvents{0};
        const auto createdSubscription = bus.Subscribe<Horo::Editor::ProjectCreatedEvent>([&](const auto &) {
            ++createdEvents;
        });
        const auto revisionSubscription = bus.Subscribe<Horo::Editor::ProjectCreationRevisionChangedEvent>([&](const auto &) {
            ++revisionEvents;
        });

        const auto started = service.StartCreate(ValidRequest(temporary.Path() / "EventProject"));
        REQUIRE((started.HasValue()));
        REQUIRE((WaitForTerminal(service, started.Value().id).state == Horo::Editor::ProjectCreationOperationState::Succeeded));
        REQUIRE((createdEvents.load() == 0));
        REQUIRE((revisionEvents.load() == 0));
        service.PumpMainThread();
        REQUIRE((createdEvents.load() == 1));
        REQUIRE((revisionEvents.load() == 1));
        service.PumpMainThread();
        REQUIRE((createdEvents.load() == 1));
        REQUIRE((revisionEvents.load() == 1));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }
}  // namespace
