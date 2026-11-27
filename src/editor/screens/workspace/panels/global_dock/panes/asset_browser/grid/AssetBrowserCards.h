#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct ImDrawList;
struct ImFont;
struct ImVec2;

namespace Horo::Editor {
    class IEditorGuiRenderer;
    struct ContentBrowserDirectory;
    struct ContentBrowserEntry;

    struct AssetBrowserCardDrawContext {
        ImDrawList *drawList;
        ImFont *font;
        ImFont *iconFont;
        float fontSize;
        const ImVec2 &cardMin;
        float cardWidth;
        float cardHeight;
        float previewWidth;
        float previewHeight;
        std::string_view secondaryText;
        bool listView;
        bool hovered;
        bool selected;
        bool dimmed;
    };

    /** @brief Owns preview textures and renders Asset Browser cards. */
    class AssetBrowserCardRenderer {
    public:
        /** @brief Binds the renderer used to create preview textures. */
        void Attach(IEditorGuiRenderer *renderer) noexcept;

        /** @brief Destroys every owned preview texture and releases the renderer binding. */
        void Detach() noexcept;

        /**
         * @brief Releases cached previews that are no longer represented by visible entries.
         * @param directory Current directory projection.
         * @param visibleEntries Indices of entries that remain visible.
         */
        void RetainVisible(const ContentBrowserDirectory &directory, const std::vector<std::size_t> &visibleEntries);

        /** @brief Draws one folder or asset card, resolving its preview texture when available. */
        void Draw(const AssetBrowserCardDrawContext &drawContext, const ContentBrowserEntry &entry);

    private:
        [[nodiscard]] std::uintptr_t ResolvePreview(const ContentBrowserEntry &entry);

        IEditorGuiRenderer *m_renderer{nullptr};

        struct TransparentStringHash {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(const std::string_view value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }
        };

        std::unordered_map<std::string, std::pair<std::uint64_t, std::uintptr_t>, TransparentStringHash, std::equal_to<>> m_previewTextures;
    };
}  // namespace Horo::Editor
