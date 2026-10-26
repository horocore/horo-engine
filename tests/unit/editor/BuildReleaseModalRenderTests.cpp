#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/ExternalProcess.h"
#include "editor/modals/build_release/BuildActivity.h"
#include "editor/modals/build_release/BuildModal.h"
#include "editor/modals/build_release/BuildReleaseModal.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <condition_variable>
#include <imgui_internal.h>
#include <limits>
#include <mutex>
#include <thread>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    class HeldBuildProcess final : public IExternalProcessRunner {
    public:
        Result<ExternalProcessResult> Run(const ExternalProcessRequest &, const CancellationToken &cancellation) override {
            std::unique_lock lock{mutex_};
            entered_ = true;
            changed_.notify_all();
            while (!released_ && !cancellation.IsCancellationRequested())
                changed_.wait_for(lock, std::chrono::milliseconds{10});
            return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, 1});
        }

        bool WaitUntilRunning() {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(lock, std::chrono::seconds{5}, [this] {
                return entered_;
            });
        }

        void Finish() {
            std::lock_guard lock{mutex_};
            released_ = true;
            changed_.notify_all();
        }

    private:
        std::mutex mutex_;
        std::condition_variable changed_;
        bool entered_{}, released_{};
    };

    struct BuildActivityFixture {
        BuildActivityFixture() : service{process, jobs, files, &output} {
            root = std::filesystem::temp_directory_path() /
                   ("horo build modal " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(root);
        }

        ~BuildActivityFixture() {
            process.Finish();
            service.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Cancel);
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        Application::GameplayBuildRequest Request() const {
            return {.projectRoot = root};
        }

        std::filesystem::path root;
        HeldBuildProcess process;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
        NativeDurableFileSystem files;
        BuildOutputStore output{64};
        Application::GameplayBuildService service;
    };

    struct BuildReleaseHarness {
        BuildReleaseHarness()
            : localization(LocaleTag{"en-US"}), theme{fonts}, context{.engineEvents = engineEvents,
                                                                      .editorEvents = editorEvents,
                                                                      .localization = localization,
                                                                      .theme = theme,
                                                                      .settings = settings},
              host(editorEvents, input) {
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = {320, 600};
            io.DeltaTime = 1.0F / 60;
            auto *font = io.Fonts->AddFontDefault();
            REQUIRE(io.Fonts->Build());
            fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font};
            for (const auto *locale : {"en-US", "tr-TR"})
                REQUIRE(localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                     (std::string{locale} + ".json")));
        }

        ~BuildReleaseHarness() {
            host.ForceDetachAllForShutdown();
            ImGui::DestroyContext();
        }

        void Draw() {
            ImGui::NewFrame();
            host.Draw();
            ImGui::Render();
        }

        EngineDataBus engineEvents;
        EditorDataBus editorEvents;
        LocalizationService localization;
        Theme::Fonts fonts;
        ThemeContext theme;
        EditorSettingsSnapshot settings;
        EditorGuiContext context;
        Input::InputRouter input;
        EditorModalHost host;
        std::optional<std::uint64_t> buildId;
    };

    class ObservedReleaseJobs final : public IBuildReleaseJobs {
    public:
        std::vector<Release::ReleaseJobSnapshot> snapshots;
        int submissions{};
        int cancellations{};

        std::vector<Release::ReleaseJobSnapshot> List() const override {
            return snapshots;
        }

        std::optional<Release::ReleaseDiagnostic> Diagnostic(Release::ReleaseJobId, Release::ReleaseDiagnosticId) const override {
            return std::nullopt;
        }

        Result<Release::ReleaseJobId> Submit(Release::ReleaseExecutionPlan) override {
            ++submissions;
            return Result<Release::ReleaseJobId>::Success({1});
        }

        Result<void> RequestCancel(Release::ReleaseJobId) override {
            ++cancellations;
            return Result<void>::Success();
        }
    };
}  // namespace

