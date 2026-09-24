#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserCards.h"

#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorTheme.h"
#include "editor/renderer/EditorGuiRenderer.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserPaneLayout.h"

#include <algorithm>
#include <string_view>
#include <unordered_set>

namespace Horo::Editor {
    namespace {
        constexpr float CardRadius = 6.0F;

        struct AssetCardPresentation {
            std::string_view name;
            ImU32 gradientStart;
            ImU32 gradientMid;
            ImU32 gradientEnd;
            Ui::UiIcon icon;
        };

        [[nodiscard]] AssetCardPresentation TintedPresentation(const std::string_view name, const ImVec4 tint, const float amount,
                                                               const Ui::UiIcon icon) {
            return {
                name,
                Theme::U32(Theme::Mix(Theme::Bg1(), tint, amount * 0.55F)),
                Theme::U32(Theme::Mix(Theme::Bg1(), tint, amount * 0.78F)),
                Theme::U32(Theme::Mix(Theme::Bg1(), tint, amount)),
                icon,
            };
        }

        [[nodiscard]] AssetCardPresentation PresentEntry(const ContentBrowserEntry &entry) {
            if (entry.kind == ContentBrowserEntryKind::Directory) {
                if (entry.displayName == "materials" || entry.displayName == "Materials") {
                    return TintedPresentation(entry.displayName, Theme::Accent(), 0.10F, Ui::UiIcon::Folder);
                }
                if (entry.displayName == "models" || entry.displayName == "Models") {
                    return TintedPresentation(entry.displayName, Theme::Accent(), 0.16F, Ui::UiIcon::Folder);
                }
                if (entry.displayName == "scenes" || entry.displayName == "Scenes") {
                    return TintedPresentation(entry.displayName, Theme::Ok(), 0.12F, Ui::UiIcon::Folder);
                }
                if (entry.displayName == "shaders" || entry.displayName == "Shaders") {
                    return TintedPresentation(entry.displayName, Theme::Muted(), 0.09F, Ui::UiIcon::Folder);
                }
                if (entry.displayName == "textures" || entry.displayName == "Textures") {
                    return TintedPresentation(entry.displayName, Theme::Warn(), 0.11F, Ui::UiIcon::Folder);
                }
                return TintedPresentation(entry.displayName, Theme::Accent(), 0.08F, Ui::UiIcon::Folder);
            }
            if (entry.previewFallback == Assets::AssetPreviewFallback::Image) {
                return TintedPresentation(entry.displayName, Theme::Err(), 0.09F, Ui::UiIcon::Image);
            }
            if (entry.previewFallback == Assets::AssetPreviewFallback::Audio) {
                return TintedPresentation(entry.displayName, Theme::Warn(), 0.10F, Ui::UiIcon::AudioFile);
            }
            if (entry.assetType.find("prefab") != std::string::npos) {
                return TintedPresentation(entry.displayName, Theme::Accent(), 0.09F, Ui::UiIcon::HierarchyGeneric);
            }
            if (entry.previewFallback == Assets::AssetPreviewFallback::Mesh) {
                return TintedPresentation(entry.displayName, Theme::Ok(), 0.11F, Ui::UiIcon::HierarchyMesh);
            }
            return TintedPresentation(entry.displayName, Theme::Accent(), 0.09F, Ui::UiIcon::Description);
        }

        [[nodiscard]] std::uint64_t PreviewFingerprint(const Assets::AssetPreviewImage &image) {
            std::uint64_t hash = 1469598103934665603ULL;
            const auto mix = [&hash](const std::uint8_t value) {
                hash ^= value;
                hash *= 1099511628211ULL;
            };
            for (unsigned shift = 0; shift < 32; shift += 8) {
                mix(static_cast<std::uint8_t>(image.width >> shift & 0xffU));
                mix(static_cast<std::uint8_t>(image.height >> shift & 0xffU));
            }
            for (const std::uint8_t value : image.pixels)
                mix(value);
            return hash;
        }
    }  // namespace

    /** @copydoc AssetBrowserCardRenderer::Attach */
    void AssetBrowserCardRenderer::Attach(IEditorGuiRenderer *renderer) noexcept {
        m_renderer = renderer;
    }

    /** @copydoc AssetBrowserCardRenderer::Detach */
    void AssetBrowserCardRenderer::Detach() noexcept {
        if (m_renderer != nullptr) {
            for (const auto &[path, preview] : m_previewTextures) {
                static_cast<void>(path);
                m_renderer->DestroyTexture(preview.second);
            }
        }
        m_previewTextures.clear();
        m_renderer = nullptr;
    }

    /** @copydoc AssetBrowserCardRenderer::RetainVisible */
    void AssetBrowserCardRenderer::RetainVisible(const ContentBrowserDirectory &directory, const std::vector<std::size_t> &visibleEntries) {
        if (m_renderer == nullptr || m_previewTextures.empty())
            return;
        std::unordered_set<std::string_view> visiblePaths;
        visiblePaths.reserve(visibleEntries.size());
        for (const std::size_t entryIndex : visibleEntries)
            visiblePaths.emplace(directory.entries[entryIndex].absolutePath);
        for (auto cached = m_previewTextures.begin(); cached != m_previewTextures.end();) {
            if (visiblePaths.contains(cached->first)) {
                ++cached;
                continue;
            }
            m_renderer->DestroyTexture(cached->second.second);
            cached = m_previewTextures.erase(cached);
        }
    }

