#include "EditorUiTestHarness.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <imgui_test_engine/imgui_te_context.h>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Explicit test document owner; no runtime fallback or project binding is fabricated by production. */
    Assets::AssetId Owner() {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 1;
        return Assets::AssetId::FromBytes(bytes);
    }

    /** @brief Runs the real Inspector against its real transaction/history controller on the owner GUI thread. */
    class InspectorHost final {
    public:
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{"test-project", runtime, {}, {.documentSessionOwner = Owner()}};
        LocalizationService localization{LocaleTag{"en-US"}};
        EngineDataBus engine;
        EditorSettingsSnapshot settings;
        InspectorPanel panel;
        SceneObjectId box;
        enum class Action {
            Undo,
            Clear,
            Select
        };
        std::optional<Action> pending;

        InspectorHost() {
            const auto catalog = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/en-US.json";
            REQUIRE(localization.LoadCatalogFile(catalog));
            REQUIRE(localization.Prepare(LocaleTag{"en-US"}));
            REQUIRE(localization.ActivatePrepared());
            EditorWorkspaceViewCommandData create;
            create.command = EditorWorkspaceViewCommand::CreatePrimitive;
            create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
            controller.ProcessCommand(create);
            box = controller.ViewModel().primarySelection.value();
            PanelContext context{.dataBus = controller.DataBus()};
            panel.OnAttach(context);
        }

        ~InspectorHost() {
            panel.OnDetach();
        }

        InspectorHost(const InspectorHost &) = delete;
        InspectorHost &operator=(const InspectorHost &) = delete;

        /** @brief Only the GUI frame applies commands; the Test Engine coroutine queues intent across its joined handoff. */
        void Draw(ImGuiTestContext *) {
            if (pending) {
                EditorWorkspaceViewCommandData command;
                command.command =
                    *pending == Action::Undo ? EditorWorkspaceViewCommand::UndoScene : EditorWorkspaceViewCommand::SelectObject;
                if (*pending == Action::Select)
                    command.objectPayload = box;
                if (*pending == Action::Clear)
                    command.objectSelection = ObjectSelectionRequest{};
                controller.ProcessCommand(command);
                pending.reset();
            }
            ImFont *font = ImGui::GetIO().Fonts->Fonts.front();
            const ThemeContext theme{.fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font}};
            const EditorGuiContext gui{.engineEvents = engine,
                                       .editorEvents = controller.DataBus(),
                                       .localization = localization,
                                       .theme = theme,
                                       .settings = settings};
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({360, 600});
            ImGui::Begin("PhysicsInspector", nullptr, ImGuiWindowFlags_NoSavedSettings);
            EditorWorkspaceViewCommandData command;
            panel.DrawPanel(ImGui::GetCursorScreenPos(), {340, 560}, controller.ViewModel(), command, gui);
            ImGui::End();
            if (command.command != EditorWorkspaceViewCommand::None)
                controller.ProcessCommand(command);
        }

        /** @brief Borrows actual selected-object state while the GUI coroutine is suspended at a test boundary. */
        const SceneObject &Box() const {
            const auto &objects = controller.ViewModel().objects;
            const auto found = std::ranges::find(objects, box, &SceneObject::id);
            REQUIRE(found != objects.end());
            return *found;
        }
    };

    /** @brief Uses the exact child window so field lookup scrolls the real owning content instead of a global wildcard parent. */
    void SelectContent(ImGuiTestContext &ui) {
        ui.SetRef("PhysicsInspector/##Content");
    }

    /** @brief Creates the paired typed draft using the normal generic component menu, leaving authoring unchanged. */
    void StageBody(ImGuiTestContext &ui, InspectorHost &host) {
        SelectContent(ui);
        IM_CHECK(ui.ItemExists("###InspectorAddComponent"));
        ui.ItemClick("###InspectorAddComponent");
        ui.ItemClick("//**/###inspector_physics_body");
        ui.Yield();
        IM_CHECK(!host.Box().components.rigidBody);
        IM_CHECK(ui.ItemExists("**/workspace.physics.profile"));
        IM_CHECK(ui.ItemExists("**/workspace.physics.material_asset"));
    }

    /** @brief Supplies deliberate author-entered fixture identities to the actual shared text controls. */
    void EnterBindings(ImGuiTestContext &ui) {
        ui.ItemInputValue("**/workspace.physics.profile", "00000000-0000-0000-0000-000000000002");
        ui.ItemInputValue("**/workspace.physics.material_asset", "00000000-0000-0000-0000-000000000003");
    }
}  // namespace

TEST_CASE("Physics creation uses the generic Inspector route and canonical Apply undo and selection revocation",
          "[ui][imgui][editor][physics][inspector]") {
    Tests::EditorUiTestHarness harness;
    InspectorHost host;
    const auto result = harness.RunScenario("physics", "component_creation", [&](ImGuiTestContext *ui) {
        host.Draw(ui);
    }, [&](Tests::UiScenarioPipe &pipeline) {
        pipeline.Step("Box retains visible generic creation", [&](ImGuiTestContext &ui) {
            SelectContent(ui);
            IM_CHECK(ui.ItemExists("###InspectorAddComponent"));
            IM_CHECK(!ui.ItemExists("**/workspace.physics.profile"));
        });
        pipeline.Step("Stage full body and collider cards without publication", [&](ImGuiTestContext &ui) {
            StageBody(ui, host);
        });
        pipeline.Step("Apply authored bindings once through document history", [&](ImGuiTestContext &ui) {
            EnterBindings(ui);
            ui.ItemClick("Apply Physics edit");
            ui.Yield();
            IM_CHECK(host.controller.ViewModel().physicsEditError.empty());
            IM_CHECK(host.Box().components.rigidBody.has_value());
            IM_CHECK(host.Box().components.colliders.size() == 1);
            IM_CHECK(host.controller.ViewModel().canUndo);
        });
        pipeline.Step("Undo canonical publication and revoke another staged selection", [&](ImGuiTestContext &ui) {
            host.pending = InspectorHost::Action::Undo;
            ui.Yield();
            IM_CHECK(!host.Box().components.rigidBody);
            StageBody(ui, host);
            EnterBindings(ui);
            host.pending = InspectorHost::Action::Clear;
            ui.Yield();
            host.pending = InspectorHost::Action::Select;
            ui.Yield();
            SelectContent(ui);
            IM_CHECK(ui.ItemExists("###InspectorAddComponent"));
            IM_CHECK(!ui.ItemExists("**/workspace.physics.profile"));
            IM_CHECK(!host.Box().components.rigidBody);
        });
    });
    INFO(result.testEngineLog);
    REQUIRE(result.Succeeded());
}
