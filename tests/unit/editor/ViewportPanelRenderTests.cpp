#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/workspace/panels/viewport/ViewportOverlay.h"
#include "editor/screens/workspace/panels/viewport/ViewportPanel.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <imgui.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

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

TEST_CASE("Viewport compass selects both signed ends of each axis", "[unit][editor][viewport]") {
    using namespace Horo::Editor;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {900.0F, 600.0F};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());
    ImFont *font = io.Fonts->Fonts.front();
    const Theme::Fonts fonts{.sans = font, .sansCompact = font, .sansEmphasis = font};
    TestLocalization localization;
    ViewportOverlayState state;
    state.camera.position = {4.0F, 3.0F, 5.0F};
    const Horo::Math::Vec3 forward = Horo::Math::TryNormalize(state.camera.target - state.camera.position).Value();
    const Horo::Math::Vec3 right = Horo::Math::TryNormalize(Horo::Math::Cross(forward, state.camera.up)).Value();
    const Horo::Math::Vec3 up = Horo::Math::Cross(right, forward);
    const auto draw = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({900.0F, 600.0F});
        ImGui::Begin("CompassTest", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const ViewportOverlayAction action = DrawViewportOverlay({0.0F, 0.0F}, {800.0F, 500.0F}, state, fonts, localization);
        ImGui::End();
        ImGui::Render();
        return action;
    };
    constexpr std::array endpoints{
        std::pair{Horo::Math::Vec3{1.0F, 0.0F, 0.0F}, EditorViewportAxisView::PositiveX},
        std::pair{Horo::Math::Vec3{-1.0F, 0.0F, 0.0F}, EditorViewportAxisView::NegativeX},
        std::pair{Horo::Math::Vec3{0.0F, 1.0F, 0.0F}, EditorViewportAxisView::PositiveY},
        std::pair{Horo::Math::Vec3{0.0F, -1.0F, 0.0F}, EditorViewportAxisView::NegativeY},
        std::pair{Horo::Math::Vec3{0.0F, 0.0F, 1.0F}, EditorViewportAxisView::PositiveZ},
        std::pair{Horo::Math::Vec3{0.0F, 0.0F, -1.0F}, EditorViewportAxisView::NegativeZ},
    };
    for (const auto &[axis, expected] : endpoints) {
        io.AddMousePosEvent(752.0F + Horo::Math::Dot(axis, right) * 29.0F, 64.0F - Horo::Math::Dot(axis, up) * 29.0F);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(draw());
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(draw());
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        REQUIRE(draw().axisView == expected);
    }
    ImGui::DestroyContext();
}

TEST_CASE("Viewport grid control follows shading control", "[unit][editor][viewport]") {
    using namespace Horo::Editor;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {900.0F, 600.0F};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());
    ImFont *font = io.Fonts->Fonts.front();
    const Theme::Fonts fonts{.sans = font, .sansCompact = font, .sansEmphasis = font};
    TestLocalization localization;
    ViewportOverlayState state;
    const auto draw = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({900.0F, 600.0F});
        ImGui::Begin("GridTest", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const ViewportOverlayAction action = DrawViewportOverlay({0.0F, 0.0F}, {800.0F, 500.0F}, state, fonts, localization);
        ImGui::End();
        ImGui::Render();
        return action;
    };

    io.AddMousePosEvent(299.0F, 34.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    static_cast<void>(draw());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(draw());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    REQUIRE(draw().toggleGrid);
    ImGui::DestroyContext();
}

