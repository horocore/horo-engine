#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Platform/PlatformErrors.h"
#include "ProjectLanguageWorkspace.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::Language;
    using namespace std::chrono_literals;

    /** @brief Counts native runs across independent owned workers. */
    struct CountingRunner final : IExternalProcessRunner {
        Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &cancellation) override {
            ++launches;
            return native.Run(request, cancellation);
        }

        NativeExternalProcessRunner native;
        std::atomic_uint32_t launches{};
    };

    /** @brief Owns fixture source files and the process runner for a project language workspace. */
    struct LanguageProject final {
        TestSupport::SourceProject project;
        SourceFileOpenService files{project.directory};
        CountingRunner runner;

        SourceDocumentSnapshot Open(const char *path) {
            project.Put("int token;", path);
            auto opened = files.Open({.path = path});
            REQUIRE(opened.HasValue());
            REQUIRE(opened.Value().sourceSnapshot.has_value());
            return *opened.Value().sourceSnapshot;
        }

        std::unique_ptr<ProjectLanguageWorkspace> Create(const std::span<const ProjectLanguageProvider> providers) {
            auto workspace = ProjectLanguageWorkspace::Create(runner, files, providers);
            REQUIRE(workspace.HasValue());
            return std::move(workspace).Value();
        }
    };

    /** @brief Selects the real child path only through a host descriptor. */
    ProjectLanguageProvider Provider() {
        return {.id = "cpp", .languageId = "cpp", .extensions = {".CPP", ".h"}, .executable = HORO_LANGUAGE_SERVER_CHILD, .arguments = {}};
    }

    /** @brief Drives normal workspace polling until the exact condition or the local deadline. */
    template <typename Step> bool Await(Step step) {
        const auto deadline = ProjectLanguageWorkspace::Clock::now() + 5s;
        while (ProjectLanguageWorkspace::Clock::now() < deadline) {
            if (step(ProjectLanguageWorkspace::Clock::now()))
                return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }

    /** @brief Retires the project worker through the same poll path used at project switch. */
    void Retire(ProjectLanguageWorkspace &workspace) {
        workspace.Shutdown(ProjectLanguageWorkspace::Clock::now());
        REQUIRE(Await([&](const auto now) {
            static_cast<void>(workspace.Poll({}, now));
            return workspace.Finished();
        }));
    }

    TEST_CASE("Project language workspace lazily shares one native provider across matching sources") {
        LanguageProject project;
        const std::array providers{Provider()};
        auto workspace = project.Create(providers);
        CHECK(workspace->Poll({}, ProjectLanguageWorkspace::Clock::now()).empty());
        CHECK(project.runner.launches == 0);
        const auto first = project.Open("first.cpp");
        const auto second = project.Open("second.h");
        const std::array sources{first, second};
        std::vector<DocumentInstanceId> routed;
        REQUIRE(Await([&](const auto now) {
            for (const auto &event : workspace->Poll(sources, now)) {
                REQUIRE(std::holds_alternative<LanguageDiagnostics>(event.payload));
                routed.push_back(event.context.source.Identity().instance);
            }
            return routed.size() == sources.size();
        }));
        CHECK(routed[0] == first.Identity().instance);
        CHECK(routed[1] == second.Identity().instance);
        CHECK(project.runner.launches == 1);
        const auto requested = workspace->Request(second, SourceLanguageRequestKind::Hover);
        REQUIRE(requested.HasValue());
        REQUIRE(Await([&](const auto now) {
            for (const auto &event : workspace->Poll(sources, now)) {
                CHECK(event.context.source.SharesStateWith(second));
                CHECK(std::get<LanguageHover>(event.payload).text == "hover fixture");
                return true;
            }
            return false;
        }));
        Retire(*workspace);
        CHECK(workspace->ProviderError("cpp") == nullptr);
        CHECK(workspace->Request(first, SourceLanguageRequestKind::Hover).HasError());
    }

    TEST_CASE("Project language workspace isolates unavailable providers and permits explicit restart") {
        LanguageProject project;
        const auto first = project.Open("first.cpp");
        const auto script = project.Open("other.lua");
        const std::array sources{first, script};
        auto unavailable = Provider();
        unavailable.id = "script";
        unavailable.languageId = "lua";
        unavailable.extensions = {".lua"};
        unavailable.executable = project.project.directory / "missing-language-server";
        const std::array providers{Provider(), unavailable};
        auto workspace = project.Create(providers);
        bool cppReady{};
        REQUIRE(Await([&](const auto now) {
            for (const auto &event : workspace->Poll(sources, now))
                cppReady = cppReady || event.context.source.SharesStateWith(first);
            return cppReady && workspace->ProviderError("script") != nullptr;
        }));
        CHECK(workspace->ProviderError("script")->code.Value() == PlatformErrors::ProcessLaunchFailed.code.Value());
        CHECK(workspace->ProviderError("cpp") == nullptr);
        REQUIRE(project.runner.launches == 2);

        REQUIRE(workspace->Restart("cpp", ProjectLanguageWorkspace::Clock::now()));
        REQUIRE(Await([&](const auto now) {
            const auto events = workspace->Poll(sources, now);
            return project.runner.launches == 3 && !events.empty();
        }));
        CHECK(workspace->Request(first, SourceLanguageRequestKind::Hover).HasValue());
        CHECK(workspace->Request(script, SourceLanguageRequestKind::Hover).HasError());
        CHECK(project.files.Documents().Edit(script.Identity().instance, {script.Revision(), 0, 3, "local"}).HasValue());
        Retire(*workspace);
    }

    TEST_CASE("Old project language handles and snapshots cannot act on the replacement project") {
        LanguageProject firstProject;
        LanguageProject nextProject;
        const std::array providers{Provider()};
        const std::array firstDocuments{firstProject.Open("same.cpp")};
        const std::array nextDocuments{nextProject.Open("same.cpp")};
        auto first = firstProject.Create(providers);
        auto next = nextProject.Create(providers);
        REQUIRE(Await([&](const auto now) {
            return !first->Poll(firstDocuments, now).empty();
        }));
        auto oldRequest = first->Request(firstDocuments.front(), SourceLanguageRequestKind::Hover);
        REQUIRE(oldRequest.HasValue());
        Retire(*first);

        CHECK(next->Poll(firstDocuments, ProjectLanguageWorkspace::Clock::now()).empty());
        CHECK(nextProject.runner.launches == 0);
        REQUIRE(Await([&](const auto now) {
            return !next->Poll(nextDocuments, now).empty();
        }));
        auto liveRequest = next->Request(nextDocuments.front(), SourceLanguageRequestKind::Hover);
        REQUIRE(liveRequest.HasValue());
        CHECK(liveRequest.Value().request == oldRequest.Value().request);
        CHECK(next->Request(firstDocuments.front(), SourceLanguageRequestKind::Hover).HasError());
        REQUIRE(next->Cancel(oldRequest.Value()).HasValue());
        REQUIRE(Await([&](const auto now) {
            for (const auto &event : next->Poll(nextDocuments, now)) {
                CHECK(event.context.source.SharesStateWith(nextDocuments.front()));
                CHECK(std::holds_alternative<LanguageHover>(event.payload));
                return true;
            }
            return false;
        }));
        Retire(*next);
    }

    TEST_CASE("Project language workspace rejects ambiguous routing and project-relative executables before launch") {
        LanguageProject project;
        auto conflicting = Provider();
        conflicting.id = "another";
        conflicting.extensions = {".cpp"};
        const std::array providers{Provider(), conflicting};
        CHECK(ProjectLanguageWorkspace::Create(project.runner, project.files, providers).HasError());
        auto relative = Provider();
        relative.executable = "language-server";
        CHECK(ProjectLanguageWorkspace::Create(project.runner, project.files, std::span{&relative, 1}).HasError());
        CHECK(project.runner.launches == 0);
    }
}  // namespace