TEST_CASE("Release request form and review fit localized modals without starting work", "[editor][gui][build_release]") {
    BuildReleaseHarness harness;
    for (const auto *locale : {"en-US", "tr-TR"}) {
        REQUIRE(harness.localization.Prepare(LocaleTag{locale}));
        REQUIRE(harness.localization.ActivatePrepared());
        for (const auto *key : {"title", "tab_configuration", "tab_build", "tab_packages", "tab_distribution", "tab_output", "tab_logs",
                                "stage_validate", "stage_package", "stage_publish", "draft_hint"})
            CHECK_FALSE(harness.localization.Get("editor", std::string{"workspace.build_release."} + key).starts_with("[missing:"));
        for (const auto size : {ImVec2{320, 600}, ImVec2{1200, 900}}) {
            ImGui::GetIO().DisplaySize = size;
            for (const auto page : {BuildReleasePage::Build, BuildReleasePage::Release}) {
                for (int tab = 0; tab < 2; ++tab) {
                    INFO("locale=" << locale << ", width=" << size.x << ", tab=" << tab);
                    REQUIRE(harness.host
                                .OpenRoot(std::make_unique<BuildReleaseModal>(harness.context, page, Application::GameplayBuildRequest{},
                                                                              nullptr, harness.buildId, nullptr,
                                                                              static_cast<BuildReleaseTab>(tab)))
                                .HasValue());
                    CHECK(harness.host.InteractionScope().kind == EditorInteractionScopeKind::Modal);
                    harness.Draw();
                    harness.Draw();
                    bool found{};
                    bool foundContent{};
                    for (auto *window : ImGui::GetCurrentContext()->Windows) {
                        const std::string_view name{window->Name};
                        if (!window->Active || name.find("BuildReleaseWorkflow") == std::string_view::npos)
                            continue;
                        INFO("window=" << name);
                        CHECK(window->Size.x <= size.x);
                        CHECK(name.find("PrepareReleaseSummary") == std::string_view::npos);
                        if (name.find("ReleaseReviewDetails") != std::string_view::npos)
                            CHECK((window->ChildFlags & ImGuiChildFlags_Borders) == 0);
                        if (name.find("BuildReleaseTabs") != std::string_view::npos)
                            FAIL("Release preparation must use a sectioned form and review, not editor tabs");
                        if (name.find("PrepareReleaseContent") != std::string_view::npos) {
                            foundContent = true;
                            if (tab == 0 && size.x >= 1200 && name.substr(name.rfind('/') + 1).starts_with("##PrepareReleaseContent_")) {
                                for (const auto *id : {"##ReleaseIdentity", "##ReleaseSource"}) {
                                    const auto *table = ImGui::GetCurrentContext()->Tables.GetByKey(window->GetID(id));
                                    REQUIRE(table != nullptr);
                                    REQUIRE(table->ColumnsCount == 2);
                                    std::array<float, 2> top{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
                                    std::array<float, 2> bottom{};
                                    for (const auto &vertex : window->DrawList->VtxBuffer) {
                                        if (vertex.col != Theme::U32(Theme::Bg3()) || !table->OuterRect.Contains(vertex.pos))
                                            continue;
                                        const int column = vertex.pos.x < table->Columns[1].MinX ? 0 : 1;
                                        top[column] = std::min(top[column], vertex.pos.y);
                                        bottom[column] = std::max(bottom[column], vertex.pos.y);
                                    }
                                    REQUIRE(bottom[0] > top[0]);
                                    REQUIRE(bottom[1] > top[1]);
                                    CHECK(std::abs(top[0] - top[1]) < 0.5F);
                                    CHECK(std::abs(bottom[0] - bottom[1]) < 0.5F);
                                }
                            }
                        }
                        CHECK(window->ScrollMax.x == 0);
                        found = true;
                    }
                    CHECK(found);
                    CHECK(foundContent);
                    CHECK_FALSE(harness.buildId.has_value());
                    REQUIRE(harness.host.RequestClose(*harness.host.TopModalId(), ModalCloseReason::Cancelled).HasValue());
                    harness.host.OnUpdate(0);
                    CHECK_FALSE(harness.host.HasOpenModal());
                }
            }
        }
    }
}

TEST_CASE("Release modal open refresh close and reopen never submit or cancel service jobs", "[editor][gui][build_release]") {
    ObservedReleaseJobs jobs;
    jobs.snapshots.push_back({.id = {42}, .state = Release::ReleaseJobState::Running});
    BuildReleaseHarness harness;
    REQUIRE(harness.localization.Prepare(LocaleTag{"en-US"}));
    REQUIRE(harness.localization.ActivatePrepared());
    for (int attempt = 0; attempt < 2; ++attempt) {
        REQUIRE(harness.host
                    .OpenRoot(std::make_unique<BuildReleaseModal>(harness.context, BuildReleasePage::Release,
                                                                  Application::GameplayBuildRequest{}, nullptr, harness.buildId, &jobs))
                    .HasValue());
        harness.host.OnUpdate(1);
        harness.Draw();
        REQUIRE(harness.host.RequestClose(*harness.host.TopModalId(), ModalCloseReason::Cancelled).HasValue());
        harness.host.OnUpdate(0);
        CHECK(jobs.submissions == 0);
        CHECK(jobs.cancellations == 0);
        CHECK(jobs.snapshots.front().state == Release::ReleaseJobState::Running);
    }
}

TEST_CASE("Reviewing a release request never submits an unvalidated execution plan", "[editor][gui][build_release]") {
    ObservedReleaseJobs jobs;
    BuildReleaseHarness harness;
    ImGui::GetIO().DisplaySize = {1200, 900};
    REQUIRE(harness.host
                .OpenRoot(std::make_unique<BuildReleaseModal>(harness.context, BuildReleasePage::Release,
                                                              Application::GameplayBuildRequest{}, nullptr, harness.buildId, &jobs))
                .HasValue());
    harness.Draw();
    harness.Draw();
    const auto activeWindow = [](std::string_view suffix) -> ImGuiWindow * {
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::string_view{window->Name}.find("BuildReleaseWorkflow") != std::string_view::npos &&
                std::string_view{window->Name}.find(suffix) != std::string_view::npos)
                return window;
        return nullptr;
    };
    const auto clickPrimary = [&] {
        const auto *footer = activeWindow("##ModalFooter");
        REQUIRE(footer != nullptr);
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(footer->WorkRect.Max.x - 10, footer->WorkRect.Min.y + 16);
        io.AddMouseButtonEvent(0, true);
        harness.Draw();
        io.AddMouseButtonEvent(0, false);
        harness.Draw();
        harness.Draw();
    };
    REQUIRE(activeWindow("##ReleaseGates") != nullptr);
    clickPrimary();
    REQUIRE(activeWindow("##ReleaseReviewDetails") != nullptr);
    CHECK(activeWindow("##ReleaseGates") == nullptr);
    clickPrimary();
    CHECK(harness.host.HasOpenModal());
    CHECK(jobs.submissions == 0);
    CHECK(jobs.cancellations == 0);
}

