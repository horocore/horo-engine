/** @file SourceEditorWidgetFixture.h
 * @brief Shared actual-widget fixture for interaction and lexical rendering regression coverage.
 */
#pragma once
#include "Horo/Editor/SourceFileOpenService.h"
#include "SourceEditorWidget.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "support/OwnedTestDirectory.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace Horo::Editor::Tests {
    /** @brief Drives the actual Zep adapter with real immutable service publications and ImGui input frames. */
    struct SourceEditorWidgetFixture final {
        HeadlessEditorGuiFixture gui;
        Horo::Tests::OwnedTestDirectory directory{"source-widget"};
        SourceFileOpenService documents{directory.Path()};
        SourceEditorWidget first{SourceEditingViewId{1}};
        SourceEditorWidget second{SourceEditingViewId{2}};
        std::string clipboard;

        SourceEditorWidgetFixture() {
            ImGui::GetIO().ConfigMacOSXBehaviors = false;
            ImGui::GetPlatformIO().Platform_ClipboardUserData = &clipboard;
            ImGui::GetPlatformIO().Platform_GetClipboardTextFn = [](ImGuiContext *context) {
                return static_cast<std::string *>(context->PlatformIO.Platform_ClipboardUserData)->c_str();
            };
            ImGui::GetPlatformIO().Platform_SetClipboardTextFn = [](ImGuiContext *context, const char *text) {
                *static_cast<std::string *>(context->PlatformIO.Platform_ClipboardUserData) = text;
            };
        }

        SourceDocumentSnapshot Open(const char *name, const std::string_view text) {
            {
                std::ofstream file(directory.Path() / name, std::ios::binary);
                file.write(text.data(), static_cast<std::streamsize>(text.size()));
                REQUIRE(file.good());
            }
            const auto opened = documents.Open({.path = name, .mode = SourceOpenMode::EmbeddedOnly});
            REQUIRE(opened.HasValue());
            REQUIRE(opened.Value().sourceSnapshot);
            return *opened.Value().sourceSnapshot;
        }

        std::optional<SourceEditingCommand> Frame(SourceEditorWidget &widget, const bool focus = true, const float scale = 1.0F,
                                                  const bool enabled = true, const Theme::Fonts *fonts = nullptr) {
            gui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({700, 500});
            ImGui::Begin("Widget test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
            const auto intent = widget.Draw({620, 400, scale, focus, enabled}, fonts);
            ImGui::End();
            gui.EndFrame();
            return intent;
        }

        SourceDocumentSnapshot Apply(const SourceEditingCommand &intent) {
            const auto *edit = std::get_if<SourceEditSubmission>(&intent);
            REQUIRE(edit);
            const auto result = SubmitSourceEditingCommand(documents.Documents(), *edit);
            REQUIRE(result.HasValue());
            return result.Value().snapshot;
        }

        std::optional<SourceEditingCommand> Shortcut(SourceEditorWidget &widget, const ImGuiKey key) {
            auto &io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(key, true);
            const auto intent = Frame(widget);
            io.AddKeyEvent(key, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            static_cast<void>(Frame(widget));
            return intent;
        }
    };

}  // namespace Horo::Editor::Tests
