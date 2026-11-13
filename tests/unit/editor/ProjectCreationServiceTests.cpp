#include "../../support/NativePublicationFiles.h"
#include "Horo/Application/PlatformServicesAuthoring.h"
#include "Horo/Application/PlatformServicesDefinitionCodec.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Security/SecureMemory.h"
#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
    /** @brief Injects failures around genuine durable preparation and irreversible native publication. */
    class CreationFiles final : public Horo::TestSupport::NativePublicationFiles {
    public:
        bool failWrite{};
        bool failSync{};
        bool failBeforeCommit{};
        bool failAfterCommit{};
        bool throwAfterCommit{};
        bool throwDuringWrite{};
        bool throwBeforeCommit{};
        bool allocationAfterCommit{};
        bool unknownAfterCommit{};

        Horo::Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            if (throwDuringWrite)
                throw std::runtime_error("injected preparation exception");
            if (failWrite)
                return Failure();
            return native.WriteDurable(path, bytes);
        }

        Horo::Result<void> SyncDirectory(const std::filesystem::path &path) override {
            if (failSync)
                return Failure();
            return native.SyncDirectory(path);
        }

        Horo::Result<void> PublishDirectoryNoReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                                            Horo::AtomicFileReplacementReceipt &receipt) override {
            if (throwBeforeCommit)
                throw std::runtime_error("injected publication exception");
            if (failBeforeCommit)
                return Failure();
            auto published = native.PublishDirectoryNoReplaceTracked(prepared, destination, receipt);
            if (published.HasError())
                return published;
            if (throwAfterCommit)
                throw std::runtime_error("injected post-commit failure");
            if (allocationAfterCommit)
                throw std::bad_alloc{};
            if (unknownAfterCommit)
                throw 41;
            if (failAfterCommit)
                return Failure();
            return published;
        }

    private:
        static Horo::Result<void> Failure() {
            return Horo::Result<void>::Failure(Horo::MakeError(Horo::SecurityErrors::EntropyUnavailable, "injected filesystem failure"));
        }
    };

    /** @brief Supplies deterministic nonzero entropy and can simulate zero/failure without native discovery. */
    class CreationRandom final : public Horo::Security::SecureRandomSource {
    public:
        bool fail{};
        bool zero{};
        std::uint32_t calls{};

        Horo::Result<void> Fill(const std::span<std::byte> destination) override {
            ++calls;
            std::ranges::fill(destination, zero ? std::byte{} : std::byte{0xA5});
            if (fail)
                return Horo::Result<void>::Failure(Horo::MakeError(Horo::SecurityErrors::EntropyUnavailable));
            return Horo::Result<void>::Success();
        }
    };

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
        const auto source = Horo::Application::DecodeProjectSourceDocument(project);
        REQUIRE(source.HasValue());
        const auto salt = Horo::Application::ResolveProjectPlatformServicesSalt(source.Value());
        REQUIRE(salt.HasValue());
        const auto ledgerBytes = Read(root / Horo::Application::PlatformServicesDocumentPaths::Ledger);
        const auto ids = Horo::Application::DecodePlatformServicesLedger(ledgerBytes, source.Value().metadata.projectId, salt.Value());
        REQUIRE(ids.HasValue());
        CHECK(ids.Value().Entries().empty());
        CHECK(ledgerBytes.find("psid1:") == std::string::npos);
        const auto achievements =
            Horo::Application::DecodePlatformAchievementDefinitions(Read(root /
                                                                         Horo::Application::PlatformServicesDocumentPaths::Achievements),
                                                                    ids.Value());
        const auto stats =
            Horo::Application::DecodePlatformStatDefinitions(Read(root / Horo::Application::PlatformServicesDocumentPaths::Stats),
                                                             ids.Value());
        REQUIRE(achievements.HasValue());
        REQUIRE(stats.HasValue());
        REQUIRE(
            Horo::Application::DecodePlatformLeaderboardDefinitions(Read(root /
                                                                         Horo::Application::PlatformServicesDocumentPaths::Leaderboards),
                                                                    ids.Value(), stats.Value())
                .HasValue());
        REQUIRE(
            Horo::Application::DecodePlatformPresenceDefinitions(Read(root / Horo::Application::PlatformServicesDocumentPaths::Presence),
                                                                 ids.Value())
                .HasValue());
        const auto collision = Horo::Application::LoadProjectCollisionSettings(root, compatibility.metadata->projectId);
        REQUIRE(collision.HasValue());
        CHECK(collision.Value().settings->Input().revision == 1);
        CHECK(collision.Value().settings->Schema()->Profiles().size() == 1);
        CHECK(collision.Value().settings->Capture(1).HasValue());
        const auto surfaces = Horo::Application::LoadProjectSurfaceSettings(root, compatibility.metadata->projectId);
        REQUIRE(surfaces.HasValue());
        REQUIRE(surfaces.Value().settings->Input().defaultSurfaceId.has_value());
        REQUIRE(surfaces.Value().settings->Catalog()->Entries().size() == 1);
        CHECK(surfaces.Value().settings->Catalog()->Contains(*surfaces.Value().settings->Input().defaultSurfaceId));
        CHECK(surfaces.Value().settings->Capture(1).HasValue());
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

    TEST_CASE("Creation retains foreign staging and cleans only its own unpublished tree", "[unit][editor][platform-creation]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        auto files = std::make_shared<CreationFiles>();
        const auto destination = temporary.Path() / "MyGame";
        const auto staging = temporary.Path() / ".MyGame.horo-create-1";
        bool foreign{};
        SECTION("Foreign staging survives") {
            foreign = true;
            REQUIRE(std::filesystem::create_directory(staging));
            std::ofstream(staging / "sentinel") << "foreign";
        }
        SECTION("Durable write fails") {
            files->failWrite = true;
        }
        SECTION("Directory sync fails") {
            files->failSync = true;
        }
        SECTION("Publication fails before commit") {
            files->failBeforeCommit = true;
        }
        Horo::Editor::ProjectCreationService service{jobs, bus, std::make_shared<CreationRandom>(), files};
        const auto started = service.StartCreate(ValidRequest(destination));
        REQUIRE(started.HasValue());
        const auto terminal = WaitForTerminal(service, started.Value().id);
        CHECK(terminal.state == Horo::Editor::ProjectCreationOperationState::Failed);
        CHECK(terminal.publication == Horo::Editor::ProjectCreationPublicationState::Unpublished);
        REQUIRE(terminal.error.has_value());
        CHECK_FALSE(std::filesystem::exists(destination));
        if (foreign)
            CHECK(Read(staging / "sentinel") == "foreign");
        else {
            CHECK_FALSE(std::filesystem::exists(staging));
            REQUIRE(terminal.error->cause.has_value());
            CHECK(terminal.error->cause->message == "injected filesystem failure");
        }
    }

    TEST_CASE("Unexpected creation exceptions leave a terminal unpublished operation and clean owned staging",
              "[unit][editor][platform-creation]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        auto files = std::make_shared<CreationFiles>();
        SECTION("Preparation exception") {
            files->throwDuringWrite = true;
        }
        SECTION("Publication exception before commit") {
            files->throwBeforeCommit = true;
        }
        Horo::Editor::ProjectCreationService service{jobs, bus, std::make_shared<CreationRandom>(), files};
        const auto root = temporary.Path() / "ExceptionProject";
        const auto started = service.StartCreate(ValidRequest(root));
        REQUIRE(started.HasValue());
        const auto terminal = WaitForTerminal(service, started.Value().id);
        CHECK(terminal.state == Horo::Editor::ProjectCreationOperationState::Failed);
        CHECK(terminal.publication == Horo::Editor::ProjectCreationPublicationState::Unpublished);
        REQUIRE(terminal.error.has_value());
        CHECK(terminal.error->code == Horo::Editor::ProjectCreationErrorCode::UnexpectedFailure);
        REQUIRE(terminal.error->cause.has_value());
        CHECK(terminal.error->cause->code.Value() == "project_creation.unexpected_failure");
        CHECK_FALSE(std::filesystem::exists(root));
        CHECK_FALSE(std::filesystem::exists(temporary.Path() / ".ExceptionProject.horo-create-1"));
    }

    TEST_CASE("Post-commit creation failure retains the whole project for reconciliation", "[unit][editor][platform-creation]") {
        TemporaryDirectory temporary;
        Horo::JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::EngineDataBus bus;
        auto files = std::make_shared<CreationFiles>();
        SECTION("Typed post-commit failure") {
            files->failAfterCommit = true;
        }
        SECTION("Throw after commit") {
            files->throwAfterCommit = true;
        }
        SECTION("Allocation exception after commit") {
            files->allocationAfterCommit = true;
        }
        SECTION("Nonstandard exception after commit") {
            files->unknownAfterCommit = true;
        }
        std::atomic<int> createdEvents{};
        const auto subscription = bus.Subscribe<Horo::Editor::ProjectCreatedEvent>([&](const auto &) {
            ++createdEvents;
        });
        Horo::Editor::ProjectCreationService service{jobs, bus, std::make_shared<CreationRandom>(), files};
        const auto destination = temporary.Path() / std::filesystem::path{u8"Project ü 图"};
        const auto started = service.StartCreate(ValidRequest(destination));
        REQUIRE(started.HasValue());
        const auto terminal = WaitForTerminal(service, started.Value().id);
        CHECK(terminal.state == Horo::Editor::ProjectCreationOperationState::Failed);
        CHECK(terminal.publication == Horo::Editor::ProjectCreationPublicationState::ReconciliationRequired);
        REQUIRE(terminal.error.has_value());
        CHECK(terminal.error->code == Horo::Editor::ProjectCreationErrorCode::DurabilityUnconfirmed);
        REQUIRE(terminal.error->cause.has_value());
        CHECK(terminal.error->cause->code.Value() ==
              (files->failAfterCommit ? "security.entropy_unavailable" : "project_creation.unexpected_failure"));
        CHECK(std::filesystem::is_regular_file(destination / ".horo/project.json"));
        for (const auto path :
             {Horo::Application::PlatformServicesDocumentPaths::Ledger, Horo::Application::PlatformServicesDocumentPaths::Achievements,
              Horo::Application::PlatformServicesDocumentPaths::Stats, Horo::Application::PlatformServicesDocumentPaths::Leaderboards,
              Horo::Application::PlatformServicesDocumentPaths::Presence})
            CHECK(std::filesystem::is_regular_file(destination / path));
        CHECK(service.StartCreate(ValidRequest(destination)).HasError());
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
        service.PumpMainThread();
        CHECK(createdEvents.load() == 0);
    }
}  // namespace