TEST_CASE("Collider gestures pass through the real viewport in logical pixels and cancel with capture",
          "[unit][editor][physics][collider]") {
    using namespace Horo;
    using namespace Horo::Editor;
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.DisplaySize = {640, 480};
    io.DeltaTime = 1.0F / 60;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());
    float dpi = 1;
    Math::ClipDepthRange depth = Math::ClipDepthRange::NegativeOneToOne;
    bool cancel = false;
    bool reload = false;
    SECTION("2x framebuffer with zero-to-one clip depth") {
        dpi = 2;
        depth = Math::ClipDepthRange::ZeroToOne;
    }
    SECTION("exclusive modal capture cancels") {
        cancel = true;
    }
    SECTION("replacement generation cancels") {
        reload = true;
    }
    io.DisplayFramebufferScale = {dpi, dpi};
    EngineDataBus engine;
    EditorDataBus events;
    TestLocalization localization;
    ImFont *font = io.Fonts->Fonts.front();
    const ThemeContext theme{.fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font}};
    const EditorSettingsSnapshot settings{};
    const EditorGuiContext gui{.engineEvents = engine,
                               .editorEvents = events,
                               .localization = localization,
                               .theme = theme,
                               .settings = settings};
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = 1;
    const auto asset = Assets::AssetId::FromBytes(bytes);
    SceneObjectSnapshot object{.id = {1}, .name = "Compound"};
    object.components.rigidBody = Runtime::RigidBodyComponent{.id = {1}, .body = {10}};
    object.components.colliders.push_back({.id = {2},
                                           .collider = {20},
                                           .body = {{1}, {10}},
                                           .collisionProfile = Physics::CollisionProfileId::FromBytes(bytes),
                                           .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1), asset}}});
    auto port = SceneDocumentTransactions::Create({asset, 1}, {object});
    REQUIRE(port.HasValue());
    const auto source = port.Value().Snapshot().Value();
    EditorWorkspaceViewModel view;
    view.physicsContext = {source.session, source.revision, source.state, true, {}};
    view.viewportColliders = ProjectPhysicsColliders(source.objects).Value();
    view.selectedColliders = {view.viewportColliders.front().target};
    view.activeTransformTool = EditorTransformTool::Move;
    Input::InputRouter router;
    Input::RawInputCollector collector;
    auto workspace = router.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
    FakeViewportRenderer renderer;
    renderer.depthRange = depth;
    ViewportPanel panel;
    PanelContext panelContext{.dataBus = events,
                              .viewportRenderer = &renderer,
                              .inputRouter = &router,
                              .workspaceInputContext = &workspace};
    panel.OnAttach(panelContext);
    Input::FrameNumber frame = 1;
    EditorWorkspaceViewCommandData command;
    const auto draw = [&] {
        command = {};
        ImGui::NewFrame();
        collector.BeginFrame(frame++);
        collector.SetPointerPosition(io.MousePos.x, io.MousePos.y);
        collector.SetPointerButton(Input::PointerButton::Primary, io.MouseDown[ImGuiMouseButton_Left]);
        router.BeginFrame(collector.Commit());
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({420, 320});
        ImGui::Begin("ColliderViewport", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        panel.DrawPanel(ImGui::GetCursorScreenPos(), {400, 280}, view, command, gui);
        ImGui::End();
        ImGui::Render();
    };
    io.AddFocusEvent(true);
    io.AddMousePosEvent(230, 148);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    draw();
    io.AddMousePosEvent(250, 148);
    draw();
    CHECK(command.command == EditorWorkspaceViewCommand::None);  // Preview is local, never an authored/object command.
    CHECK(port.Value().Snapshot().Value().revision == source.revision);
    std::optional<Input::InputContextToken> modal;
    if (cancel)
        modal.emplace(router.PushContext(Input::InputContextId{"test.modal"}, Input::InputContextKind::ModalRoot));
    if (reload)
        ++view.physicsContext.session.generation;
    if (cancel || reload)
        draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    draw();
    if (cancel || reload) {
        CHECK(command.command == EditorWorkspaceViewCommand::None);
    } else {
        REQUIRE(command.command == EditorWorkspaceViewCommand::CommitPhysicsColliders);
        REQUIRE(command.colliderPayload);
        REQUIRE(command.colliderPayload->edits.size() == 1);
        CHECK(command.colliderPayload->context.session == source.session);
        CHECK(command.colliderPayload->edits.front().transform.translation.x > 0);
        const auto prepared = PrepareColliderTransformEdits(source, command.colliderPayload->edits);
        REQUIRE(prepared.HasValue());
        REQUIRE(port.Value().Execute(command.colliderPayload->context, prepared.Value()).HasValue());
        CHECK(port.Value().Snapshot().Value().revision == source.revision + 1);
    }
    CHECK(renderer.requestedExtent.width == static_cast<std::uint32_t>(400 * dpi));
    CHECK(renderer.requestedExtent.height == static_cast<std::uint32_t>(280 * dpi));
    panel.OnDetach();
    ImGui::DestroyContext();
}
