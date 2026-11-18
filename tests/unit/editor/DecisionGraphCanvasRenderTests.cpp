#include "Horo/AI/DecisionGraphCompiler.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/screens/workspace/DecisionGraphCanvas.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <imgui.h>
#include <map>
#include <memory>
#include <span>

namespace Horo::Editor {
    namespace {
        /** @brief Provides long labels without any native localization/filesystem setup. */
        class CanvasLabels final : public ILocalizationService {
        public:
            const std::string &Get(std::string_view, std::string_view key) const override {
                return labels_.try_emplace(std::string{key}, "Localized graph label " + std::string{key}).first->second;
            }

        private:
            mutable std::map<std::string, std::string> labels_;
        };

        /** @brief Owns real offscreen ImGui and the production adapter in the correct destruction order. */
        struct CanvasFixture final {
            std::unique_ptr<ImGuiContext, void (*)(ImGuiContext *)> imgui{ImGui::CreateContext(), &ImGui::DestroyContext};
            EngineDataBus engine;
            EditorDataBus editor;
            CanvasLabels labels;
            Theme::Fonts fonts;
            ThemeContext theme{fonts};
            EditorSettingsSnapshot settings{.settings = DefaultEditorSettings()};
            EditorGuiContext context{engine, editor, labels, theme, settings};
            DecisionGraphCanvas canvas;

            CanvasFixture() {
                auto &io = ImGui::GetIO();
                io.DisplaySize = {1024, 768};
                io.DeltaTime = 1.0F / 60.0F;
                io.IniFilename = nullptr;
                auto *font = io.Fonts->AddFontDefault();
                REQUIRE(io.Fonts->Build());
                fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
                Theme::ApplyCurrentTheme();
            }

            void Draw(const DecisionGraphWorkspaceDocument &document, float width) {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({width, 650});
                ImGui::Begin("Decision canvas test", nullptr, ImGuiWindowFlags_NoSavedSettings);
                canvas.Retain(std::span{&document, 1});
                const auto interaction = canvas.Draw(document, context);
                CHECK_FALSE(interaction.edit.has_value());
                ImGui::End();
                ImGui::Render();
                const auto *draw = ImGui::GetDrawData();
                REQUIRE(draw);
                CHECK(draw->TotalVtxCount > 0);
                for (int list = 0; list < draw->CmdListsCount; ++list)
                    for (const auto &vertex : draw->CmdLists[list]->VtxBuffer) {
                        REQUIRE(std::isfinite(vertex.pos.x));
                        REQUIRE(std::isfinite(vertex.pos.y));
                    }
            }
        };
    }  // namespace

    TEST_CASE("Decision graph canvas renders native links across revisions and narrow widths", "[unit][editor][decision][canvas]") {
        CanvasFixture fixture;
        const auto first = AI::DecisionNodeId::Create(1).Value();
        const auto last = AI::DecisionNodeId::Create(2).Value();
        const auto edge = AI::DecisionNodeId::Create(3).Value();
        auto source = std::make_shared<AI::DecisionGraphSource>();
        source->body = AI::StateMachineAssetDescriptor{
            .initial = first,
            .states = {{.id = first}, {.id = last, .terminal = AI::DecisionStateTerminal::Succeeded}},
            .transitions = {{.id = edge, .source = first, .target = last}},
        };
        DecisionGraphWorkspaceDocument document;
        document.identity.instance = DocumentInstanceId::Create(5).Value();
        document.graph = source;
        document.revision = 1;
        document.editable = true;
        fixture.Draw(document, 920.0F);
        fixture.Draw(document, 280.0F);
        auto replacement = std::make_shared<AI::DecisionGraphSource>(*source);
        std::get<AI::StateMachineAssetDescriptor>(replacement->body).states.back().terminal = AI::DecisionStateTerminal::Failed;
        document.graph = std::move(replacement);
        ++document.revision;
        fixture.Draw(document, 280.0F);
        fixture.canvas.Retain({});
    }
}  // namespace Horo::Editor
