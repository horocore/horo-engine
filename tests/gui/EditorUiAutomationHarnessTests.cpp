#include "EditorUiTestHarness.h"
#include "Horo/Runtime/Render/RenderFrontend.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_test_engine/imgui_te_context.h>
#include <memory>
#include <stdexcept>

namespace {
    struct TrackingSurfaceState {
        int initializeCount{0};
        int beginFrameCount{0};
        int presentCount{0};
        int shutdownCount{0};
    };

    class TrackingSurface final : public Horo::Tests::IEditorUiTestSurface {
    public:
        explicit TrackingSurface(std::shared_ptr<TrackingSurfaceState> state) : state_(std::move(state)) {}

        void Initialize(ImGuiContext &context) override {
            ++state_->initializeCount;
            ImGui::SetCurrentContext(&context);
            ImGuiIO &io = ImGui::GetIO();
            io.DisplaySize = {640.0F, 480.0F};
            io.DisplayFramebufferScale = {1.0F, 1.0F};
            io.DeltaTime = 1.0F / 60.0F;
        }

        [[nodiscard]] bool BeginFrame() override {
            ++state_->beginFrameCount;
            return true;
        }

        void Present() override {
            ++state_->presentCount;
        }

        void Shutdown() noexcept override {
            ++state_->shutdownCount;
        }

        [[nodiscard]] bool IsInteractive() const noexcept override {
            return false;
        }

        [[nodiscard]] Horo::Editor::IEditorViewportRenderer &ViewportRenderer() noexcept override {
            return viewportRenderer_;
        }

        void RenderViewport(const Horo::Editor::EditorViewportSceneView) override {}

        [[nodiscard]] std::string_view RendererName() const noexcept override {
            return "tracking";
        }

    private:
        class TrackingViewportRenderer final : public Horo::Editor::IEditorViewportRenderer {
        public:
            void RequestExtent(Horo::Editor::EditorViewportExtent) noexcept override {}

            void RequestGrid(const Horo::Editor::EditorViewportGridOptions &) noexcept override {}

            void RequestLightVisualizer(const Horo::Editor::EditorViewportLightVisualizerOptions &) noexcept override {}

            [[nodiscard]] Horo::Editor::EditorViewportExtent RequestedExtent() const noexcept override {
                return {};
            }

            [[nodiscard]] Horo::Math::ClipDepthRange ClipDepthRange() const noexcept override {
                return Horo::Math::ClipDepthRange::NegativeOneToOne;
            }

            [[nodiscard]] Horo::Result<void> ExecutePreparedFrame(Horo::Render::RenderFrameScope &frame,
                                                                  std::span<const Horo::Render::RenderPassDescriptor> passes) override {
                return frame.Execute(passes);
            }

            [[nodiscard]] Horo::Result<void> ExecuteStaticMeshPass(const Horo::Render::StaticMeshPassDescriptor &) override {
                return Horo::Result<void>::Success();
            }

            [[nodiscard]] Horo::Editor::EditorViewportTextureView TextureView() const noexcept override {
                return {};
            }

            [[nodiscard]] bool IsReady() const noexcept override {
                return false;
            }
        } viewportRenderer_;

        std::shared_ptr<TrackingSurfaceState> state_;
    };

    void CheckScenarioSteps(const Horo::Tests::EditorUiScenarioResult &result) {
        REQUIRE_FALSE(result.steps.empty());
        for (const Horo::Tests::UiScenarioStepResult &step : result.steps) {
            INFO("UI step: " << step.name);
            INFO("frames: " << step.firstFrame << ".." << step.lastFrame);
            INFO("diagnostic: " << step.diagnostic);
            CHECK(step.status == Horo::Tests::UiScenarioStepStatus::Passed);
        }
    }
}  // namespace

TEST_CASE("UI harness owns an injected presentation surface lifecycle", "[ui][imgui][editor][contract]") {
    const auto state = std::make_shared<TrackingSurfaceState>();
    {
        Horo::Tests::EditorUiTestHarness harness{std::make_unique<TrackingSurface>(state)};
        const auto result = harness.Run("horo_harness", "injected_surface", [](ImGuiTestContext *) {
        }, [](ImGuiTestContext *context) {
            context->Yield(2);
        });

        REQUIRE(result.Succeeded());
        REQUIRE(state->initializeCount == 1);
        REQUIRE(state->beginFrameCount > 0);
        REQUIRE(state->presentCount == state->beginFrameCount);
        REQUIRE(state->shutdownCount == 0);
    }
    REQUIRE(state->shutdownCount == 1);
}

