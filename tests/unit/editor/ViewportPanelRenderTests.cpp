#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/workspace/panels/viewport/ViewportPanel.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {
    class TestLocalization final : public Horo::Editor::ILocalizationService {
    public:
        [[nodiscard]] const std::string &Get(const std::string_view, const std::string_view localKey) const override {
            std::string_view value = localKey;
            if (localKey == "workspace.viewport.object_count") {
                value = "{} objects";
            }
            const auto [entry, inserted] = values_.try_emplace(std::string(localKey), value);
            static_cast<void>(inserted);
            return entry->second;
        }

    private:
        mutable std::unordered_map<std::string, std::string> values_;
    };

    class FakeViewportRenderer final : public Horo::Editor::IEditorViewportRenderer {
    public:
        void RequestExtent(const Horo::Editor::EditorViewportExtent extent) noexcept override {
            requestedExtent = extent;
        }

        void RequestGrid(const Horo::Editor::EditorViewportGridOptions &options) noexcept override {
            gridOptions = options;
        }

        void RequestLightVisualizer(const Horo::Editor::EditorViewportLightVisualizerOptions &options) noexcept override {
            lightVisualizerOptions = std::move(options);
        }

        [[nodiscard]] Horo::Editor::EditorViewportExtent RequestedExtent() const noexcept override {
            return requestedExtent;
        }

        [[nodiscard]] Horo::Math::ClipDepthRange ClipDepthRange() const noexcept override {
            return depthRange;
        }

        [[nodiscard]] Horo::Result<void> ExecuteStaticMeshPass(const Horo::Render::StaticMeshPassDescriptor &) override {
            return Horo::Result<void>::Success();
        }

        [[nodiscard]] Horo::Editor::EditorViewportTextureView TextureView() const noexcept override {
            return Horo::Editor::EditorViewportTextureView{
                .textureId = textureId,
                .u0 = 0.25F,
                .v0 = 0.75F,
                .u1 = 0.75F,
                .v1 = 0.25F,
            };
        }

        [[nodiscard]] bool IsReady() const noexcept override {
            return true;
        }

        Horo::Editor::EditorViewportExtent requestedExtent{};
        Horo::Editor::EditorViewportGridOptions gridOptions{};
        Horo::Editor::EditorViewportLightVisualizerOptions lightVisualizerOptions{};
        Horo::Math::ClipDepthRange depthRange{Horo::Math::ClipDepthRange::ZeroToOne};
        static constexpr std::uintptr_t textureId = 42;
    };
}  // namespace

