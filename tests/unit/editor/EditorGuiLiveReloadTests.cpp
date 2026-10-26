#include "GuiRuntimeBridge.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Editor/SettingsModal.h"
#include "ICompilerLogger.h"
#include "IFileChangeNotifier.h"
#include "RuntimeObjectSystem.h"
#include "editor/screens/welcome/WelcomeView.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPanel.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <imgui_internal.h>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {
    using namespace Horo::Editor;

    struct CompilerLog : ICompilerLogger {
        // Compiler output arrives on RCC++'s reader thread; assertions run after completion.
        std::mutex mutex;
        std::vector<std::string> output;

        void Capture(const char *format, va_list args) {
            std::array<char, 4096> text{};
            std::vsnprintf(text.data(), text.size(), format, args);
            std::lock_guard lock(mutex);
            if (std::strcmp(format, "%s") == 0)
                output.emplace_back(text.data());
        }

        void LogError(const char *format, ...) override {
            va_list args;
            va_start(args, format);
            Capture(format, args);
            va_end(args);
        }

        void LogWarning(const char *format, ...) override {
            va_list args;
            va_start(args, format);
            Capture(format, args);
            va_end(args);
        }

        void LogInfo(const char *format, ...) override {
            va_list args;
            va_start(args, format);
            Capture(format, args);
            va_end(args);
        }
    };

    struct TemporaryBuild {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("horo GUI live-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        TemporaryBuild() {
            std::filesystem::create_directories(path);
        }

        ~TemporaryBuild() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    };

    std::string Read(const std::filesystem::path &path) {
        std::ifstream input{path};
        REQUIRE(input.good());
        std::ostringstream text;
        text << input.rdbuf();
        return text.str();
    }

    void Write(const std::filesystem::path &path, const std::string &text) {
        std::ofstream output{path};
        output << text;
        REQUIRE(output.good());
    }

    void Replace(std::string &text, const std::string &from, const std::string &to) {
        std::size_t offset = 0;
        while ((offset = text.find(from, offset)) != std::string::npos) {
            text.replace(offset, from.size(), to);
            offset += to.size();
        }
    }

    void Compile(RuntimeObjectSystem &runtime, unsigned short project = 1) {
        runtime.CompileAllInProject(true, project);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{300};
        while (!runtime.GetIsCompiledComplete() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        REQUIRE(runtime.GetIsCompiledComplete());
    }

    /** @brief Drives the real watcher without manually selecting compilation inputs. */
    void WaitForFileChangeCompilation(RuntimeObjectSystem &runtime) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{180};
        while (!runtime.GetIsCompiledComplete() && std::chrono::steady_clock::now() < deadline) {
            if (!runtime.GetIsCompiling())
                runtime.GetFileChangeNotifier()->Update(0.02F);
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        REQUIRE(runtime.GetIsCompiledComplete());
    }

    /** @brief Matches the native path literal emitted into the generated adapter. */
    std::string TrackingPath(std::filesystem::path path) {
        std::string text = path.make_preferred().string();
        Replace(text, "\\", "\\\\");
        return text;
    }

    void Configure(RuntimeObjectSystem &runtime, const std::filesystem::path &directory, unsigned short project = 0) {
        runtime.SetIntermediateDir(directory.string().c_str(), project);
        std::ifstream includes{HORO_GUI_LIVE_INCLUDE_FILE};
        std::string include;
        while (std::getline(includes, include))
            if (!include.empty())
                runtime.AddIncludeDir(include.c_str(), project);
        runtime.SetAdditionalCompileOptions("@\"" HORO_GUI_LIVE_FLAGS_FILE "\"", project);
        runtime.SetAdditionalLinkOptions("\"" HORO_GUI_LIVE_HOST_IMPORT "\"", project);
    }

    struct Registry : GuiLive::IRegistry {
        explicit Registry(const SystemTable &table) : system(table) {}

        const SystemTable &system;

        GuiLive::Binding Find(GuiLive::Id route, GuiLive::Id signature) const noexcept override {
            for (const auto *module : system.modules)
                if (module)
                    if (const auto binding = module->Find(route, signature); binding.invoke)
                        return binding;
            return {};
        }
    };

    float Draw(Tests::EditorGuiContextFixture &fixture, WelcomeViewState &state) {
        WelcomeViewModel model;
        model.recentProjects = {{"One", "/one", "", ""}, {"Two", "/two", "", ""}};
        fixture.imgui.BeginFrame();
        REQUIRE(GuiLive::Invoke<&DrawWelcomeView>(fixture.theme.liveReload, model, state, fixture.context, WelcomeViewAssets{},
                                                  GuiContentRegion{0, 0, 1280, 800})
                    .command == WelcomeViewCommand::None);
        fixture.imgui.EndFrame();
        for (auto *window : GImGui->Windows)
            if (std::strstr(window->Name, "ProjectCard"))
                return window->Size.y;
        FAIL("Expected the real project-card child window");
        return 0;
    }

    /** @brief Measures a real shared control through the currently installed runtime module. */
    float SharedControlHeight(Tests::EditorGuiContextFixture &fixture, const Registry &registry) {
        const auto draw = GuiLive::Find<float()>(&registry, GuiLive::Id{"SharedControls"});
        REQUIRE(static_cast<bool>(draw));
        fixture.imgui.BeginFrame();
        ImGui::Begin("Shared component test");
        const float height = draw();
        ImGui::End();
        fixture.imgui.EndFrame();
        return height;
    }
}  // namespace

