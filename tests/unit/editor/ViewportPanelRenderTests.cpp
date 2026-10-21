#include "../../helpers/extensions/ViewportContributionTestSupport.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Foundation/DataBus.h"
#include "ViewportPanelRenderSupport.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/workspace/panels/viewport/ViewportOverlay.h"
#include "editor/screens/workspace/panels/viewport/ViewportPanel.h"
#include "editor/screens/workspace/panels/viewport/navigation/NavigationOverlayLayer.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <imgui.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {
    using TestLocalization = Horo::Editor::Tests::ViewportRenderSupport::TestLocalization;
    using FakeViewportRenderer = Horo::Editor::Tests::ViewportRenderSupport::FakeViewportRenderer;
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
    Horo::Tests::ViewportContribution viewportContribution;
    ViewportPanel panel;
    PanelContext panelContext{
        .dataBus = editorEvents,
        .viewportRenderer = &renderer,
        .inputRouter = &inputRouter,
        .workspaceInputContext = &workspaceInput,
        .extensionViewport = &viewportContribution.host,
    };
    panel.OnAttach(panelContext);

    Input::FrameNumber inputFrame = 1;
    ImVec2 viewportOrigin;

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
        viewportOrigin = ImGui::GetCursorScreenPos();
        panel.DrawPanel(viewportOrigin, ImVec2(400.0F, 280.0F), viewModel, command, context);
        ImGui::End();
        ImGui::Render();
    };

    const auto rotationPinCenter = [&]() -> std::optional<ImVec2> {
        const ImU32 highlight = ImGui::GetColorU32(ImVec4{1.0F, 1.0F, 1.0F, 0.9F});
        ImVec2 total{};
        int count = 0;
        const ImDrawData *drawData = ImGui::GetDrawData();
        for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
            for (const ImDrawVert &vertex : drawData->CmdLists[listIndex]->VtxBuffer)
                if (vertex.col == highlight) {
                    total.x += vertex.pos.x;
                    total.y += vertex.pos.y;
                    ++count;
                }
        if (count == 0)
            return std::nullopt;
        return ImVec2{total.x / count, total.y / count};
    };

    const auto rotationRingPointNear = [&](const ImVec2 target) -> std::optional<ImVec2> {
        const ImU32 ringColor = ImGui::GetColorU32(ImVec4{0.48F, 0.64F, 1.0F, 1.0F});
        float nearestDistance = std::numeric_limits<float>::max();
        std::optional<ImVec2> nearest;
        const ImDrawData *drawData = ImGui::GetDrawData();
        for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
            for (const ImDrawVert &vertex : drawData->CmdLists[listIndex]->VtxBuffer) {
                if (vertex.col != ringColor)
                    continue;
                const float distance = std::hypot(vertex.pos.x - target.x, vertex.pos.y - target.y);
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearest = vertex.pos;
                }
            }
        }
        return nearest;
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
    REQUIRE((renderer.requestedExtent.height == 560));
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
    io.AddMousePosEvent(230.0F, 148.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    command = {};
    io.AddMousePosEvent(250.0F, 148.0F);
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

    const std::array tones{std::pair{Extensions::EditorViewportTone::Neutral, Theme::U32(Theme::Text())},
                           std::pair{Extensions::EditorViewportTone::Accent, Theme::U32(Theme::Accent())},
                           std::pair{Extensions::EditorViewportTone::X, Theme::U32(Theme::Err())},
                           std::pair{Extensions::EditorViewportTone::Y, Theme::U32(Theme::Ok())},
                           std::pair{Extensions::EditorViewportTone::Z, Theme::U32(Theme::Accent())}};
    Extensions::EditorViewportDefinition guides;
    for (std::size_t i = 0; i < tones.size(); ++i) {
        const float y = 0.6F + static_cast<float>(i) * 0.04F;
        guides.lines.emplace_back(0.1F, y, 0.3F, y, 2.0F, tones[i].first, std::int8_t{-1});
    }
    REQUIRE(viewportContribution.Publish(std::move(guides), "fixture.guides", true));
    auto guideOwner = std::move(*viewportContribution.registration);
    drawFrame();
    const ImDrawData *guideDraw = ImGui::GetDrawData();
    REQUIRE(guideDraw != nullptr);
    for (std::size_t i = 0; i < tones.size(); ++i) {
        bool found = false;
        const float y = viewportOrigin.y + (0.6F + static_cast<float>(i) * 0.04F) * 280.0F;
        for (int list = 0; list < guideDraw->CmdListsCount; ++list)
            for (const ImDrawVert &vertex : guideDraw->CmdLists[list]->VtxBuffer)
                found = found || (vertex.col == tones[i].second && std::abs(vertex.pos.y - y) < 2.0F &&
                                  vertex.pos.x >= viewportOrigin.x + 40.0F && vertex.pos.x <= viewportOrigin.x + 120.0F);
        CHECK(found);
    }
    guideOwner.Reset();

    // A provider handle far from the built-in gizmo must use the same captured transaction path.
    REQUIRE(viewportContribution.Publish({"horo.scene.transform",
                                          Extensions::EditorViewportTool::Move,
                                          {{0.1F, 0.7F, 0.3F, 0.7F, 2, Extensions::EditorViewportTone::X, 0}}}));
    const auto beginContributedDrag = [&] {
        command = {};
        io.AddMousePosEvent(88.0F, 204.0F);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        drawFrame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        drawFrame();
        REQUIRE(inputRouter.HasCapture());
        command = {};
        io.AddMousePosEvent(108.0F, 204.0F);
        drawFrame();
        REQUIRE(command.command == EditorWorkspaceViewCommand::PreviewObjectTransform);
        REQUIRE(command.objectPayload == SceneObjectId{1});
    };
    beginContributedDrag();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    REQUIRE(command.command == EditorWorkspaceViewCommand::CommitObjectTransform);
    CHECK_FALSE(inputRouter.HasCapture());
    beginContributedDrag();
    REQUIRE(viewportContribution.host.SetVisible({"fixture.package", "fixture.module", 1}, "fixture.gizmo", false).HasValue());
    command = {};
    drawFrame();
    REQUIRE(command.command == EditorWorkspaceViewCommand::CancelObjectTransformPreview);
    CHECK_FALSE(inputRouter.HasCapture());
    REQUIRE(viewportContribution.host.SetVisible({"fixture.package", "fixture.module", 1}, "fixture.gizmo", true).HasValue());
    beginContributedDrag();
    inputRouter.CancelCapture(Input::CaptureCancellationReason::FocusLost);
    command = {};
    drawFrame();
    REQUIRE(command.command == EditorWorkspaceViewCommand::CancelObjectTransformPreview);
    CHECK_FALSE(inputRouter.HasCapture());
    beginContributedDrag();
    command = {};
    panel.OnPresentationHidden(command);
    REQUIRE(command.command == EditorWorkspaceViewCommand::CancelObjectTransformPreview);
    CHECK_FALSE(inputRouter.HasCapture());
    beginContributedDrag();
    command.command = EditorWorkspaceViewCommand::SaveScene;
    panel.OnPresentationHidden(command);
    CHECK(command.command == EditorWorkspaceViewCommand::SaveScene);
    CHECK_FALSE(inputRouter.HasCapture());
    beginContributedDrag();
    viewportContribution.admission->Revoke();
    command = {};
    drawFrame();
    REQUIRE(command.command == EditorWorkspaceViewCommand::CancelObjectTransformPreview);
    CHECK_FALSE(inputRouter.HasCapture());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    command = {};
    drawFrame();

    viewModel.activeTransformTool = EditorTransformTool::Rotate;
    command = {};
    io.AddMousePosEvent(10.0F, 200.0F);
    drawFrame();
    const std::optional<ImVec2> ringPoint = rotationRingPointNear({278.0F, 226.0F});
    REQUIRE(ringPoint.has_value());
    io.AddMousePosEvent(ringPoint->x, ringPoint->y);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    const std::optional<ImVec2> pinAtStart = rotationPinCenter();
    REQUIRE(pinAtStart.has_value());
    command = {};
    io.AddMousePosEvent(160.0F, 220.0F);
    drawFrame();
    const std::optional<ImVec2> pinAfterRotation = rotationPinCenter();
    REQUIRE(pinAfterRotation.has_value());
    REQUIRE(std::hypot(pinAfterRotation->x - pinAtStart->x, pinAfterRotation->y - pinAtStart->y) > 10.0F);
    REQUIRE((command.command == EditorWorkspaceViewCommand::PreviewObjectTransform));
    REQUIRE((command.transformPayload.has_value()));
    REQUIRE((command.transformPayload->rotation != Math::Quaternion::Identity()));
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    REQUIRE_FALSE(rotationPinCenter().has_value());
    REQUIRE((command.command == EditorWorkspaceViewCommand::CommitObjectTransform));

    viewModel.activeTransformSpace = EditorTransformSpace::World;
    const Math::Transform parentTransform{.rotation = Math::Quaternion::FromEulerRadians({0.0F, 0.35F, 0.0F}),
                                          .scale = {2.0F, 0.75F, 1.5F}};
    viewModel.primarySelectionParentWorldTransform = parentTransform.ToMatrix();
    viewModel.primarySelectionWorldTransform =
        Math::Multiply(parentTransform.ToMatrix(), viewModel.objects.front().localTransform.ToMatrix());
    command = {};
    io.AddMousePosEvent(10.0F, 200.0F);
    drawFrame();
    const std::optional<ImVec2> worldRingPoint = rotationRingPointNear({278.0F, 226.0F});
    REQUIRE(worldRingPoint.has_value());
    io.AddMousePosEvent(worldRingPoint->x, worldRingPoint->y);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    command = {};
    io.AddMousePosEvent(160.0F, 220.0F);
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
    io.AddMousePosEvent(210.0F, 148.0F);
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

TEST_CASE("Navigation overlay clips crossing triangles and segments before perspective division", "[editor][viewport][navigation]") {
    using namespace Horo;
    using namespace Horo::Editor;
    for (const auto depth : {Math::ClipDepthRange::NegativeOneToOne, Math::ClipDepthRange::ZeroToOne}) {
        NavigationOverlayPrimitive primitive{.points = {{{-4, 0, 0.5F}, {4, 0, 0.5F}, {0, 4, 0.5F}}}, .triangle = true};
        const auto triangle = ProjectNavigationOverlay(primitive, Math::Mat4::Identity(), {20, 30}, {640, 480}, depth);
        REQUIRE(triangle.count >= 3);
        CHECK(triangle.count <= triangle.points.size());
        for (std::size_t i = 0; i < triangle.count; ++i) {
            CHECK(triangle.points[i].x >= 20);
            CHECK(triangle.points[i].x <= 660);
            CHECK(triangle.points[i].y >= 30);
            CHECK(triangle.points[i].y <= 510);
        }
        primitive.triangle = false;
        primitive.points = {{{0, 0, -2}, {0.5F, 0, 0.5F}, {}}};
        CHECK(ProjectNavigationOverlay(primitive, Math::Mat4::Identity(), {}, {640, 480}, depth).count == 2);
        primitive.points[1].z = -3;
        CHECK(ProjectNavigationOverlay(primitive, Math::Mat4::Identity(), {}, {640, 480}, depth).count == 0);
        CHECK(ProjectNavigationOverlay(primitive, Math::Mat4::Identity(), {}, {0, 480}, depth).count == 0);
    }
}