TEST_CASE("Basic Dear ImGui widgets execute as named child test steps", "[ui][imgui][editor][contract]") {
    struct AppState {
        bool buttonClicked{false};
        bool checkboxChecked{false};
        int sliderValue{0};
        std::array<char, 32> text{};
        bool aboutVisible{false};
    } app;

    Horo::Tests::EditorUiTestHarness harness{Horo::Tests::CreateHeadlessEditorUiTestSurface()};
    const auto result = harness.RunScenario("horo_harness", "basic_widget_pipeline", [&](ImGuiTestContext *) {
        ImGui::Begin("My Window", nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoSavedSettings);
        app.buttonClicked = ImGui::Button("My Button") || app.buttonClicked;
        if (ImGui::TreeNodeEx("Node", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Checkbox", &app.checkboxChecked);
            ImGui::TreePop();
        }
        ImGui::SliderInt("Slider", &app.sliderValue, 0, 200);
        ImGui::InputText("Text", app.text.data(), app.text.size());
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Tools")) {
                ImGui::MenuItem("About Dear ImGui", nullptr, &app.aboutVisible);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::End();
    }, [&](Horo::Tests::UiScenarioPipe &scenario) {
        scenario.SetRef("My Window")
            .Step("Click action button",
                  [](ImGuiTestContext &ui) {
            ui.ItemClick("My Button");
        })
            .Step("Enable nested checkbox",
                  [](ImGuiTestContext &ui) {
            ui.ItemCheck("Node/Checkbox");
        })
            .Step("Set slider value",
                  [](ImGuiTestContext &ui) {
            ui.ItemInputValue("Slider", 123);
        })
            .Step("Enter text",
                  [](ImGuiTestContext &ui) {
            ui.ItemInputValue("Text", "Horo");
        })
            .Step("Open About menu item", [](ImGuiTestContext &ui) {
            ui.MenuCheck("//My Window/Tools/About Dear ImGui");
        }).Step("Verify application state", [&](ImGuiTestContext &) {
            IM_CHECK(app.buttonClicked);
            IM_CHECK(app.checkboxChecked);
            IM_CHECK_EQ(app.sliderValue, 123);
            IM_CHECK_STR_EQ(app.text.data(), "Horo");
            IM_CHECK(app.aboutVisible);
        });
    });

    INFO("Test Engine log:\n" << result.testEngineLog);
    CheckScenarioSteps(result);
    REQUIRE(result.Succeeded());
}

TEST_CASE("UI harness transfers a successful Test Engine scenario to Catch2", "[ui][imgui][editor]") {
    Horo::Tests::EditorUiTestHarness harness{Horo::Tests::CreateHeadlessEditorUiTestSurface()};
    bool clicked = false;
    const auto result = harness.Run("horo_harness", "button_click", [&](ImGuiTestContext *) {
        ImGui::Begin("Harness###horo_ui_harness", nullptr, ImGuiWindowFlags_NoSavedSettings);
        clicked = ImGui::Button("Action###horo_ui_harness_action") || clicked;
        ImGui::End();
    }, [](ImGuiTestContext *context) {
        context->SetRef("Harness###horo_ui_harness");
        context->ItemClick("Action###horo_ui_harness_action");
    });

    INFO("simulated frames: " << result.simulatedFrames);
    INFO("Test Engine log:\n" << result.testEngineLog);
    REQUIRE(result.exception == nullptr);
    REQUIRE_FALSE(result.frameBudgetExceeded);
    REQUIRE(result.testsRun == 1);
    REQUIRE(result.testsSucceeded == 1);
    REQUIRE(clicked);
}

TEST_CASE("UI harness reports a Test Engine assertion without asserting from the coroutine", "[ui][imgui][editor]") {
    Horo::Tests::EditorUiTestHarness harness{Horo::Tests::CreateHeadlessEditorUiTestSurface()};
    const auto result = harness.Run("horo_harness", "engine_assertion", [](ImGuiTestContext *) {
    }, [](ImGuiTestContext *) {
        IM_CHECK(false);
    });

    INFO("Test Engine log:\n" << result.testEngineLog);
    REQUIRE(result.testsRun == 1);
    REQUIRE(result.testsSucceeded == 0);
    REQUIRE_FALSE(result.Succeeded());
}

TEST_CASE("UI harness transfers coroutine exceptions after joining", "[ui][imgui][editor]") {
    Horo::Tests::EditorUiTestHarness harness{Horo::Tests::CreateHeadlessEditorUiTestSurface()};
    const auto result = harness.Run("horo_harness", "exception", [](ImGuiTestContext *) {
    }, [](ImGuiTestContext *) {
        throw std::runtime_error{"scenario failure"};
    });

    REQUIRE(result.exception != nullptr);
    REQUIRE_FALSE(result.Succeeded());
    std::string message;
    try {
        std::rethrow_exception(result.exception);
    } catch (const std::exception &error) {
        message = error.what();
    }
    REQUIRE(message == "scenario failure");
}

TEST_CASE("UI harness aborts after a GUI coroutine exception", "[ui][imgui][editor]") {
    Horo::Tests::EditorUiTestHarness harness{Horo::Tests::CreateHeadlessEditorUiTestSurface()};
    const auto result = harness.Run("horo_harness", "gui_exception", [](ImGuiTestContext *) {
        throw std::runtime_error{"GUI frame failure"};
    }, [](ImGuiTestContext *context) {
        while (!context->Abort)
            context->Yield();
    });

    REQUIRE(result.exception != nullptr);
    REQUIRE_FALSE(result.frameBudgetExceeded);
    REQUIRE_FALSE(result.Succeeded());
}

TEST_CASE("UI scenario pipe skips child tests after the first failed step", "[ui][imgui][editor][contract]") {
    Horo::Tests::EditorUiTestHarness harness{Horo::Tests::CreateHeadlessEditorUiTestSurface()};
    bool skippedOperationRan = false;
    const auto result = harness.RunScenario("horo_harness", "failed_child_step", [](ImGuiTestContext *) {
    }, [&](Horo::Tests::UiScenarioPipe &scenario) {
        scenario
            .Step("Fail", [](ImGuiTestContext &) {
            IM_CHECK(false);
        }).Step("Must be skipped", [&](ImGuiTestContext &) {
            skippedOperationRan = true;
        });
    });

    REQUIRE(result.steps.size() == 2);
    REQUIRE(result.steps[0].status == Horo::Tests::UiScenarioStepStatus::Failed);
    REQUIRE(result.steps[1].status == Horo::Tests::UiScenarioStepStatus::Skipped);
    REQUIRE_FALSE(skippedOperationRan);
    REQUIRE_FALSE(result.Succeeded());
}

TEST_CASE("UI harness rejects a second process-local ImGui context", "[ui][imgui][editor][contract]") {
    Horo::Tests::EditorUiTestHarness owner{Horo::Tests::CreateHeadlessEditorUiTestSurface()};
    REQUIRE_THROWS_AS(Horo::Tests::EditorUiTestHarness{}, std::logic_error);
}

TEST_CASE("UI harness stops a scenario at its deterministic frame budget", "[ui][imgui][editor]") {
    Horo::Tests::EditorUiTestHarness harness{Horo::Tests::CreateHeadlessEditorUiTestSurface()};
    const auto result = harness.Run("horo_harness", "frame_budget", [](ImGuiTestContext *) {
    }, [](ImGuiTestContext *context) {
        while (!context->Abort)
            context->Yield();
    }, Horo::Tests::EditorUiScenarioBudget{3});

    REQUIRE(result.frameBudgetExceeded);
    REQUIRE(result.simulatedFrames == 3);
    REQUIRE_FALSE(result.Succeeded());
}

TEST_CASE("UI harness cancellation aborts and joins the active scenario", "[ui][imgui][editor]") {
    Horo::Tests::EditorUiTestHarness harness{Horo::Tests::CreateHeadlessEditorUiTestSurface()};
    const auto result = harness.Run("horo_harness", "cancellation", [&](ImGuiTestContext *) {
        harness.RequestCancel();
    }, [](ImGuiTestContext *context) {
        while (!context->Abort)
            context->Yield();
    });

    REQUIRE(result.cancelled);
    REQUIRE_FALSE(result.frameBudgetExceeded);
    REQUIRE_FALSE(result.Succeeded());
}
