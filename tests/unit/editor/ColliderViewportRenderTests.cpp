#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Foundation/DataBus.h"
#include "ViewportPanelRenderSupport.h"
#include "editor/screens/workspace/panels/viewport/ViewportPanel.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <optional>

namespace {
    using TestLocalization = Horo::Editor::Tests::ViewportRenderSupport::TestLocalization;
    using FakeViewportRenderer = Horo::Editor::Tests::ViewportRenderSupport::FakeViewportRenderer;
}  // namespace

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Own ImGui before any fixture member requiring GUI state and release it last. */
    struct ColliderUiLifetime {
        ColliderUiLifetime() {
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.DisplaySize = {640, 480};
            io.DeltaTime = 1.0F / 60;
            io.Fonts->AddFontDefault();
            static_cast<void>(io.Fonts->Build());
        }

        ~ColliderUiLifetime() {
            ImGui::DestroyContext();
        }

        ColliderUiLifetime(const ColliderUiLifetime &) = delete;
        ColliderUiLifetime &operator=(const ColliderUiLifetime &) = delete;
    };

    /** @brief Exercise the real viewport/capture adapters against the canonical Scene transaction owner. */
    struct ColliderViewportFixture final : ColliderUiLifetime {
        EngineDataBus engine;
        EditorDataBus events;
        TestLocalization localization;
        Theme::Fonts fonts;
        ThemeContext theme{.fonts = fonts};
        EditorSettingsSnapshot settings;
        std::optional<EditorGuiContext> gui;
        std::optional<SceneDocumentTransactions> port;
        SceneTransactionSnapshot source;
        EditorWorkspaceViewModel view;
        Input::InputRouter router;
        Input::RawInputCollector collector;
        Input::InputContextToken workspace;
        std::optional<Input::InputContextToken> modal;
        FakeViewportRenderer renderer;
        ViewportPanel panel;
        Input::FrameNumber frame{1};
        EditorWorkspaceViewCommandData command;

        /** @brief Compose existing GUI and owner dependencies; renderer fake supplies only a texture surface. */
        ColliderViewportFixture() {
            ImFont *font = ImGui::GetIO().Fonts->Fonts.front();
            fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font};
            gui.emplace(EditorGuiContext{.engineEvents = engine,
                                         .editorEvents = events,
                                         .localization = localization,
                                         .theme = theme,
                                         .settings = settings});
            PrepareSource();
            workspace = router.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
            PanelContext context{.dataBus = events,
                                 .viewportRenderer = &renderer,
                                 .inputRouter = &router,
                                 .workspaceInputContext = &workspace};
            panel.OnAttach(context);
        }

        ~ColliderViewportFixture() {
            panel.OnDetach();
        }

        ColliderViewportFixture(const ColliderViewportFixture &) = delete;
        ColliderViewportFixture &operator=(const ColliderViewportFixture &) = delete;

        /** @brief Project valid authored collider values through production source contracts. */
        void PrepareSource() {
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
            auto created = SceneDocumentTransactions::Create({asset, 1}, {object});
            REQUIRE(created.HasValue());
            port.emplace(std::move(created).Value());
            source = port->Snapshot().Value();
            view.physicsContext = {source.session, source.revision, source.state, true, {}};
            view.viewportColliders = ProjectPhysicsColliders(source.objects).Value();
            view.selectedColliders = {view.viewportColliders.front().target};
            view.activeTransformTool = EditorTransformTool::Move;
        }

        /** @brief Route logical input through an actual ViewportPanel draw frame. */
        void Draw() {
            command = {};
            ImGui::NewFrame();
            const auto &io = ImGui::GetIO();
            collector.BeginFrame(frame++);
            collector.SetPointerPosition(io.MousePos.x, io.MousePos.y);
            collector.SetPointerButton(Input::PointerButton::Primary, io.MouseDown[ImGuiMouseButton_Left]);
            collector.SetKey(Input::Key::Escape, ImGui::IsKeyDown(ImGuiKey_Escape));
            router.BeginFrame(collector.Commit());
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({420, 320});
            ImGui::Begin("ColliderViewport", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
            panel.DrawPanel(ImGui::GetCursorScreenPos(), {400, 280}, view, command, *gui);
            ImGui::End();
            ImGui::Render();
        }

        /** @brief Begin and advance a real axis drag, verifying preview does not become authored state. */
        void BeginGesture() {
            auto &io = ImGui::GetIO();
            io.AddFocusEvent(true);
            io.AddMousePosEvent(230, 148);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            Draw();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            Draw();
            io.AddMousePosEvent(250, 148);
            Draw();
            CHECK(command.command == EditorWorkspaceViewCommand::None);
            CHECK(port->Snapshot().Value().revision == source.revision);
        }

        /** @brief Verify one release intent commits through the real Scene owner exactly once. */
        void CheckCommit() {
            REQUIRE(command.command == EditorWorkspaceViewCommand::CommitPhysicsColliders);
            REQUIRE(command.colliderPayload);
            REQUIRE(command.colliderPayload->edits.size() == 1);
            CHECK(command.colliderPayload->context.session == source.session);
            CHECK(command.colliderPayload->edits.front().transform.translation.x > 0);
            const auto prepared = PrepareColliderTransformEdits(source, command.colliderPayload->edits);
            REQUIRE(prepared.HasValue());
            REQUIRE(port->Execute(command.colliderPayload->context, prepared.Value()).HasValue());
            CHECK(port->Snapshot().Value().revision == source.revision + 1);
        }
    };

    /** @brief Runtime-generated scenarios avoid assumptions about Catch2 SECTION control flow in static analysis. */
    enum class ColliderViewportScenario {
        Normal,
        HighDpi,
        Modal,
        Reload,
        Escape,
        Detach,
        Unavailable
    };
}  // namespace