TEST_CASE("Build request form is distinct from release tabs and fits localized narrow and wide windows", "[editor][gui][build_release]") {
    BuildReleaseHarness harness;
    for (const auto *locale : {"en-US", "tr-TR"}) {
        REQUIRE(harness.localization.Prepare(LocaleTag{locale}));
        REQUIRE(harness.localization.ActivatePrepared());
        for (const auto size : {ImVec2{320, 600}, ImVec2{1200, 900}}) {
            ImGui::GetIO().DisplaySize = size;
            REQUIRE(
                harness.host
                    .OpenRoot(std::make_unique<BuildModal>(harness.context, Application::GameplayBuildRequest{}, nullptr, harness.buildId))
                    .HasValue());
            harness.Draw();
            harness.Draw();
            bool found{};
            for (auto *window : ImGui::GetCurrentContext()->Windows) {
                const std::string_view name{window->Name};
                if (!window->Active || name.find("BuildWorkflow") == std::string_view::npos)
                    continue;
                INFO("locale=" << locale << ", window=" << name);
                CHECK(window->Size.x <= size.x);
                CHECK(window->ScrollMax.x == 0);
                CHECK(name.find("BuildReleaseTabs") == std::string_view::npos);
                found = true;
            }
            CHECK(found);
            CHECK_FALSE(harness.buildId.has_value());
            REQUIRE(harness.host.RequestClose(*harness.host.TopModalId(), ModalCloseReason::Cancelled).HasValue());
            harness.host.OnUpdate(0);
        }
    }
}

