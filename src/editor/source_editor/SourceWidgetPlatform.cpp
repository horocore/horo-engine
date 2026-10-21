#include "SourceWidgetPlatform.h"

#include "Horo/Foundation/Utf8.h"
#include "SourceEditorWidget.h"
#include "SourceWidgetText.h"

#include <cmath>
#include <vector>
#include <zep/filesystem.h>
#include <zep/window.h>

namespace Horo::Editor {
    /** @copydoc ValidSourceWidgetFrame */
    bool ValidSourceWidgetFrame(const SourceWidgetFrame &frame) noexcept {
        return std::isfinite(frame.width) && std::isfinite(frame.height) && frame.width > 0 && frame.height > 0 &&
               std::isfinite(frame.dpiScale) && frame.dpiScale > 0;
    }

    namespace {
        /** @brief Validates one external marker before replacing any existing decoration. */
        bool ValidDecoration(const SourceDocumentSnapshot &source, const SourceWidgetDecoration &decoration) {
            return decoration.begin <= decoration.end && SourceWidgetText::Boundary(source, decoration.begin) &&
                   SourceWidgetText::Boundary(source, decoration.end) && decoration.description.size() <= 4096 &&
                   IsValidUtf8ScalarSequence(decoration.description);
        }

        /** @brief Keeps semantic marker color selection outside range validation. */
        Zep::ThemeColor DecorationColor(const SourceWidgetDecorationKind kind) {
            if (kind == SourceWidgetDecorationKind::Error)
                return Zep::ThemeColor::Error;
            return kind == SourceWidgetDecorationKind::Warning ? Zep::ThemeColor::Warning : Zep::ThemeColor::Info;
        }

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

    /** @copydoc PublishSourceWidgetDecorations */
    bool PublishSourceWidgetDecorations(Zep::ZepBuffer &buffer, const SourceDocumentSnapshot &source,
                                        const std::span<const SourceWidgetDecoration> decorations) {
        for (const auto &decoration : decorations) {
            if (!ValidDecoration(source, decoration))
                return false;
        }
        std::vector<std::shared_ptr<Zep::RangeMarker>> markers;
        markers.reserve(decorations.size());
        for (const auto &decoration : decorations) {
            auto marker = std::make_shared<Zep::RangeMarker>(buffer);
            marker->SetRange({static_cast<long>(SourceWidgetText::DisplayOffset(source, decoration.begin)),
                              static_cast<long>(SourceWidgetText::DisplayOffset(source, decoration.end))});
            marker->SetDescription(decoration.description);
            marker->displayType = Zep::RangeMarkerDisplayType::Underline | Zep::RangeMarkerDisplayType::Tooltip;
            marker->SetHighlightColor(DecorationColor(decoration.kind));
            markers.push_back(std::move(marker));
        }
        buffer.ClearRangeMarkers(Zep::RangeMarkerType::All);
        for (auto &marker : markers)
            buffer.AddRangeMarker(std::move(marker));
        return true;
    }

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

}  // namespace Horo::Editor
