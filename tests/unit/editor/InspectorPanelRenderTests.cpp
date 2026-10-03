#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {
    class TestLocalization final : public Horo::Editor::ILocalizationService {
    public:
        [[nodiscard]] const std::string &Get(const std::string_view, const std::string_view localKey) const override {
            const auto [entry, inserted] = values_.try_emplace(std::string(localKey), localKey);
            static_cast<void>(inserted);
            return entry->second;
        }

        void Set(const std::string_view key, std::string value) {
            values_.insert_or_assign(std::string(key), std::move(value));
        }

    private:
        mutable std::unordered_map<std::string, std::string> values_;
    };
}  // namespace

TEST_CASE("Inspector Panel Render Tests", "[unit][editor]") {
    using namespace Horo;
    using namespace Horo::Editor;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0F, 480.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());

    EngineDataBus engineEvents;
    EditorDataBus editorEvents;
    TestLocalization localization;
    localization.Set("workspace.panel.inspector", "Inspector");
    localization.Set("workspace.panel.scene", "Scene");
    localization.Set("workspace.inspector.object_options", "Object options");
    ImFont *defaultFont = io.Fonts->Fonts.front();
    const Theme::Fonts fonts{.sans = defaultFont, .sansCompact = defaultFont, .sansEmphasis = defaultFont};
    const ThemeContext theme{.fonts = fonts};
    const EditorSettingsSnapshot settings{};
    const EditorGuiContext context{.engineEvents = engineEvents,
                                   .editorEvents = editorEvents,
                                   .localization = localization,
                                   .theme = theme,
                                   .settings = settings};
    EditorWorkspaceViewModel viewModel;
    viewModel.documentRevision = DocumentRevision{3};
    viewModel.objects = {SceneObject{
        .id = SceneObjectId{7},
        .name = "Box",
        .kind = SceneObjectKind::Mesh,
        .localTransform =
            Math::Transform{
                .translation = {1.0F, 2.0F, 3.0F},
                .rotation = Math::Quaternion::FromEulerRadians({0.1F, 0.2F, 0.3F}),
                .scale = {1.0F, 1.5F, 2.0F},
            },
    }};
    viewModel.primarySelection = SceneObjectId{7};
    viewModel.selectedObjects = {SceneObjectId{7}};
    EditorWorkspaceViewCommandData command;
    InspectorPanel panel;
    ImVec2 windowSize{280.0F, 440.0F};
    ImVec2 panelSize{260.0F, 400.0F};

    const auto drawFrame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(windowSize);
        ImGui::Begin("InspectorRenderTest", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        panel.DrawPanel(ImGui::GetCursorScreenPos(), panelSize, viewModel, command, context);
        ImGui::End();
        ImGui::Render();
    };

    io.AddFocusEvent(true);
    io.AddMousePosEvent(170.0F, 116.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();

    REQUIRE((command.command == EditorWorkspaceViewCommand::None));
    REQUIRE((panel.GetObservedEventTypes() == std::vector<std::string>({"SceneDocumentChangedEvent", "SelectionChangedEvent"})));

    command = {};
    io.AddMousePosEvent(60.0F, 65.0F);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    io.AddInputCharactersUTF8("Hero");
    drawFrame();
    REQUIRE(io.WantTextInput);
    REQUIRE((command.command == EditorWorkspaceViewCommand::None));
    command = {};
    io.AddMousePosEvent(120.0F, 135.0F);
    drawFrame();
    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::UpdateObjectName));
    REQUIRE((command.objectPayload == SceneObjectId{7}));
    REQUIRE((command.stringPayload == "Hero"));

    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    command = {};
    io.AddMousePosEvent(120.0F, 145.0F);
    drawFrame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    drawFrame();
    command = {};
    io.AddMousePosEvent(155.0F, 145.0F);
    drawFrame();
    if (command.command == EditorWorkspaceViewCommand::None) {
        io.AddMousePosEvent(165.0F, 145.0F);
        drawFrame();
    }
    REQUIRE((command.command == EditorWorkspaceViewCommand::PreviewObjectTransform));
    REQUIRE((command.transformUpdates.has_value()));
    REQUIRE((command.transformUpdates->front().object == SceneObjectId{7}));
    REQUIRE((command.transformUpdates->front().localTransform.translation.x != 1.0F));

    command = {};
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::CommitObjectTransform));
    REQUIRE((command.transformUpdates.has_value()));

    SceneObject light{
        .id = SceneObjectId{8},
        .name = "Key Light",
        .kind = SceneObjectKind::Light,
    };
    light.components.light = Runtime::LightComponent{
        .kind = Runtime::LightKind::Spot,
        .color = {1.0F, 0.8F, 0.6F},
        .intensity = 2.0F,
        .range = 18.0F,
        .innerConeRadians = 0.25F,
        .outerConeRadians = 0.75F,
    };
    viewModel.objects = {light};
    viewModel.primarySelection = light.id;
    viewModel.selectedObjects = {light.id};
    ++viewModel.documentRevision.value;
    command = {};
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::None));

    localization.Set("workspace.panel.inspector", "Nesne Özellikleri ve Yapılandırma");
    localization.Set("workspace.inspector.camera_projection", "Son Derece Uzun Kamera İzdüşüm Yöntemi");
    localization.Set("workspace.inspector.camera_orthographic_height", "Ortografik Görüntüleme Yüksekliği");
    SceneObject camera{
        .id = SceneObjectId{9},
        .name = "Çok Uzun Yerelleştirilmiş Kamera Nesnesi",
        .kind = SceneObjectKind::Camera,
    };
    camera.components.camera = Runtime::CameraComponent{.projection = Runtime::CameraProjection::Orthographic};
    viewModel.objects = {camera};
    viewModel.primarySelection = camera.id;
    viewModel.selectedObjects = {camera.id};
    ++viewModel.documentRevision.value;
    windowSize = {180.0F, 440.0F};
    panelSize = {160.0F, 400.0F};
    io.DisplaySize = {180.0F, 480.0F};
    command = {};
    drawFrame();
    REQUIRE((command.command == EditorWorkspaceViewCommand::None));
    const ImDrawData *drawData = ImGui::GetDrawData();
    REQUIRE((drawData != nullptr));
    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
        for (const ImDrawCmd &drawCommand : drawData->CmdLists[listIndex]->CmdBuffer) {
            REQUIRE((drawCommand.ClipRect.x >= 0.0F));
            REQUIRE((drawCommand.ClipRect.z <= io.DisplaySize.x));
        }
    }

    ImGui::DestroyContext();
}