    std::uintptr_t AssetBrowserCardRenderer::ResolvePreview(const ContentBrowserEntry &entry) {
        if (m_renderer == nullptr || !entry.previewImage.IsValid())
            return 0;
        const std::uint64_t fingerprint = PreviewFingerprint(entry.previewImage);
        if (const auto found = m_previewTextures.find(entry.absolutePath); found != m_previewTextures.end()) {
            if (found->second.first == fingerprint)
                return found->second.second;
            m_renderer->DestroyTexture(found->second.second);
            m_previewTextures.erase(found);
        }
        auto uploaded = m_renderer->CreateTexture(EditorRgba8ImageView{
            .width = entry.previewImage.width,
            .height = entry.previewImage.height,
            .pixels = entry.previewImage.pixels,
        });
        if (uploaded.HasError())
            return 0;
        const std::uintptr_t textureId = std::move(uploaded).Value();
        m_previewTextures.try_emplace(entry.absolutePath, fingerprint, textureId);
        return textureId;
    }

    /** @copydoc AssetBrowserCardRenderer::Draw */
    void AssetBrowserCardRenderer::Draw(const AssetBrowserCardDrawContext &drawContext, const ContentBrowserEntry &entry) {
        const auto &[drawList, font, iconFont, fontSize, cardMin, cardWidth, cardHeight, previewWidth, previewHeight, secondaryText,
                     listView, hovered, selected, dimmed] = drawContext;
        const AssetCardPresentation asset = PresentEntry(entry);
        const ImVec2 cardMax{cardMin.x + cardWidth, cardMin.y + cardHeight};
        const ImVec2 thumbMax{cardMin.x + previewWidth, cardMin.y + previewHeight};
        drawList->AddRectFilled(cardMin, cardMax, Theme::U32(Theme::CardSurface()), CardRadius);
        drawList->AddRectFilled(cardMin, thumbMax, asset.gradientStart, CardRadius,
                                listView ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersTop);
        drawList->AddRectFilledMultiColor({cardMin.x + 1.0F, cardMin.y + 1.0F}, {thumbMax.x - 1.0F, thumbMax.y}, asset.gradientStart,
                                          asset.gradientMid, asset.gradientMid, asset.gradientEnd);
        if (const std::uintptr_t previewTexture = ResolvePreview(entry); previewTexture != 0)
            drawList->AddImage(previewTexture, {cardMin.x + 3.0F, cardMin.y + 3.0F}, {thumbMax.x - 3.0F, thumbMax.y - 3.0F});
        else {
            const float iconSize = entry.kind == ContentBrowserEntryKind::Directory ? 34.0F : 20.0F;
            const ImU32 iconColor = Theme::U32(entry.kind == ContentBrowserEntryKind::Directory ? Theme::Muted() : Theme::Text());
            Ui::DrawEditorIcon(drawList, asset.icon,
                               {cardMin.x + (previewWidth - iconSize) * 0.5F, cardMin.y + (previewHeight - iconSize) * 0.5F},
                               {iconSize, iconSize}, iconColor, iconFont);
        }
        if (hovered) {
            ImVec4 hoverOverlay = Theme::Text();
            hoverOverlay.w = 0.04F;
            drawList->AddRectFilled(cardMin, thumbMax, Theme::U32(hoverOverlay), CardRadius,
                                    listView ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersTop);
        }
        if (dimmed) {
            ImVec4 dimOverlay = Theme::Bg0();
            dimOverlay.w = 0.46F;
            drawList->AddRectFilled(cardMin, cardMax, Theme::U32(dimOverlay), CardRadius);
            drawList->AddRect({cardMin.x + 2.0F, cardMin.y + 2.0F}, {cardMax.x - 2.0F, cardMax.y - 2.0F}, Theme::U32(Theme::Accent()),
                              CardRadius, ImDrawFlags_RoundCornersAll, 1.0F);
        }
        if (listView)
            drawList->AddLine({thumbMax.x, cardMin.y}, {thumbMax.x, cardMax.y}, Theme::U32(Theme::CardBorder()), 1.0F);
        else
            drawList->AddLine({cardMin.x, thumbMax.y}, thumbMax, Theme::U32(Theme::CardBorder()), 1.0F);
        ImU32 outlineColor = Theme::U32(Theme::CardBorder());
        if (selected)
            outlineColor = Theme::U32(Theme::Accent());
        else if (hovered)
            outlineColor = Theme::U32(Theme::BorderStrong());
        drawList->AddRect(cardMin, cardMax, outlineColor, CardRadius, ImDrawFlags_RoundCornersAll, selected ? 1.5F : 1.0F);
        const std::string name{asset.name};
        const float metaX = listView ? thumbMax.x + 12.0F : cardMin.x + 8.0F;
        const float metaY = listView ? cardMin.y + 7.0F : thumbMax.y + 6.0F;
        const ImVec4 clipRect{metaX, cardMin.y, cardMax.x - 8.0F, cardMax.y};
        drawList->AddText(font, fontSize, {metaX, metaY}, Theme::U32(Theme::Text()), name.c_str(), nullptr, 0.0F, &clipRect);
        drawList->AddText(font, AssetBrowserLayout::SecondaryFontSize(), {metaX, metaY + fontSize + 2.0F}, Theme::U32(Theme::Dim()),
                          secondaryText.data(), secondaryText.data() + secondaryText.size(), 0.0F, &clipRect);
    }
}  // namespace Horo::Editor
