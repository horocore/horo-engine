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
#include "editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPane.h"

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

    void DrawBuildPane(Horo::Editor::GlobalDockBuildOutputPane &pane, const Horo::Editor::EditorGuiContext &context, const float width) {
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({width + 24.0F, 360.0F});
        ImGui::Begin("Active build", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        Horo::Editor::EditorWorkspaceViewCommandData command;
        pane.Draw(ImGui::GetCursorScreenPos(), width, command, context);
        ImGui::End();
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
    NativeDurableFileSystem files;
    BuildOutputStore output{16};
    Application::GameplayBuildService builds{processes, jobs, files, &output};
    Application::GameplayBuildRequest request{.projectRoot = project, .environment = {.gameplaySdkPackage = project}};
    const auto started = builds.Start(request);
    REQUIRE(started.HasValue());
    REQUIRE(processes.WaitUntilRunning());
    REQUIRE(builds.QueryActiveProject(project).has_value());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {1280.0F, 720.0F};
    io.DeltaTime = 1.0F / 60.0F;
    ImFont *font = io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());

    EngineDataBus engineEvents;
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
    DrawBuildPane(pane, context, 900.0F);
    ImGui::Render();
    REQUIRE(ImGui::GetDrawData()->CmdListsCount > 0);

    REQUIRE(builds.RequestCancel(started.Value()));
    ImGui::NewFrame();
    DrawBuildPane(pane, context, 260.0F);
    ImGui::Render();
    REQUIRE(ImGui::GetDrawData()->CmdListsCount > 0);

    builds.Shutdown();
    REQUIRE_FALSE(builds.QueryActiveProject(project).has_value());
    pane.Detach();
    ImGui::DestroyContext();
    jobs.Shutdown(ShutdownPolicy::Cancel);
    std::error_code error;
    std::filesystem::remove_all(project, error);
    CHECK_FALSE(error);
}
