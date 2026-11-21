#include "../../support/project/ProjectCreationFixture.h"

namespace {
    using namespace Horo::Tests::ProjectCreation;

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