TEST_CASE("Registered GUI modules share one host and retain state across partial reload, failure and recovery",
          "[editor][live-ui][native]") {
    TemporaryBuild build;
    CompilerLog logger;
    SystemTable system;
    RuntimeObjectSystem runtime;
    Development::LinkGuiModules();
    REQUIRE(runtime.Initialise(&logger, &system));
    runtime.GetFileChangeNotifier()->SetMonitoringActive(false);
    Registry registry{system};
    // RCC++ owns process-global constructor metadata. Match the editor's single-runtime lifetime.
    Configure(runtime, build.path);
    Compile(runtime, 0);
    REQUIRE(runtime.LoadCompiledModule());
    {
        std::lock_guard lock(logger.mutex);
        REQUIRE_FALSE(logger.output.empty());
        for (const auto &line : logger.output) {
            INFO(line);
            CHECK_FALSE(line.empty());
            CHECK(line.find_first_of("\r\n") == std::string::npos);
            CHECK(line.find("warning D9025") == std::string::npos);
        }
    }
    REQUIRE(static_cast<bool>(GuiLive::Find<decltype(DrawWelcomeView)>(&registry, GuiLive::EntryId<&DrawWelcomeView>())));
    REQUIRE(static_cast<bool>(
        GuiLive::Find<GuiLive::MethodSignature<&IWorkspacePanel::DrawPanel>>(&registry, GuiLive::TypeId(typeid(GlobalDockPanel)))));
    REQUIRE(
        static_cast<bool>(GuiLive::Find<GuiLive::MethodSignature<&EditorModal::Draw>>(&registry, GuiLive::TypeId(typeid(SettingsModal)))));
    Tests::EditorGuiContextFixture fixture;
    fixture.theme.liveReload = &registry;
    WelcomeViewState state;
    state.search[0] = 'T';
    state.sortSelection = 1;
    REQUIRE(Draw(fixture, state) == 76.0F);
    const auto source = build.path / "WelcomeView.cpp";
    const auto component = build.path / "EditorUiComponents.cpp";
    auto componentDrawing = Read(HORO_TEST_SHARED_UI_SOURCE);
    Write(component, componentDrawing);
    auto drawing = Read(HORO_TEST_WELCOME_SOURCE);
    Write(source, drawing);
    auto wrapper = Read(HORO_TEST_WELCOME_WRAPPER);
    Replace(wrapper, TrackingPath(HORO_TEST_WELCOME_SOURCE), TrackingPath(source));
    Replace(wrapper, TrackingPath(HORO_TEST_SHARED_UI_SOURCE), TrackingPath(component));
    wrapper += R"(
#include "Horo/Editor/EditorUiComponents.h"
class HoroGuiSharedProbe : public HoroGuiModule {
public:
    void Init(bool) override { PerModuleInterface::g_pSystemTable->modules.back() = this; }
    void Serialize(ISimpleSerializer *) override {}
    Horo::Editor::GuiLive::Binding Find(Horo::Editor::GuiLive::Id route, Horo::Editor::GuiLive::Id signature) const noexcept override {
        using namespace Horo::Editor;
        if (route != GuiLive::Id{"SharedControls"}) return {};
        return GuiLive::Adapter<float(), [] {
            static_cast<void>(Ui::Button({.label = "Shared control", .componentSize = Ui::ComponentSize::Medium}));
            return ImGui::GetItemRectSize().y;
        }>::Bind(signature);
    }
};
REGISTERSINGLETON(HoroGuiSharedProbe, true);
)";
    const auto module = build.path / "GuiLive_Welcome.cpp";
    Write(module, wrapper);
    Configure(runtime, build.path / "changed", 1);
    runtime.AddToRuntimeFileList(module.string().c_str(), 1);
    Compile(runtime);
    REQUIRE(runtime.LoadCompiledModule());
    REQUIRE(Draw(fixture, state) == 76.0F);
    runtime.GetFileChangeNotifier()->SetMonitoringActive(true);
    runtime.GetFileChangeNotifier()->Update(2.0F);
    Replace(drawing, "tokens.spacing.cardPadding", "(tokens.spacing.cardPadding + 12.0F)");
    Write(source, drawing);
    WaitForFileChangeCompilation(runtime);
    REQUIRE(runtime.LoadCompiledModule());
    REQUIRE(Draw(fixture, state) == 100.0F);
    REQUIRE(std::string_view{state.search.data()} == "T");
    REQUIRE(state.sortSelection == 1);
    REQUIRE(state.visibleProjectIndices == std::vector<std::size_t>{1});
    runtime.GetFileChangeNotifier()->Update(2.0F);
    Write(source, "#error deliberate invalid GUI candidate\n");
    WaitForFileChangeCompilation(runtime);
    REQUIRE_FALSE(runtime.LoadCompiledModule());
    REQUIRE(Draw(fixture, state) == 100.0F);
    Replace(drawing, "+ 12.0F", "+ 6.0F");
    runtime.GetFileChangeNotifier()->Update(2.0F);
    Write(source, drawing);
    WaitForFileChangeCompilation(runtime);
    REQUIRE(runtime.LoadCompiledModule());
    REQUIRE(Draw(fixture, state) == 88.0F);
    REQUIRE(std::string_view{state.search.data()} == "T");
    REQUIRE(SharedControlHeight(fixture, registry) == 32.0F);
    Replace(componentDrawing, "actualSize.y = resolvedStyle.metrics.minimumHeight;",
            "actualSize.y = resolvedStyle.metrics.minimumHeight + 6.0F;");
    runtime.GetFileChangeNotifier()->Update(2.0F);
    Write(component, componentDrawing);
    WaitForFileChangeCompilation(runtime);
    REQUIRE(runtime.LoadCompiledModule());
    REQUIRE(SharedControlHeight(fixture, registry) == 38.0F);
    REQUIRE(Draw(fixture, state) == 88.0F);
    REQUIRE(std::string_view{state.search.data()} == "T");
    REQUIRE(state.sortSelection == 1);
    runtime.GetFileChangeNotifier()->SetMonitoringActive(false);
    runtime.CleanObjectFiles();
}
