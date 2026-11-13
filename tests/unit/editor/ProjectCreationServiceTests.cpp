#include "../../support/project/ProjectCreationFixture.h"

namespace {
    using namespace Horo::Tests::ProjectCreation;

    TEST_CASE("Creates Portable Project In An Atomic Promotion", "[unit][editor][platform-creation]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        Horo::Editor::ProjectCreationService service{jobs, bus, std::make_shared<CreationRandom>(),
                                                     std::make_shared<Horo::NativeDurableFileSystem>()};
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
        const auto current = Horo::Application::CurrentEngineReleaseVersion();
        REQUIRE((project.find("\"horoVersion\": \"" + Horo::Application::FormatHoroVersion(current.value) + "\"") != std::string::npos));
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
        CheckPlatformAuthority(root, project);
        CheckPhysicsCatalogs(root, compatibility.metadata->projectId);
        REQUIRE((compatibility.metadata->horoVersion == current));
        const auto *decision = Horo::Application::BuiltInReleaseCompatibilityRegistry().Find(current);
        REQUIRE(decision != nullptr);
        REQUIRE((compatibility.metadata->persistentContract == decision->persistentContract));
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
        Horo::Editor::ProjectCreationService service{jobs, bus, std::make_shared<CreationRandom>(),
                                                     std::make_shared<Horo::NativeDurableFileSystem>()};
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
        Horo::Editor::ProjectCreationService service{jobs, bus, std::make_shared<CreationRandom>(),
                                                     std::make_shared<Horo::NativeDurableFileSystem>()};
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
        Horo::Editor::ProjectCreationService service{jobs, bus, std::make_shared<CreationRandom>(),
                                                     std::make_shared<Horo::NativeDurableFileSystem>()};

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
        Horo::Editor::ProjectCreationService service{jobs, bus, std::make_shared<CreationRandom>(),
                                                     std::make_shared<Horo::NativeDurableFileSystem>()};
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

    TEST_CASE("Shutdown revokes queued creation without leaving a permanent queued operation", "[unit][editor][platform-creation]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 0, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        auto random = std::make_shared<CreationRandom>();
        Horo::Editor::ProjectCreationService service{jobs, bus, random, std::make_shared<Horo::NativeDurableFileSystem>()};
        std::atomic<int> created{};
        std::atomic<int> changed{};
        const auto createdSubscription = bus.Subscribe<Horo::Editor::ProjectCreatedEvent>([&](const auto &) {
            ++created;
        });
        const auto revisionSubscription = bus.Subscribe<Horo::Editor::ProjectCreationRevisionChangedEvent>([&](const auto &) {
            ++changed;
        });
        const auto root = temporary.Path() / "ShutdownQueued";
        const auto accepted = service.StartCreate(ValidRequest(root));
        REQUIRE(accepted.HasValue());
        const auto queued = service.Query(accepted.Value().id);
        REQUIRE(queued.has_value());
        REQUIRE(queued->state == Horo::Editor::ProjectCreationOperationState::Queued);
        jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
        const auto terminal = service.Query(accepted.Value().id);
        REQUIRE(terminal.has_value());
        CHECK(terminal->state == Horo::Editor::ProjectCreationOperationState::Cancelled);
        CHECK(terminal->publication == Horo::Editor::ProjectCreationPublicationState::Unpublished);
        REQUIRE(terminal->error.has_value());
        REQUIRE(terminal->error->cause.has_value());
        CHECK(terminal->error->cause->code.Value() == "job.cancelled");
        const auto repeated = service.Query(accepted.Value().id);
        REQUIRE(repeated.has_value());
        CHECK(repeated->state == Horo::Editor::ProjectCreationOperationState::Cancelled);
        CHECK(random->calls == 0);
        CHECK(std::filesystem::is_empty(temporary.Path()));
        CHECK(changed.load() == 0);
        service.PumpMainThread();
        CHECK(changed.load() == 1);
        CHECK(created.load() == 0);
    }

    TEST_CASE("Publishes Committed Events Only After Main Thread Dispatch", "[unit][editor]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        Horo::Editor::ProjectCreationService service{jobs, bus, std::make_shared<CreationRandom>(),
                                                     std::make_shared<Horo::NativeDurableFileSystem>()};
        std::atomic<int> createdEvents{0};
        std::atomic<int> revisionEvents{0};
        std::atomic<int> foreignThreadEvents{0};
        const auto ownerThread = std::this_thread::get_id();
        const auto createdSubscription = bus.Subscribe<Horo::Editor::ProjectCreatedEvent>([&](const auto &) {
            ++createdEvents;
            if (std::this_thread::get_id() != ownerThread)
                ++foreignThreadEvents;
        });
        const auto revisionSubscription = bus.Subscribe<Horo::Editor::ProjectCreationRevisionChangedEvent>([&](const auto &) {
            ++revisionEvents;
            if (std::this_thread::get_id() != ownerThread)
                ++foreignThreadEvents;
        });

