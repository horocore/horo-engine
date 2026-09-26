#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/ExternalProcess.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"
#include "editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPane.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <imgui.h>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace {
    class ActiveBuildLocalization final : public Horo::Editor::ILocalizationService {
    public:
        [[nodiscard]] const std::string &Get(std::string_view, std::string_view key) const override {
            const std::size_t separator = key.rfind('.');
            return labels_.try_emplace(std::string{key}, key.substr(separator == std::string_view::npos ? 0 : separator + 1)).first->second;
        }

    private:
        mutable std::unordered_map<std::string, std::string> labels_;
    };

    class WaitingBuildProcess final : public Horo::IExternalProcessRunner {
    public:
        Horo::Result<Horo::ExternalProcessResult> Run(const Horo::ExternalProcessRequest &,
                                                      const Horo::CancellationToken &cancellation) override {
            {
                std::lock_guard lock(mutex_);
                running_ = true;
            }
            condition_.notify_one();
            while (!cancellation.IsCancellationRequested())
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            return Horo::Result<Horo::ExternalProcessResult>::Success({Horo::ProcessTerminationReason::Cancelled, 1});
        }

        [[nodiscard]] bool WaitUntilRunning() {
            std::unique_lock lock(mutex_);
            return condition_.wait_for(lock, std::chrono::seconds{10}, [this] {
                return running_;
            });
        }

    private:
        std::mutex mutex_;
        std::condition_variable condition_;
        bool running_{};
    };

    [[nodiscard]] ImVec2 DrawBuildPane(Horo::Editor::GlobalDockBuildOutputPane &pane, const Horo::Editor::EditorGuiContext &context,
                                       const float width) {
        using namespace Horo::Editor;
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({width + 24.0F, 360.0F});
        ImGui::Begin("Active build", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const GlobalDockToolbarChipProps cancel{.id = "BuildCancelActive",
                                                .label = context.localization.Get("editor", "workspace.global_dock.build_output.cancel"),
                                                .tone = GlobalDockTone::Warning};
        const float buttonWidth = MeasureGlobalDockToolbarChip(cancel, context.theme.fonts);
        const ImVec2 cancelCenter{origin.x + width - metrics.contentPadding - buttonWidth * 0.5F, origin.y + metrics.toolbarHeight * 1.5F};
        Horo::Editor::EditorWorkspaceViewCommandData command;
        pane.Draw(origin, width, command, context);
        ImGui::End();
        return cancelCenter;
    }

    void ClickCancelButton(Horo::Editor::GlobalDockBuildOutputPane &pane, const Horo::Editor::EditorGuiContext &context, ImGuiIO &io,
                           const ImVec2 center) {
        io.AddMousePosEvent(center.x, center.y);
        io.AddMouseButtonEvent(0, true);
        ImGui::NewFrame();
        static_cast<void>(DrawBuildPane(pane, context, 900.0F));
        ImGui::Render();
        io.AddMouseButtonEvent(0, false);
        ImGui::NewFrame();
        static_cast<void>(DrawBuildPane(pane, context, 900.0F));
        ImGui::Render();
    }

    [[nodiscard]] bool RenderActiveAndCancellingFrames(Horo::Application::GameplayBuildService &builds, Horo::BuildOutputStore &output,
                                                       const std::filesystem::path &project,
                                                       const Horo::Application::GameplayBuildSessionId sessionId,
                                                       std::atomic_bool &releaseWorker, WaitingBuildProcess &processes) {
        using namespace Horo::Editor;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.DisplaySize = {1280.0F, 720.0F};
        io.DeltaTime = 1.0F / 60.0F;
        ImFont *font = io.Fonts->AddFontDefault();
        static_cast<void>(io.Fonts->Build());

        Horo::EngineDataBus engineEvents;
        EditorDataBus editorEvents;
        ActiveBuildLocalization localization;
        const ThemeContext theme{.fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font}};
        const EditorSettingsSnapshot settings{};
        const EditorGuiContext context{.engineEvents = engineEvents,
                                       .editorEvents = editorEvents,
                                       .localization = localization,
                                       .theme = theme,
                                       .settings = settings};
        GlobalDockBuildOutputPane pane;
        pane.Attach(&output, &builds, project.generic_string());

        ImGui::NewFrame();
        static_cast<void>(DrawBuildPane(pane, context, 900.0F));
        ImGui::Render();
        CHECK(ImGui::GetDrawData()->CmdListsCount > 0);
        releaseWorker.store(true);
        const bool running = processes.WaitUntilRunning();
        CHECK(running);

        ImGui::NewFrame();
        const ImVec2 cancelCenter = DrawBuildPane(pane, context, 900.0F);
        ImGui::Render();

        ClickCancelButton(pane, context, io, cancelCenter);
        const auto cancelled = builds.Query(sessionId);
        const bool requested = cancelled.has_value() && cancelled->cancellationRequested;

        ImGui::NewFrame();
        static_cast<void>(DrawBuildPane(pane, context, 260.0F));
        ImGui::Render();
        CHECK(ImGui::GetDrawData()->CmdListsCount > 0);
        pane.Detach();
        ImGui::DestroyContext();
        return requested;
    }
}  // namespace

TEST_CASE("Build Output renders a live build at wide and narrow widths and survives cancellation", "[unit][editor][gui]") {
    using namespace Horo;
    using namespace Horo::Editor;

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path project = std::filesystem::temp_directory_path() / ("horo-active-build-pane-" + std::to_string(nonce));
    REQUIRE(std::filesystem::create_directories(project));

    WaitingBuildProcess processes;
    JobSystem jobs{{1U, 4U}};
    std::atomic_bool workerOccupied{false};
    std::atomic_bool releaseWorker{false};
    auto blocker = jobs.Submit({}, [&](const CancellationToken &cancellation) {
        workerOccupied.store(true);
        while (!releaseWorker.load() && !cancellation.IsCancellationRequested())
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
    });
    REQUIRE(blocker.HasValue());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (!workerOccupied.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    REQUIRE(workerOccupied.load());
    NativeDurableFileSystem files;
    BuildOutputStore output{16};
    Application::GameplayBuildService builds{processes, jobs, files, &output};
    Application::GameplayBuildRequest request{.projectRoot = project, .environment = {.gameplaySdkPackage = project}};
    const auto started = builds.Start(request);
    REQUIRE(started.HasValue());
    const auto queued = builds.QueryActiveProject(project);
    REQUIRE(queued.has_value());
    CHECK(queued->state == Application::GameplayBuildState::Queued);

    CHECK(RenderActiveAndCancellingFrames(builds, output, project, started.Value(), releaseWorker, processes));

    builds.Shutdown();
    REQUIRE_FALSE(builds.QueryActiveProject(project).has_value());
    jobs.Shutdown(ShutdownPolicy::Cancel);
    std::error_code error;
    std::filesystem::remove_all(project, error);
    CHECK_FALSE(error);
}
