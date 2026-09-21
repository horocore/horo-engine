#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "Horo/Extensions/ExtensionMarketplace.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Network/NetworkProjectSettings.h"
#include "SettingsModalInternal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <imgui.h>
#include <memory>
#include <ranges>
#include <span>
#include <vector>

namespace Horo::Editor::SettingsModalInternal {
    using namespace Theme;
    using namespace Ui;
    using Theme::ScopedTextStyle;

    [[nodiscard]] const Extensions::ExtensionInventoryEntry *FindSelectedExtension(const SettingsState &st) {
        if (st.extensionInventory == nullptr)
            return nullptr;
        const auto &entries = st.extensionInventory->Entries();
        const auto selected = std::ranges::find(entries, st.selectedExtensionId, &Extensions::ExtensionInventoryEntry::packageId);
        return selected != entries.end() ? std::to_address(selected) : nullptr;
    }

    void DrawExtensionDetailSection(const char *label, const EditorGuiContext &ctx, const bool first) {
        if (!first) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0F);
            const ImVec2 divider = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(divider, {divider.x + ImGui::GetContentRegionAvail().x, divider.y}, U32(Border()), 1.0F);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0F);
        }

        ScopedTextStyle heading(ctx.theme.fonts.sansEmphasis, TextPx::Title(), FontPx::SansEmphasis);
        ImGui::PushStyleColor(ImGuiCol_Text, Text());
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0F);
    }

    void DrawExtensionDetailRow(const char *label, const char *value, const EditorGuiContext &ctx) {
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const ImVec2 itemSpacing = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{itemSpacing.x, 3.0F});
        {
            ScopedTextStyle title(ctx.theme.fonts.sans, TextPx::Body(), FontPx::Sans);
            ImGui::PushStyleColor(ImGuiCol_Text, Text());
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
        }
        if (value != nullptr && value[0] != '\0') {
            ScopedTextStyle detail(ctx.theme.fonts.sans, TextPx::Caption(), FontPx::Sans);
            ImGui::PushStyleColor(ImGuiCol_Text, Muted());
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + availableWidth);
            ImGui::TextWrapped("%s", value);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::PopStyleVar();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0F);
    }

    void DrawExtensionDetailValue(const char *value, const EditorGuiContext &ctx) {
        ScopedTextStyle detail(ctx.theme.fonts.sans, TextPx::Caption(), FontPx::Sans);
        ImGui::PushStyleColor(ImGuiCol_Text, Muted());
        ImGui::TextWrapped("%s", value);
        ImGui::PopStyleColor();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0F);
    }

    void DrawExtensionDetails(const SettingsState &st, const EditorGuiContext &ctx) {
        const Extensions::ExtensionInventoryEntry *entry = FindSelectedExtension(st);
        if (entry == nullptr) {
            ImGui::PushStyleColor(ImGuiCol_Text, Muted());
            ImGui::TextWrapped("%s", ctx.localization.Get("editor", "settings.extensions.select_prompt").c_str());
            ImGui::PopStyleColor();
            return;
        }

        if (!entry->loadError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Err());
            ImGui::TextWrapped("%s", entry->loadError.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy({0.0F, 12.0F});
        }

        DrawExtensionDetailSection(ctx.localization.Get("editor", "settings.extensions.modules").c_str(), ctx, true);
        if (entry->modules.empty()) {
            DrawExtensionDetailValue(ctx.localization.Get("editor", "settings.extensions.none").c_str(), ctx);
        }
        for (const auto &entryModule : entry->modules) {
            const std::string moduleDescription = entryModule.kind + "  ·  v" + entryModule.version;
            DrawExtensionDetailRow(entryModule.id.c_str(), moduleDescription.c_str(), ctx);
        }

        DrawExtensionDetailSection(ctx.localization.Get("editor", "settings.extensions.contributions").c_str(), ctx, false);
        if (entry->contributions.empty()) {
            DrawExtensionDetailValue(ctx.localization.Get("editor", "settings.extensions.none").c_str(), ctx);
        }
        for (const auto &contribution : entry->contributions) {
            DrawExtensionDetailRow(contribution.type.c_str(), contribution.id.c_str(), ctx);
        }

        DrawExtensionDetailSection(ctx.localization.Get("editor", "settings.extensions.tab.manifest").c_str(), ctx, false);
        if (entry->absoluteManifestPath.empty()) {
            DrawExtensionDetailValue(ctx.localization.Get("editor", "settings.extensions.builtin_manifest").c_str(), ctx);
        } else {
            DrawExtensionDetailValue(entry->absoluteManifestPath.string().c_str(), ctx);
        }
    }

    struct ExtensionCardLayout {
        ImVec2 min;
        ImVec2 max;
        float width;
        float toggleClusterWidth;
    };

    ExtensionCardLayout DrawExtensionCardFrame(SettingsState &st, const EditorGuiContext &ctx,
                                               const Extensions::ExtensionInventoryEntry &entry) {
        const float cardWidth = ImGui::GetContentRegionAvail().x;
        constexpr float cardHeight = 136.0F;
        const ImVec2 cardMin = ImGui::GetCursorScreenPos();
        const ImVec2 cardMax{cardMin.x + cardWidth, cardMin.y + cardHeight};
        const bool selected = st.selectedExtensionId == entry.packageId;
        auto *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(cardMin, cardMax, U32(selected ? ImVec4{Accent().x, Accent().y, Accent().z, 0.09F} : Bg1()),
                                Layout::Radius);
        drawList->AddRect(cardMin, cardMax, U32(selected ? ImVec4{Accent().x, Accent().y, Accent().z, 0.50F} : Border()), Layout::Radius);
        if (selected) {
            drawList->AddRectFilled(cardMin, {cardMin.x + 3.0F, cardMax.y}, U32(Accent()), Layout::Radius);
        }
        ImGui::InvisibleButton("card", {cardWidth, cardHeight});
        if (ImGui::IsItemClicked()) {
            st.selectedExtensionId = entry.packageId;
        }

        const ImVec2 dotCenter{cardMin.x + 18.0F, cardMin.y + 23.0F};
        drawList->AddCircleFilled(dotCenter, 8.0F,
                                  U32(entry.runtimeActive ? ImVec4{Ok().x, Ok().y, Ok().z, 0.14F}
                                                          : ImVec4{Dim().x, Dim().y, Dim().z, 0.12F}));
        drawList->AddCircleFilled(dotCenter, 4.5F, U32(entry.runtimeActive ? Ok() : Dim()));

        const std::string toggleLabel =
            ctx.localization.Get("editor", entry.enabled ? "settings.plugins.status.enabled" : "settings.plugins.status.disabled");
        float toggleLabelWidth = 0.0F;
        {
            ScopedTextStyle toggleText(ctx.theme.fonts.sans, TextPx::Label(), FontPx::Sans);
            toggleLabelWidth = ImGui::CalcTextSize(toggleLabel.c_str()).x;
        }
        const float toggleClusterWidth = 36.0F + 8.0F + toggleLabelWidth;
        return {.min = cardMin, .max = cardMax, .width = cardWidth, .toggleClusterWidth = toggleClusterWidth};
    }

    void DrawExtensionCardText(const EditorGuiContext &ctx, const Extensions::ExtensionInventoryEntry &entry,
                               const ExtensionCardLayout &card) {
        ImGui::SetCursorScreenPos({card.min.x + 34.0F, card.min.y + 13.0F});
        {
            ScopedTextStyle title(ctx.theme.fonts.sansEmphasis, TextPx::Title(), FontPx::SansEmphasis);
            ImGui::PushClipRect({card.min.x + 34.0F, card.min.y}, {card.max.x - card.toggleClusterWidth - 28.0F, card.min.y + 42.0F}, true);
            ImGui::TextUnformatted(entry.displayName.c_str());
            ImGui::PopClipRect();
        }

        ImGui::SetCursorScreenPos({card.min.x + 16.0F, card.min.y + 45.0F});
        ImGui::PushStyleColor(ImGuiCol_Text, Muted());
        ImGui::PushClipRect({card.min.x + 16.0F, card.min.y + 43.0F}, {card.max.x - 16.0F, card.max.y - 42.0F}, true);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card.width - 32.0F);
        ImGui::TextWrapped("%s", entry.description.empty() ? entry.packageId.c_str() : entry.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopClipRect();
        ImGui::PopStyleColor();
    }

    void DrawExtensionCardBadges(const EditorGuiContext &ctx, const Extensions::ExtensionInventoryEntry &entry,
                                 const ExtensionCardLayout &card) {
        using enum Horo::Editor::Ui::BadgeTone;
        ImGui::SetCursorScreenPos({card.min.x + 16.0F, card.max.y - 34.0F});
        ImGui::PushClipRect({card.min.x + 14.0F, card.max.y - 38.0F}, {card.max.x - 14.0F, card.max.y - 4.0F}, true);
        const char *originKey =
            entry.origin == Extensions::ExtensionOrigin::BuiltIn ? "settings.extensions.origin.builtin" : "settings.extensions.origin.user";
        Badge({.label = ("v" + entry.version).c_str(), .tone = Accent}, ctx.theme.fonts);
        Badge({.label = ctx.localization.Get("editor", originKey).c_str(), .tone = Neutral}, ctx.theme.fonts);
        if (entry.RestartRequired())
            Badge({.label = ctx.localization.Get("editor", "settings.extensions.restart_required").c_str(), .tone = Warning},
                  ctx.theme.fonts);
        ImGui::PopClipRect();
    }

    void DrawExtensionCardToggle(SettingsState &st, Extensions::ExtensionInventory &inventory, const EditorGuiContext &ctx,
                                 const Extensions::ExtensionInventoryEntry &entry, const ExtensionCardLayout &card) {
        bool enabled = entry.enabled;
        ImGui::SetCursorScreenPos({card.max.x - card.toggleClusterWidth - 16.0F, card.min.y + 12.0F});
        if (ToggleControl("##extension-enabled", &enabled, ctx.theme.fonts, false)) {
            if (Result<void> changed = inventory.SetEnabled(entry.packageId, enabled); changed.HasError()) {
                st.modalFeedback = changed.ErrorValue().message;
            } else {
                st.modalFeedback = ctx.localization.Get("editor", "settings.extensions.feedback.restart");
            }
        }
        ImGui::SameLine(0.0F, 8.0F);
        {
            ScopedTextStyle toggleText(ctx.theme.fonts.sans, TextPx::Label(), FontPx::Sans);
            ImGui::PushStyleColor(ImGuiCol_Text, enabled ? Text() : Muted());
            ImGui::TextUnformatted(
                ctx.localization.Get("editor", enabled ? "settings.plugins.status.enabled" : "settings.plugins.status.disabled").c_str());
            ImGui::PopStyleColor();
        }
    }

    void DrawExtensionCard(SettingsState &st, Extensions::ExtensionInventory &inventory, const EditorGuiContext &ctx,
                           const Extensions::ExtensionInventoryEntry &entry) {
        ImGui::PushID(entry.packageId.c_str());
        const ExtensionCardLayout card = DrawExtensionCardFrame(st, ctx, entry);
        DrawExtensionCardText(ctx, entry, card);
        DrawExtensionCardBadges(ctx, entry, card);
        DrawExtensionCardToggle(st, inventory, ctx, entry, card);
        ImGui::SetCursorScreenPos({card.min.x, card.max.y + 8.0F});
        ImGui::PopID();
    }

    void DrawExtensionListHeader(SettingsState &st, const EditorGuiContext &ctx,
                                 const std::vector<Extensions::ExtensionInventoryEntry> &entries) {
        {
            ScopedTextStyle heading(ctx.theme.fonts.sansEmphasis, TextPx::Label(), FontPx::SansEmphasis);
            ImGui::PushStyleColor(ImGuiCol_Text, Dim());
            ImGui::TextUnformatted(ctx.localization.Get("editor", "settings.extensions.installed_heading").c_str());
            ImGui::PopStyleColor();
        }
        const std::string totalLabel = std::format("{} {}", entries.size(), ctx.localization.Get("editor", "settings.extensions.total"));
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(totalLabel.c_str()).x);
        ImGui::PushStyleColor(ImGuiCol_Text, Muted());
        ImGui::TextUnformatted(totalLabel.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy({0.0F, 8.0F});

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10.0F, 7.0F});
        st.pluginFilter.resize(std::min(st.pluginFilter.size(), std::size_t{127}));
        st.pluginFilter.resize(127, '\0');
        const std::string filterHint = ctx.localization.Get("editor", "settings.extensions.filter");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##extension-filter", filterHint.c_str(), st.pluginFilter.data(), st.pluginFilter.size() + 1);
        st.pluginFilter.resize(st.pluginFilter.find('\0'));
        ImGui::PopStyleVar();
        ImGui::Dummy({0.0F, 12.0F});
    }

    void DrawExtensionListPane(SettingsState &st, Extensions::ExtensionInventory &inventory, const EditorGuiContext &ctx,
                               const std::vector<Extensions::ExtensionInventoryEntry> &entries, const float listWidth,
                               const float paneHeight) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{14.0F, 14.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg2());
        ImGui::BeginChild("ExtensionListPane", {listWidth, paneHeight}, true, ImGuiWindowFlags_AlwaysUseWindowPadding);
        DrawExtensionListHeader(st, ctx, entries);

        bool drewAny = false;
        for (const auto &entry : entries) {
            if (!ContainsCaseInsensitive(entry.displayName.c_str(), st.pluginFilter) &&
                !ContainsCaseInsensitive(entry.packageId.c_str(), st.pluginFilter))
                continue;
            drewAny = true;
            DrawExtensionCard(st, inventory, ctx, entry);
        }
        if (!drewAny) {
            ImGui::PushStyleColor(ImGuiCol_Text, Muted());
            ImGui::TextUnformatted(ctx.localization.Get("editor", "settings.extensions.empty").c_str());
            ImGui::PopStyleColor();
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void DrawExtensionCards(SettingsState &st, const EditorGuiContext &ctx) {
        Extensions::ExtensionInventory &inventory = *st.extensionInventory;
        const auto &entries = inventory.Entries();
        if (!entries.empty() &&
            std::ranges::find(entries, st.selectedExtensionId, &Extensions::ExtensionInventoryEntry::packageId) == entries.end())
            st.selectedExtensionId = entries.front().packageId;

        const float availableWidth = ImGui::GetContentRegionAvail().x;
        constexpr float paneGap = 14.0F;
        constexpr float minimumDetailWidth = 360.0F;
        const float maximumListWidth = std::max(250.0F, availableWidth - paneGap - minimumDetailWidth);
        const float listWidth = std::clamp(availableWidth * 0.42F, 250.0F, std::min(640.0F, maximumListWidth));
        const float paneHeight = std::max(0.0F, ImGui::GetContentRegionAvail().y);

        DrawExtensionListPane(st, inventory, ctx, entries, listWidth, paneHeight);

        ImGui::SameLine(0.0F, paneGap);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{8.0F, 0.0F});
        ImGui::BeginChild("ExtensionDetailPane", {0.0F, paneHeight}, false, ImGuiWindowFlags_AlwaysUseWindowPadding);
        DrawExtensionDetails(st, ctx);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    bool DrawMarketplaceSearch(SettingsState &st, const EditorGuiContext &ctx,
                               const Extensions::ExtensionMarketplaceSnapshot &marketplace) {
        constexpr float searchButtonWidth = 96.0F;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10.0F, 7.0F});
        ImGui::SetNextItemWidth(std::max(120.0F, ImGui::GetContentRegionAvail().x - searchButtonWidth - 8.0F));
        st.marketplaceQuery.resize(std::min(st.marketplaceQuery.size(), std::size_t{127}));
        st.marketplaceQuery.resize(127, '\0');
        const std::string searchHint = ctx.localization.Get("editor", "settings.extensions.marketplace.search");
        const bool submitted = ImGui::InputTextWithHint("##marketplace-search", searchHint.c_str(), st.marketplaceQuery.data(),
                                                        st.marketplaceQuery.size() + 1, ImGuiInputTextFlags_EnterReturnsTrue);
        st.marketplaceQuery.resize(st.marketplaceQuery.find('\0'));
        ImGui::SameLine(0.0F, 8.0F);
        const bool busy = marketplace.status == Extensions::ExtensionMarketplaceStatus::Searching ||
                          marketplace.status == Extensions::ExtensionMarketplaceStatus::Installing;
        ImGui::BeginDisabled(busy);
        const bool searchClicked = ImGui::Button(ctx.localization.Get("editor", "settings.extensions.marketplace.search.action").c_str(),
                                                 {searchButtonWidth, 0.0F});
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        if ((submitted || searchClicked) && !busy)
            static_cast<void>(st.extensionMarketplace->Search(st.marketplaceQuery));
        return busy;
    }

    bool DrawMarketplaceStatus(const EditorGuiContext &ctx, const Extensions::ExtensionMarketplaceSnapshot &marketplace) {
        if (marketplace.status == Extensions::ExtensionMarketplaceStatus::Searching) {
            ImGui::PushStyleColor(ImGuiCol_Text, Muted());
            ImGui::TextUnformatted(ctx.localization.Get("editor", "settings.extensions.marketplace.searching").c_str());
            ImGui::PopStyleColor();
            return false;
        }
        if (!marketplace.message.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, marketplace.status == Extensions::ExtensionMarketplaceStatus::Error ? Err() : Ok());
            ImGui::TextWrapped("%s", marketplace.message.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy({0.0F, 8.0F});
        }
        if (marketplace.entries.empty() && marketplace.status != Extensions::ExtensionMarketplaceStatus::Error) {
            ImGui::PushStyleColor(ImGuiCol_Text, Muted());
            ImGui::TextUnformatted(ctx.localization.Get("editor", "settings.extensions.marketplace.empty").c_str());
            ImGui::PopStyleColor();
            return false;
        }
        return true;
    }

    void DrawMarketplaceCard(SettingsState &st, const EditorGuiContext &ctx, const Extensions::ExtensionMarketplaceSnapshot &marketplace,
                             const Extensions::ExtensionMarketplaceEntry &entry, const bool busy) {
        ImGui::PushID(entry.packageId.c_str());
        const ImVec2 cardMin = ImGui::GetCursorScreenPos();
        const float cardWidth = ImGui::GetContentRegionAvail().x;
        constexpr float cardHeight = 104.0F;
        const ImVec2 cardMax{cardMin.x + cardWidth, cardMin.y + cardHeight};
        auto *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(cardMin, cardMax, U32(Bg2()), Layout::Radius);
        drawList->AddRect(cardMin, cardMax, U32(Border()), Layout::Radius);

        ImGui::SetCursorScreenPos({cardMin.x + 16.0F, cardMin.y + 13.0F});
        {
            ScopedTextStyle title(ctx.theme.fonts.sansEmphasis, TextPx::Title(), FontPx::SansEmphasis);
            ImGui::TextUnformatted(entry.displayName.c_str());
        }
        ImGui::SameLine(0.0F, 8.0F);
        Badge({.label = ("v" + entry.version).c_str(), .tone = BadgeTone::Accent}, ctx.theme.fonts);
        ImGui::SetCursorScreenPos({cardMin.x + 16.0F, cardMin.y + 42.0F});
        ImGui::PushStyleColor(ImGuiCol_Text, Muted());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardWidth - 154.0F);
        ImGui::TextWrapped("%s", entry.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos({cardMin.x + 16.0F, cardMax.y - 25.0F});
        ImGui::PushStyleColor(ImGuiCol_Text, Dim());
        ImGui::TextUnformatted(entry.author.c_str());
        ImGui::PopStyleColor();

        const bool installed = std::ranges::any_of(st.extensionInventory->Entries(), [&entry](const auto &item) {
            return item.packageId == entry.packageId;
        });
        const bool installing =
            marketplace.status == Extensions::ExtensionMarketplaceStatus::Installing && marketplace.activePackageId == entry.packageId;
        ImGui::SetCursorScreenPos({cardMax.x - 116.0F, cardMin.y + 35.0F});
        ImGui::BeginDisabled(installed || busy);
        const char *actionKey = "settings.plugins.action.install";
        if (installed) {
            actionKey = "settings.extensions.marketplace.installed";
        } else if (installing) {
            actionKey = "settings.extensions.marketplace.installing";
        }
        if (const std::string action = ctx.localization.Get("editor", actionKey); ImGui::Button(action.c_str(), {100.0F, 32.0F})) {
            const Result<void> started = st.extensionMarketplace->Install(entry.packageId);
            if (started.HasError())
                st.modalFeedback = started.ErrorValue().message;
        }
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos({cardMin.x, cardMax.y + 8.0F});
        ImGui::PopID();
    }

    void DrawExtensionMarketplace(SettingsState &st, const EditorGuiContext &ctx) {
        if (st.extensionMarketplace == nullptr) {
            ImGui::PushStyleColor(ImGuiCol_Text, Err());
            ImGui::TextWrapped("%s", ctx.localization.Get("editor", "settings.extensions.marketplace.unavailable").c_str());
            ImGui::PopStyleColor();
            return;
        }

        st.extensionMarketplace->Update();
        Extensions::ExtensionMarketplaceSnapshot marketplace = st.extensionMarketplace->Snapshot();
        if (marketplace.status == Extensions::ExtensionMarketplaceStatus::Idle) {
            static_cast<void>(st.extensionMarketplace->Search(st.marketplaceQuery));
            marketplace = st.extensionMarketplace->Snapshot();
        }

        const bool busy = DrawMarketplaceSearch(st, ctx, marketplace);
        ImGui::Dummy({0.0F, 12.0F});
        marketplace = st.extensionMarketplace->Snapshot();
        if (!DrawMarketplaceStatus(ctx, marketplace))
            return;

        ImGui::BeginChild("ExtensionMarketplaceResults", {0.0F, 0.0F}, false, ImGuiWindowFlags_AlwaysUseWindowPadding);
        for (const auto &entry : marketplace.entries)
            DrawMarketplaceCard(st, ctx, marketplace, entry, busy);
        ImGui::EndChild();
    }

    void DrawExtensionManager(SettingsState &st, const EditorGuiContext &ctx) {
        if (st.extensionInventory == nullptr) {
            ImGui::PushStyleColor(ImGuiCol_Text, Err());
            ImGui::TextWrapped("%s", ctx.localization.Get("editor", "settings.extensions.unavailable").c_str());
            ImGui::PopStyleColor();
            return;
        }

        DrawPluginSectionTabs(st, ctx);
        if (st.pluginSectionTab == 0)
            DrawExtensionCards(st, ctx);
        else
            DrawExtensionMarketplace(st, ctx);
    }

}  // namespace Horo::Editor::SettingsModalInternal
