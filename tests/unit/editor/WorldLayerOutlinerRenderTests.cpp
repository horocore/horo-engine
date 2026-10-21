#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/modals/world_layer/CreateWorldLayerModal.h"
#include "editor/screens/workspace/WorldLayerOutliner.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <imgui_internal.h>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    namespace WS = WorldStreaming;

    /** @brief Owns only GUI context and immutable fixture state; no device or renderer is required. */
    struct LayerGuiHarness final {
        LayerGuiHarness()
            : localization(LocaleTag{"en-US"}), theme{fonts}, context{engineEvents, editorEvents, localization, theme, settings},
              modals(editorEvents, input) {
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = {640, 900};
            io.DeltaTime = 1.0F / 60.0F;
            auto *font = io.Fonts->AddFontDefault();
            static_cast<void>(io.Fonts->Build());
            fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
            for (const auto *locale : {"en-US", "tr-TR"})
                REQUIRE(localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                     (std::string{locale} + ".json")));
            Activate("en-US");
        }

        ~LayerGuiHarness() {
            modals.ForceDetachAllForShutdown();
            Theme::SetThemePreset(Theme::Preset::HoroDark);
            ImGui::DestroyContext();
        }

        void Activate(const char *locale) {
            REQUIRE(localization.Prepare(LocaleTag{locale}));
            REQUIRE(localization.ActivatePrepared());
        }

        EngineDataBus engineEvents;
        EditorDataBus editorEvents;
        LocalizationService localization;
        Theme::Fonts fonts;
        ThemeContext theme;
        EditorSettingsSnapshot settings;
        EditorGuiContext context;
        Input::InputRouter input;
        EditorModalHost modals;
    };

    PartitionedWorldSnapshot Source() {
        const auto grid = WS::WorldCellQuantizationPolicy::Create({}, 1000, {-1, 1, -1, 1, -1, 1}, 1).Value();
        PartitionedWorldSnapshot snapshot{.document = {.settings = {.grid = grid}}};
        snapshot.document.layers.push_back({.id = WS::StreamingLayerId::Create(0).Value(), .name = std::string(128, 'L')});
        snapshot.document.layers.push_back({.id = WS::StreamingLayerId::Create(17).Value(), .name = "Gameplay"});
        snapshot.currentLayer = snapshot.document.layers.front().id;
        return snapshot;
    }
}  // namespace

TEST_CASE("World layer outliner renders narrow localized long names without mutating its publication", "[editor][gui][layers]") {
    LayerGuiHarness harness;
    auto source = Source();
    for (const auto *locale : {"en-US", "tr-TR"}) {
        harness.Activate(locale);
        for (const auto preset : {Theme::Preset::HoroDark, Theme::Preset::Light}) {
            Theme::SetThemePreset(preset);
            for (const bool empty : {false, true}) {
                source.layerFilter.name = empty ? "unmatched" : "";
                for (int frame = 0; frame < 2; ++frame) {
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos({0, 0});
                    ImGui::SetNextWindowSize({300, 880});
                    ImGui::Begin("WorldLayerOutlinerTest", nullptr, ImGuiWindowFlags_NoResize);
                    const auto intent = DrawWorldLayerOutliner(source, harness.context);
                    CHECK(intent.action == WorldLayerOutlinerAction::None);
                    CHECK(intent.expected == source.fence);
                    CHECK_FALSE(intent.command.has_value());
                    const auto *window = ImGui::GetCurrentWindow();
                    CHECK(window->DC.CursorMaxPos.x <= window->WorkRect.Max.x);
                    CHECK(window->ScrollMax.x == 0);
                    ImGui::End();
                    ImGui::Render();
                    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
                }
            }
        }
    }
    CHECK(source.currentLayer == WS::StreamingLayerId::Create(0).Value());
    CHECK(source.document.layers.size() == 2);
    CHECK(source.layerUndoCount == 0);
    CHECK_FALSE(source.dirty);
}

TEST_CASE("World layer creation modal cancellation replacement and shutdown never submit a draft", "[editor][gui][layers]") {
    LayerGuiHarness harness;
    std::size_t submissions{};
    for (const auto *locale : {"en-US", "tr-TR"}) {
        harness.Activate(locale);
        REQUIRE(harness.modals
                    .OpenRoot(std::make_unique<CreateWorldLayerModal>(harness.context, WorldDocumentFence{},
                                                                      [&](std::string, WorldDocumentFence) {
            ++submissions;
            return Result<PartitionedWorldSnapshotLease>::Failure(MakeError(WorldDocumentErrors::Stale));
        })).HasValue());
        for (int frame = 0; frame < 2; ++frame) {
            ImGui::GetIO().DisplaySize = {320, 600};
            ImGui::NewFrame();
            harness.modals.Draw();
            ImGui::Render();
        }
        REQUIRE(harness.modals.TopModalId().has_value());
        REQUIRE(harness.modals.RequestClose(*harness.modals.TopModalId(), ModalCloseReason::Cancelled).HasValue());
        harness.modals.OnUpdate(0);
        CHECK_FALSE(harness.modals.HasOpenModal());
    }
    CHECK(submissions == 0);
    auto missing = std::make_unique<CreateWorldLayerModal>(harness.context, WorldDocumentFence{}, CreateWorldLayerModal::Create{});
    REQUIRE(harness.modals.OpenRoot(std::move(missing)).HasValue());
    harness.modals.OnUpdate(0);
    CHECK_FALSE(harness.modals.HasOpenModal());
}