TEST_CASE("Collider gestures pass through the real viewport in logical pixels and cancel with capture",
          "[unit][editor][physics][collider]") {
    const auto scenario = GENERATE(ColliderViewportScenario::Normal, ColliderViewportScenario::HighDpi, ColliderViewportScenario::Modal,
                                   ColliderViewportScenario::Reload, ColliderViewportScenario::Escape, ColliderViewportScenario::Detach,
                                   ColliderViewportScenario::Unavailable);
    ColliderViewportFixture fixture;
    const float dpi = scenario == ColliderViewportScenario::HighDpi ? 2.0F : 1.0F;
    auto &io = ImGui::GetIO();
    io.DisplayFramebufferScale = {dpi, dpi};
    fixture.renderer.depthRange =
        scenario == ColliderViewportScenario::HighDpi ? Math::ClipDepthRange::ZeroToOne : Math::ClipDepthRange::NegativeOneToOne;
    fixture.BeginGesture();
    switch (scenario) {
        case ColliderViewportScenario::Modal:
            fixture.modal.emplace(fixture.router.PushContext(Input::InputContextId{"test.modal"}, Input::InputContextKind::ModalRoot));
            fixture.Draw();
            break;
        case ColliderViewportScenario::Reload:
            ++fixture.view.physicsContext.session.generation;
            fixture.Draw();
            break;
        case ColliderViewportScenario::Escape:
            io.AddKeyEvent(ImGuiKey_Escape, true);
            fixture.Draw();
            break;
        case ColliderViewportScenario::Detach:
            fixture.panel.OnDetach();
            break;
        case ColliderViewportScenario::Unavailable:
            fixture.renderer.ready = false;
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            fixture.Draw();
            fixture.renderer.ready = true;
            break;
        default:
            break;
    }
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    fixture.Draw();
    if (scenario == ColliderViewportScenario::Normal || scenario == ColliderViewportScenario::HighDpi)
        fixture.CheckCommit();
    else
        CHECK(fixture.command.command == EditorWorkspaceViewCommand::None);
    if (scenario == ColliderViewportScenario::Unavailable)
        CHECK(fixture.router.IsContextActive(fixture.workspace));
    CHECK(fixture.renderer.requestedExtent.width == static_cast<std::uint32_t>(400 * dpi));
    CHECK(fixture.renderer.requestedExtent.height == static_cast<std::uint32_t>(280 * dpi));
}

TEST_CASE("Collider movement is invariant under framebuffer density and clip convention", "[unit][editor][physics][collider]") {
    std::array<float, 2> translations{};
    for (std::size_t index = 0; index < translations.size(); ++index) {
        ColliderViewportFixture fixture;
        const float dpi = static_cast<float>(index + 1);
        ImGui::GetIO().DisplayFramebufferScale = {dpi, dpi};
        fixture.renderer.depthRange = index == 0 ? Math::ClipDepthRange::NegativeOneToOne : Math::ClipDepthRange::ZeroToOne;
        fixture.BeginGesture();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        fixture.Draw();
        fixture.CheckCommit();
        translations[index] = fixture.command.colliderPayload->edits.front().transform.translation.x;
    }
    CHECK(Math::NearlyEqual(translations[0], translations[1], 1.0e-5F));
}