        const auto started = service.StartCreate(ValidRequest(temporary.Path() / "EventProject"));
        REQUIRE((started.HasValue()));
        REQUIRE((WaitForTerminal(service, started.Value().id).state == Horo::Editor::ProjectCreationOperationState::Succeeded));
        // Terminal state commits the directory; joining also finishes both worker-side event publications.
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
        REQUIRE((createdEvents.load() == 0));
        REQUIRE((revisionEvents.load() == 0));
        service.PumpMainThread();
        REQUIRE((createdEvents.load() == 1));
        REQUIRE((revisionEvents.load() == 1));
        service.PumpMainThread();
        REQUIRE((createdEvents.load() == 1));
        REQUIRE((revisionEvents.load() == 1));
        REQUIRE((foreignThreadEvents.load() == 0));
    }

    TEST_CASE("Identity preparation failures publish no project or staging directory", "[unit][editor][platform-creation]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        auto random = std::make_shared<CreationRandom>();
        std::shared_ptr<Horo::Security::SecureRandomSource> injected = random;
        std::string expected{Horo::SecurityErrors::EntropyUnavailable.code.Value()};
        SECTION("No injected capability") {
            injected.reset();
        }
        SECTION("Entropy failure") {
            random->fail = true;
        }
        SECTION("Zero salt") {
            random->zero = true;
            expected = Horo::PlatformServices::StableIdErrors::InvalidSalt.code.Value();
        }
        Horo::Editor::ProjectCreationService service{jobs, bus, injected, std::make_shared<Horo::NativeDurableFileSystem>()};
        const auto destination = temporary.Path() / "Unpublished";
        const auto started = service.StartCreate(ValidRequest(destination));
        REQUIRE(started.HasValue());
        const auto terminal = WaitForTerminal(service, started.Value().id);
        REQUIRE(terminal.state == Horo::Editor::ProjectCreationOperationState::Failed);
        REQUIRE(terminal.error.has_value());
        CHECK(terminal.error->code == Horo::Editor::ProjectCreationErrorCode::IdentityPreparationFailed);
        REQUIRE(terminal.error->cause.has_value());
        CHECK(terminal.error->cause->code.Value() == expected);
        CHECK(random->calls == (injected ? 1U : 0U));
        CHECK_FALSE(std::filesystem::exists(destination));
        CHECK(std::filesystem::is_empty(temporary.Path()));
    }

    TEST_CASE("Creation rejects starter paths that could replace identity authority", "[unit][editor][platform-creation]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        Horo::Editor::ProjectCreationService service{jobs, bus, std::make_shared<CreationRandom>(),
                                                     std::make_shared<Horo::NativeDurableFileSystem>()};
        for (const auto &scene : {".horo/project.json", ".HORO/platform_services.json", "Assets/../.horo/project.json"}) {
            auto request = ValidRequest(temporary.Path() / "MyGame");
            request.defaultScene = scene;
            CHECK(service.StartCreate(std::move(request)).HasError());
        }
        CHECK(std::filesystem::is_empty(temporary.Path()));
    }

    TEST_CASE("Creation preserves portable UTF-8 starter paths on the native filesystem", "[unit][editor][platform-creation]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        Horo::Editor::ProjectCreationService service{jobs, bus, std::make_shared<CreationRandom>(),
                                                     std::make_shared<Horo::NativeDurableFileSystem>()};
        auto request = ValidRequest(temporary.Path() / std::filesystem::path{u8"project ü 图"});
        const std::u8string scene{u8"Assets/Scenes/başlangıç 图.horo"};
        request.defaultScene.assign(scene.begin(), scene.end());
        const auto root = request.projectRoot;
        const auto started = service.StartCreate(request);
        REQUIRE(started.HasValue());
        const auto terminal = WaitForTerminal(service, started.Value().id);
        REQUIRE(terminal.state == Horo::Editor::ProjectCreationOperationState::Succeeded);
        CHECK(terminal.publication == Horo::Editor::ProjectCreationPublicationState::Durable);
        CHECK(std::filesystem::is_regular_file(root / std::filesystem::path{scene}));
        CHECK(Read(root / ".horo/project.json").find(request.defaultScene) != std::string::npos);
    }

}  // namespace
