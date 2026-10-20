#include "SourceWidgetPlatform.h"

#include <zep/window.h>

namespace Horo::Editor {
    namespace {
        /** @brief Denies every ambient filesystem path, including Zep configuration and save shortcuts. */
        class DeniedFileSystem final : public Zep::IZepFileSystem {
        public:
            std::string Read(const Zep::fs::path &) override {
                return {};
            }

            bool Write(const Zep::fs::path &, const void *, std::size_t) override {
                return false;
            }

            Zep::fs::path GetConfigPath() const override {
                return {};
            }

            Zep::fs::path GetSearchRoot(const Zep::fs::path &, bool &foundGit) const override {
                foundGit = false;
                return {};
            }

            const Zep::fs::path &GetWorkingDirectory() const override {
                return empty_;
            }

            void SetWorkingDirectory(const Zep::fs::path &) override {}

            bool MakeDirectories(const Zep::fs::path &) override {
                return false;
            }

            bool IsDirectory(const Zep::fs::path &) const override {
                return false;
            }

            bool IsReadOnly(const Zep::fs::path &) const override {
                return true;
            }

            bool Exists(const Zep::fs::path &) const override {
                return false;
            }

            void ScanDirectory(const Zep::fs::path &, std::function<bool(const Zep::fs::path &, bool &)>) const override {}

            bool Equivalent(const Zep::fs::path &a, const Zep::fs::path &b) const override {
                return a == b;
            }

            Zep::fs::path Canonical(const Zep::fs::path &path) const override {
                return path;
            }

            void SetFlags(std::uint32_t) override {}

        private:
            Zep::fs::path empty_;
        };

    }  // namespace

    /** @copydoc WidgetMode::Select */
    void WidgetMode::Select(Zep::ZepBuffer &buffer, const Zep::GlyphIterator anchor, const Zep::GlyphIterator caret) {
        SwitchMode(anchor == caret ? Zep::EditorMode::Insert : Zep::EditorMode::Visual);
        m_visualBegin = anchor;
        m_visualEnd = caret;
        GetCurrentWindow()->SetBufferCursor(caret);
        if (anchor == caret)
            buffer.ClearSelection();
        else
            buffer.SetSelection(GetInclusiveVisualRange());
    }

    /** @copydoc MakeSourceWidgetEditor */
    std::unique_ptr<Zep::ZepEditor> MakeSourceWidgetEditor() {
        auto display = std::make_unique<WidgetDisplay>();
        auto files = std::make_unique<DeniedFileSystem>();
        auto editor = std::make_unique<Zep::ZepEditor>(display.get(), Zep::fs::path{}, Zep::ZepEditorFlags::DisableThreads, files.get());
        // Successful construction transfers these owners to Zep; guards protect failures before that boundary.
        static_cast<void>(display.release());
        static_cast<void>(files.release());
        return editor;
    }

    /** @copydoc UpdateSourceWidgetTheme */
    void UpdateSourceWidgetTheme(Zep::ZepEditor &editor) {
        auto &theme = editor.GetTheme();
        for (int value = 1; value < static_cast<int>(Zep::ThemeColor::UniqueColor0); ++value)
            theme.SetColor(static_cast<Zep::ThemeColor>(value), Zep::toNVec4f(ImGui::GetStyleColorVec4(ImGuiCol_Text)));
        for (const auto color : {Zep::ThemeColor::Background, Zep::ThemeColor::LineNumberBackground, Zep::ThemeColor::AirlineBackground,
                                 Zep::ThemeColor::TabInactive})
            theme.SetColor(color, Zep::toNVec4f(ImGui::GetStyleColorVec4(ImGuiCol_ChildBg)));
        for (const auto color : {Zep::ThemeColor::TextDim, Zep::ThemeColor::LineNumber, Zep::ThemeColor::HiddenText})
            theme.SetColor(color, Zep::toNVec4f(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)));
        theme.SetColor(Zep::ThemeColor::VisualSelectBackground, Zep::toNVec4f(ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg)));
        theme.SetColor(Zep::ThemeColor::CursorLineBackground, Zep::toNVec4f(ImGui::GetStyleColorVec4(ImGuiCol_Header)));
    }
}  // namespace Horo::Editor