TEST_CASE("Background build status follows real lifecycle and progress without owning the job", "[editor][gui][build_release]") {
    BuildReleaseHarness harness;
    REQUIRE(harness.localization.Prepare(LocaleTag{"en-US"}));
    REQUIRE(harness.localization.ActivatePrepared());
    Application::GameplayBuildSnapshot snapshot{.state = Application::GameplayBuildState::Building, .progress = 0.38F};
    CHECK_FALSE(BackgroundBuildStatus(&snapshot, harness.localization, true).available);
    CHECK_FALSE(BackgroundBuildStatus(nullptr, harness.localization, false).available);
    const auto active = BackgroundBuildStatus(&snapshot, harness.localization, false);
    CHECK(active.available);
    CHECK(active.iconResourceId == Ui::BusyStatusIcon);
    CHECK(active.value == "38%");
    CHECK(active.label == harness.localization.Get("editor", "workspace.build_release.building"));
    snapshot.progress.reset();
    CHECK(BackgroundBuildStatus(&snapshot, harness.localization, false).value.empty());
    snapshot.progress = std::numeric_limits<float>::quiet_NaN();
    CHECK(BackgroundBuildStatus(&snapshot, harness.localization, false).value.empty());
    for (const auto state : {Application::GameplayBuildState::Succeeded, Application::GameplayBuildState::Failed,
                             Application::GameplayBuildState::Cancelled, Application::GameplayBuildState::TimedOut}) {
        snapshot.state = state;
        CHECK_FALSE(BackgroundBuildStatus(&snapshot, harness.localization, false).available);
    }
}

TEST_CASE("Active and failed build footers keep fonts balanced across child windows", "[editor][gui][build_release][build_activity]") {
    std::vector<std::string> errors;
    BuildActivityFixture fixture;
    BuildReleaseHarness harness;
    // Turn recoverable ImGui assertions into explicit test failures instead of a CRT dialog.
    auto *gui = ImGui::GetCurrentContext();
    ImGui::GetIO().ConfigErrorRecoveryEnableAssert = false;
    gui->ErrorCallbackUserData = &errors;
    gui->ErrorCallback = [](ImGuiContext *, void *target, const char *message) {
        static_cast<std::vector<std::string> *>(target)->emplace_back(message);
    };
    const auto started = fixture.service.Start(fixture.Request());
    REQUIRE(started.HasValue());
    REQUIRE(fixture.process.WaitUntilRunning());
    harness.buildId = started.Value();
    for (const bool running : {true, false}) {
        if (!running) {
            fixture.process.Finish();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (BuildIsRunning(fixture.service.Query(started.Value())->state) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            REQUIRE(fixture.service.Query(started.Value())->state == Application::GameplayBuildState::Failed);
        }
        for (const auto *locale : {"en-US", "tr-TR"}) {
            REQUIRE(harness.localization.Prepare(LocaleTag{locale}));
            REQUIRE(harness.localization.ActivatePrepared());
            for (const auto size : {ImVec2{320, 600}, ImVec2{1200, 900}}) {
                ImGui::GetIO().DisplaySize = size;
                REQUIRE(harness.host
                            .OpenRoot(std::make_unique<BuildModal>(harness.context, fixture.Request(), &fixture.service, harness.buildId,
                                                                   &fixture.output))
                            .HasValue());
                for (int frame = 0; frame < 3; ++frame) {
                    harness.Draw();
                    INFO("running=" << running << ", locale=" << locale << ", width=" << size.x);
                    INFO((errors.empty() ? "No ImGui errors" : errors.front()));
                    CHECK(errors.empty());
                    CHECK(gui->FontStack.Size == 0);
                }
                bool logs{};
                for (const auto *window : gui->Windows) {
                    if (!window->Active || std::string_view{window->Name}.find("BuildWorkflow") == std::string_view::npos)
                        continue;
                    INFO(window->Name);
                    if (std::string_view{window->Name}.find("BuildStages") != std::string_view::npos)
                        CHECK(window->ScrollMax.y == 0);
                    if (std::string_view{window->Name}.find("BuildLogArea") != std::string_view::npos)
                        logs = true;
                    else
                        CHECK((window->Flags & ImGuiWindowFlags_AlwaysVerticalScrollbar) == 0);
                }
                CHECK(logs);
                REQUIRE(harness.host.RequestClose(*harness.host.TopModalId(), ModalCloseReason::Cancelled).HasValue());
                harness.host.OnUpdate(0);
                const auto snapshot = fixture.service.Query(started.Value());
                REQUIRE(snapshot.has_value());
                CHECK_FALSE(snapshot->cancellationRequested);
                CHECK(BackgroundBuildStatus(&*snapshot, harness.localization, false).available == running);
            }
        }
    }
}