TEST_CASE("Viewport Panel Render Tests", "[unit][editor]") {
    using namespace Horo;
    using namespace Horo::Editor;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0F, 480.0F);
    io.DisplayFramebufferScale = ImVec2(2.0F, 2.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());

    EngineDataBus engineEvents;
    EditorDataBus editorEvents;
    TestLocalization localization;
    ImFont *defaultFont = io.Fonts->Fonts.front();
    const Theme::Fonts fonts{.sans = defaultFont, .sansCompact = defaultFont, .sansEmphasis = defaultFont};
    const ThemeContext theme{.fonts = fonts};
    EditorSettingsSnapshot settings{};
    const EditorGuiContext context{.engineEvents = engineEvents,
                                   .editorEvents = editorEvents,
                                   .localization = localization,
                                   .theme = theme,
                                   .settings = settings};
    EditorWorkspaceViewModel viewModel;
    EditorWorkspaceViewCommandData command;
    FakeViewportRenderer renderer;
    Input::RawInputCollector inputCollector;
    Input::InputRouter inputRouter;
    auto workspaceInput = inputRouter.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
    ViewportPanel panel;
    PanelContext panelContext{
        .dataBus = editorEvents,
        .viewportRenderer = &renderer,
        .inputRouter = &inputRouter,
        .workspaceInputContext = &workspaceInput,
    };
    panel.OnAttach(panelContext);

    Input::FrameNumber inputFrame = 1;

    const auto drawFrame = [&] {
        ImGui::NewFrame();
        inputCollector.BeginFrame(inputFrame++);
        inputCollector.SetPointerPosition(io.MousePos.x, io.MousePos.y);
        inputCollector.SetPointerButton(Input::PointerButton::Primary, io.MouseDown[ImGuiMouseButton_Left]);
        inputCollector.SetPointerButton(Input::PointerButton::Secondary, io.MouseDown[ImGuiMouseButton_Right]);
        inputCollector.SetPointerButton(Input::PointerButton::Middle, io.MouseDown[ImGuiMouseButton_Middle]);
        inputCollector.SetKey(Input::Key::W, ImGui::IsKeyDown(ImGuiKey_W));
        inputCollector.SetModifiers(Input::ModifierState{.shift = io.KeyShift, .alt = io.KeyAlt});
        inputRouter.BeginFrame(inputCollector.Commit());
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(420.0F, 320.0F));
        ImGui::Begin("ViewportRenderTest", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        panel.DrawPanel(ImGui::GetCursorScreenPos(), ImVec2(400.0F, 280.0F), viewModel, command, context);
        ImGui::End();
        ImGui::Render();
    };

    io.AddFocusEvent(true);
    io.AddMousePosEvent(80.0F, 54.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::None));
    REQUIRE((ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)));
    command = {};
    io.AddMousePosEvent(300.0F, 200.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    REQUIRE((!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)));

    command = {};
    io.AddMousePosEvent(200.0F, 160.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();

    REQUIRE((renderer.requestedExtent.width == 800));
    REQUIRE((renderer.requestedExtent.height == 504));
    REQUIRE((renderer.gridOptions.visible));
    REQUIRE((command.command == EditorWorkspaceViewCommand::PickViewport));
    REQUIRE((command.viewportPickPayload.has_value()));
    REQUIRE((command.viewportPickPayload->normalizedX > 0.0F && command.viewportPickPayload->normalizedX < 1.0F));
    REQUIRE((command.viewportPickPayload->normalizedY > 0.0F && command.viewportPickPayload->normalizedY < 1.0F));
    REQUIRE((command.viewportPickPayload->aspect > 1.0F));
    REQUIRE((command.viewportPickPayload->depthRange == Math::ClipDepthRange::ZeroToOne));

    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    drawFrame();
    command = {};
    io.AddMousePosEvent(220.0F, 145.0F);
    io.AddKeyEvent(ImGuiKey_W, true);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::NavigateViewport));
    REQUIRE((command.viewportNavigationPayload.has_value()));
    REQUIRE((command.viewportNavigationPayload->yawRadians != 0.0F));
    REQUIRE((command.viewportNavigationPayload->pitchRadians != 0.0F));
    REQUIRE((command.viewportNavigationPayload->moveForward > 0.0F));
    REQUIRE((command.viewportNavigationPayload->moveForward > 0.016F));
    REQUIRE((command.viewportNavigationPayload->moveForward < 0.017F));
    const float oneSixtiethMove = command.viewportNavigationPayload->moveForward;

    command = {};
    io.DeltaTime = 1.0F / 30.0F;
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::NavigateViewport));
    REQUIRE((command.viewportNavigationPayload.has_value()));
    REQUIRE((command.viewportNavigationPayload->moveForward > oneSixtiethMove * 1.99F));
    REQUIRE((command.viewportNavigationPayload->moveForward < oneSixtiethMove * 2.01F));
    io.DeltaTime = 1.0F / 60.0F;

    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    io.AddKeyEvent(ImGuiKey_W, false);
    drawFrame();
    viewModel.activeTransformTool = EditorTransformTool::Move;
    viewModel.viewportCamera = EditorViewportCamera{};
    viewModel.objects = {SceneObject{.id = SceneObjectId{1}, .name = "Box", .kind = SceneObjectKind::Mesh}};
    viewModel.primarySelection = SceneObjectId{1};
    viewModel.primarySelectionWorldTransform = Math::Mat4::Identity();
    viewModel.primarySelectionParentWorldTransform = Math::Mat4::Identity();
    command = {};
    io.AddMousePosEvent(230.0F, 162.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    command = {};
    io.AddMousePosEvent(250.0F, 162.0F);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::PreviewObjectTransform));
    REQUIRE((command.objectPayload == SceneObjectId{1}));
    REQUIRE((command.transformPayload.has_value()));
    REQUIRE((command.transformPayload->translation.x > 0.0F));
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::CommitObjectTransform));
    REQUIRE((command.transformPayload.has_value()));

    viewModel.activeTransformTool = EditorTransformTool::Rotate;
    command = {};
    io.AddMousePosEvent(242.0F, 128.0F);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    command = {};
    io.AddMousePosEvent(242.0F, 196.0F);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::PreviewObjectTransform));
    REQUIRE((command.transformPayload.has_value()));
    REQUIRE((command.transformPayload->rotation != Math::Quaternion::Identity()));
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::CommitObjectTransform));

    viewModel.activeTransformSpace = EditorTransformSpace::World;
    const Math::Transform parentTransform{.rotation = Math::Quaternion::FromEulerRadians({0.0F, 0.35F, 0.0F}),
                                          .scale = {2.0F, 0.75F, 1.5F}};
    viewModel.primarySelectionParentWorldTransform = parentTransform.ToMatrix();
    viewModel.primarySelectionWorldTransform =
        Math::Multiply(parentTransform.ToMatrix(), viewModel.objects.front().localTransform.ToMatrix());
    command = {};
    io.AddMousePosEvent(242.0F, 128.0F);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    command = {};
    io.AddMousePosEvent(242.0F, 196.0F);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::PreviewObjectTransform));
    REQUIRE((command.transformPayload.has_value()));
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::CommitObjectTransform));

    viewModel.activeTransformTool = EditorTransformTool::Scale;
    viewModel.objects.front().localTransform.scale = {-1.0F, 1.0F, 1.0F};
    viewModel.primarySelectionWorldTransform =
        Math::Multiply(parentTransform.ToMatrix(), viewModel.objects.front().localTransform.ToMatrix());
    command = {};
    io.AddMousePosEvent(210.0F, 162.0F);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    command = {};
    io.AddMousePosEvent(235.0F, 137.0F);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::PreviewObjectTransform));
    REQUIRE((command.transformPayload.has_value()));
    REQUIRE((command.transformPayload->scale.x < -1.0F));
    REQUIRE((command.transformPayload->scale.y > 1.0F));
    command = {};
    auto modalContext = inputRouter.PushContext(Input::InputContextId{"test.modal"}, Input::InputContextKind::ModalRoot);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::CancelObjectTransformPreview));
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::None));
    modalContext.Reset();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    command = {};
    drawFrame();

    bool foundViewportTexture = false;
    bool foundAdapterUv0 = false;
    bool foundAdapterUv1 = false;
    const ImDrawData *drawData = ImGui::GetDrawData();
    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
        const ImDrawList *drawList = drawData->CmdLists[listIndex];
        for (const ImDrawCmd &drawCommand : drawList->CmdBuffer) {
            foundViewportTexture = foundViewportTexture || drawCommand.GetTexID() == FakeViewportRenderer::textureId;
        }
        for (const ImDrawVert &vertex : drawList->VtxBuffer) {
            foundAdapterUv0 = foundAdapterUv0 || (vertex.uv.x == 0.25F && vertex.uv.y == 0.75F);
            foundAdapterUv1 = foundAdapterUv1 || (vertex.uv.x == 0.75F && vertex.uv.y == 0.25F);
        }
    }
    REQUIRE((foundViewportTexture));
    REQUIRE((foundAdapterUv0 && foundAdapterUv1));

    settings.settings.gridOverlay = false;
    drawFrame();
    REQUIRE_FALSE((renderer.gridOptions.visible));

    viewModel.viewportLights = {
        ViewportLightPresentation{
            .object = SceneObjectId{2},
            .light =
                Render::RenderLight{
                    .kind = Render::RenderLightKind::Spot,
                    .position = {0.0F, 0.0F, 0.0F},
                    .direction = {0.0F, -1.0F, 0.0F},
                },
        },
    };
    viewModel.primarySelection.reset();
    command = {};
    io.AddMousePosEvent(200.0F, 154.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::SelectObject));
    REQUIRE((command.objectPayload == SceneObjectId{2}));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);

    viewModel.primarySelection = SceneObjectId{2};
    drawFrame();
    REQUIRE((renderer.lightVisualizerOptions.selectedLight.has_value()));
    REQUIRE((renderer.lightVisualizerOptions.selectedLight->kind == Render::RenderLightKind::Spot));
    viewModel.primarySelection.reset();
    drawFrame();
    REQUIRE_FALSE((renderer.lightVisualizerOptions.selectedLight.has_value()));

    panel.OnDetach();
    ImGui::DestroyContext();
}
