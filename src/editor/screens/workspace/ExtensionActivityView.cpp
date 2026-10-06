#include "ExtensionActivityView.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/renderer/EditorGuiRenderer.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>

namespace Horo::Editor {
    ExtensionActivityView::ExtensionActivityView(const EditorGuiContext &context, Extensions::EditorActivityHost *host,
                                                 IEditorGuiRenderer *renderer)
        : context_(context), host_(host), renderer_(renderer) {}

    ExtensionActivityView::~ExtensionActivityView() {
        if (renderer_)
            for (const auto &entry : entries_)
                if (entry.texture)
                    renderer_->DestroyTexture(entry.texture);
    }

    /** @copydoc ExtensionActivityView::ApplyPendingMove */
    void ExtensionActivityView::ApplyPendingMove() {
        const auto move = std::exchange(pendingMove_, std::nullopt);
        if (!move)
            return;
        if (move->source.owner == reinterpret_cast<std::uintptr_t>(this) && move->source.revision == revision_ &&
            revision_ == host_->Registry().Revision()) {
            const auto entry = std::ranges::find(entries_, move->source.token, &Entry::token);
            if (entry != entries_.end() && host_->IsLive(entry->projection.surface.descriptor.provider)) {
                const auto moved = host_->Registry().MoveActivity(entry->projection.surface.descriptor.provider,
                                                                  entry->projection.surface.descriptor.id, move->target);
                if (moved.HasValue() && entry->projection.surface.open)
                    nativePanelClear_[static_cast<std::size_t>(move->target.side)] = entry->token;
            }
        }
        // Even rejected/stale commands pump the existing owner update boundary.
        host_->Update();
    }

    /** @copydoc ExtensionActivityView::PrepareEntry */
    ExtensionActivityView::Entry ExtensionActivityView::PrepareEntry(const Extensions::EditorActivityProjection &projection,
                                                                     const std::string &locale) {
        const auto previous = std::ranges::find_if(entries_, [&projection](const auto &entry) {
            return entry.projection.surface.descriptor.id == projection.surface.descriptor.id &&
                   entry.projection.surface.descriptor.provider == projection.surface.descriptor.provider;
        });
        Entry entry{.projection = projection};
        PrepareResources(entry, previous != entries_.end() ? std::to_address(previous) : nullptr);
        const auto &descriptor = projection.surface.descriptor;
        entry.label = host_->LocalizedText(descriptor.provider, descriptor.labelLocalizationKey, locale);
        entry.tooltip = host_->LocalizedText(descriptor.provider, descriptor.tooltipLocalizationKey, locale);
        PrepareNodeText(entry, locale);
        return entry;
    }

    /** @copydoc ExtensionActivityView::PrepareResources */
    void ExtensionActivityView::PrepareResources(Entry &entry, Entry *previous) {
        entry.token = previous != nullptr ? previous->token : nextToken_++;
        if (previous != nullptr) {
            entry.texture = std::exchange(previous->texture, 0);
            entry.focusPending =
                previous->focusPending ||
                (entry.projection.surface.focused &&
                 (!previous->projection.surface.focused || !previous->projection.surface.open ||
                  previous->projection.surface.descriptor.activity->side != entry.projection.surface.descriptor.activity->side));
        } else {
            entry.focusPending = entry.projection.surface.focused;
            if (renderer_ && entry.projection.icon) {
                const auto &icon = *entry.projection.icon;
                if (auto uploaded = renderer_->CreateTexture({icon.width, icon.height, icon.pixels}); uploaded.HasValue())
                    entry.texture = uploaded.Value();
            }
        }
        if (entry.projection.surface.open &&
            (previous == nullptr || !previous->projection.surface.open ||
             previous->projection.surface.descriptor.activity->side != entry.projection.surface.descriptor.activity->side))
            nativePanelClear_[static_cast<std::size_t>(entry.projection.surface.descriptor.activity->side)] = entry.token;
    }

    /** @copydoc ExtensionActivityView::PrepareNodeText */
    void ExtensionActivityView::PrepareNodeText(Entry &entry, const std::string &locale) {
        if (entry.projection.surface.form) {
            for (const auto &node : entry.projection.surface.form->nodes) {
                const auto &base = Extensions::EditorUiNodeBaseOf(node);
                std::string text{host_->LocalizedText(entry.projection.surface.descriptor.provider, base.label.value, locale)};
                std::visit([&](const auto &typed) {
                    if constexpr (requires { typed.text; }) {
                        text =
                            typed.text.kind == Extensions::EditorUiTextKind::TechnicalText
                                ? typed.text.value
                                : std::string{host_->LocalizedText(entry.projection.surface.descriptor.provider, typed.text.value, locale)};
                    }
                }, node.payload);
                entry.nodeText.push_back(std::move(text));
            }
        }
    }

    /** @copydoc ExtensionActivityView::DrawPlacementMenu */
    void ExtensionActivityView::DrawPlacementMenu(const Entry &entry, const std::size_t group) {
        const auto &surface = entry.projection.surface;
        if (ImGui::BeginPopupContextItem("##Placement")) {
            for (std::size_t side = 0; side < moveLabels_.size(); ++side)
                if (Ui::Button({.label = moveLabels_[side].c_str(), .font = context_.theme.fonts.sans})) {
                    const auto destination = static_cast<Extensions::EditorActivitySide>(side);
                    static_cast<void>(QueueMove(surface.descriptor.provider, surface.descriptor.id,
                                                {destination, static_cast<std::uint8_t>(group),
                                                 GroupSize(destination, static_cast<std::uint8_t>(group))}));
                    ImGui::CloseCurrentPopup();
                }
            if (Ui::Button({.label = moveEarlierLabel_.c_str(), .enabled = entry.ordinal > 0, .font = context_.theme.fonts.sans})) {
                static_cast<void>(QueueMove(surface.descriptor.provider, surface.descriptor.id,
                                            {surface.descriptor.activity->side, static_cast<std::uint8_t>(group), entry.ordinal - 1}));
                ImGui::CloseCurrentPopup();
            }
            if (Ui::Button({.label = moveLaterLabel_.c_str(),
                            .enabled = entry.ordinal + 1 < GroupSize(surface.descriptor.activity->side, static_cast<std::uint8_t>(group)),
                            .font = context_.theme.fonts.sans})) {
                static_cast<void>(QueueMove(surface.descriptor.provider, surface.descriptor.id,
                                            {surface.descriptor.activity->side, static_cast<std::uint8_t>(group), entry.ordinal + 2}));
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    /** @copydoc ExtensionActivityView::DrawVisibilityMenu */
    void ExtensionActivityView::DrawVisibilityMenu(const bool right) {
        if (ImGui::BeginPopupContextWindow("##ExtensionActivities", ImGuiPopupFlags_MouseButtonRight)) {
            for (const auto &entry : entries_) {
                const auto &surface = entry.projection.surface;
                if ((surface.descriptor.activity->side == Extensions::EditorActivitySide::Right) != right)
                    continue;
                bool visible = surface.activity.visible;
                ImGui::PushID(surface.descriptor.id.c_str());
                if (Ui::CheckboxControl(entry.label.c_str(), &visible, context_.theme.fonts))
                    static_cast<void>(host_->Registry().SetActivityVisibility(surface.descriptor.id, visible));
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    }

    void ExtensionActivityView::Update() {
        if (!host_)
            return;
        ApplyPendingMove();
        const auto revision = host_->Registry().Revision();
        const auto &locale = context_.settings.settings.languageTag;
        if (revision == revision_ && locale == locale_)
            return;
        std::vector<Entry> next;
        next.reserve(host_->Prepared().size());
        for (const auto &projection : host_->Prepared()) {
            next.push_back(PrepareEntry(projection, locale));
        }
        if (renderer_)
            for (const auto &old : entries_)
                if (old.texture)
                    renderer_->DestroyTexture(old.texture);
        std::ranges::sort(next, [](const Entry &a, const Entry &b) {
            const auto &left = a.projection.surface.descriptor;
            const auto &right = b.projection.surface.descriptor;
            return std::tie(left.activity->side, left.activity->group, left.placement.order, left.id) <
                   std::tie(right.activity->side, right.activity->group, right.placement.order, right.id);
        });
        std::int32_t ordinal{};
        for (std::size_t index = 0; index < next.size(); ++index) {
            if (const auto &activity = *next[index].projection.surface.descriptor.activity;
                index == 0 || activity.side != next[index - 1].projection.surface.descriptor.activity->side ||
                activity.group != next[index - 1].projection.surface.descriptor.activity->group)
                ordinal = 0;
            next[index].ordinal = ordinal++;
        }
        moveLabels_ = {context_.localization.Get("editor", "workspace.activity.move_left"),
                       context_.localization.Get("editor", "workspace.activity.move_right"),
                       context_.localization.Get("editor", "workspace.activity.move_bottom")};
        moveEarlierLabel_ = context_.localization.Get("editor", "workspace.activity.move_earlier");
        moveLaterLabel_ = context_.localization.Get("editor", "workspace.activity.move_later");
        entries_ = std::move(next);
        revision_ = revision;
        locale_ = locale;
    }

    bool ExtensionActivityView::QueueMove(const Extensions::EditorSurfaceProviderIdentity &provider, const std::string_view id,
                                          const Extensions::EditorActivityPlacement placement) noexcept {
        if (!host_ || !host_->IsLive(provider))
            return false;
        const auto entry = std::ranges::find_if(entries_, [&provider, id](const auto &candidate) {
            return candidate.projection.surface.descriptor.provider == provider && candidate.projection.surface.descriptor.id == id;
        });
        if (entry == entries_.end())
            return false;
        pendingMove_ = PendingMove{{reinterpret_cast<std::uintptr_t>(this), entry->token, revision_}, placement};
        return true;
    }

    std::int32_t ExtensionActivityView::GroupSize(const Extensions::EditorActivitySide side, const std::uint8_t group) const noexcept {
        return static_cast<std::int32_t>(std::ranges::count_if(entries_, [side, group](const auto &entry) {
            const auto &activity = *entry.projection.surface.descriptor.activity;
            return activity.side == side && activity.group == group;
        }));
    }

    void ExtensionActivityView::AcceptMove(const Extensions::EditorActivityPlacement placement) {
        if (!ImGui::BeginDragDropTarget())
            return;
        if (const auto *payload = ImGui::AcceptDragDropPayload("HORO_EXTENSION_ACTIVITY");
            payload && payload->DataSize == sizeof(DragPayload)) {
            DragPayload source;
            std::memcpy(&source, payload->Data, sizeof(source));
            if (source.owner == reinterpret_cast<std::uintptr_t>(this) && source.revision == revision_)
                pendingMove_ = PendingMove{source, placement};
        }
        ImGui::EndDragDropTarget();
    }

    std::optional<Extensions::EditorActivitySide> ExtensionActivityView::TakeNativePanelClear() noexcept {
        for (std::size_t side = 0; side < nativePanelClear_.size(); ++side) {
            if (const auto token = std::exchange(nativePanelClear_[side], std::nullopt); token.has_value()) {
                const auto entry = std::ranges::find(entries_, *token, &Entry::token);
                if (entry != entries_.end() && host_ && host_->IsLive(entry->projection.surface.descriptor.provider))
                    return static_cast<Extensions::EditorActivitySide>(side);
            }
        }
        return std::nullopt;
    }

    std::size_t ExtensionActivityView::Count(const bool right, const std::size_t group) const noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(entries_, [this, right, group](const auto &entry) {
            const auto &surface = entry.projection.surface;
            return host_ && host_->IsLive(surface.descriptor.provider) &&
                   surface.providerStatus == Extensions::EditorSurfaceProviderStatus::Active && surface.activity.visible &&
                   surface.descriptor.activity->group == group &&
                   (surface.descriptor.activity->side == Extensions::EditorActivitySide::Right) == right;
        }));
    }

    /** @copydoc ExtensionActivityView::ActivateEntry */
    bool ExtensionActivityView::ActivateEntry(const Entry &entry) {
        const auto &surface = entry.projection.surface;
        if (const auto toggled = host_->Registry().ToggleActivity(surface.descriptor.provider, surface.descriptor.id); toggled.HasError())
            return false;
        nativePanelClear_[static_cast<std::size_t>(surface.descriptor.activity->side)] = entry.token;
        return true;
    }

    bool ExtensionActivityView::DrawItems(const bool right, const std::size_t group, ImVec2 origin, const float width, const float bottom) {
        bool changed{};
        for (const auto &entry : entries_) {
            const auto &surface = entry.projection.surface;
            if (!host_ || !host_->IsLive(surface.descriptor.provider) ||
                surface.providerStatus != Extensions::EditorSurfaceProviderStatus::Active || !surface.activity.visible ||
                surface.descriptor.activity->group != group ||
                (surface.descriptor.activity->side == Extensions::EditorActivitySide::Right) != right)
                continue;
            if (origin.y + 30.0F > bottom)
                break;
            ImGui::SetCursorScreenPos(origin);
            ImGui::PushID(surface.descriptor.id.c_str());
            if (Ui::ActivityButton({.tooltip = entry.tooltip.c_str(),
                                    .texture = entry.texture,
                                    .size = {width, 30.0F},
                                    .active = surface.open,
                                    .enabled = surface.activity.enabled,
                                    .indicatorOnRight = right,
                                    .badgeCount = surface.activity.badgeCount},
                                   context_.theme.fonts)) {
                changed = ActivateEntry(entry) || changed;
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
                const DragPayload source{reinterpret_cast<std::uintptr_t>(this), entry.token, revision_};
                ImGui::SetDragDropPayload("HORO_EXTENSION_ACTIVITY", &source, sizeof(source));
                Ui::Hint(entry.label.c_str(), context_.theme.fonts);
                ImGui::EndDragDropSource();
            }
            AcceptMove({surface.descriptor.activity->side, static_cast<std::uint8_t>(group), entry.ordinal});
            DrawPlacementMenu(entry, group);
            ImGui::PopID();
            origin.y += 32.0F;
        }
        if (ImGui::GetDragDropPayload() && origin.y + 4.0F <= bottom) {
            ImGui::SetCursorScreenPos(origin);
            ImGui::PushID(static_cast<int>(group));
            ImGui::InvisibleButton("##ExtensionInsertion", {width, (std::min)(30.0F, bottom - origin.y)});
            const auto side = right ? Extensions::EditorActivitySide::Right : Extensions::EditorActivitySide::Left;
            AcceptMove({side, static_cast<std::uint8_t>(group), GroupSize(side, static_cast<std::uint8_t>(group))});
            ImGui::PopID();
        }
        DrawVisibilityMenu(right);
        return changed;
    }

    bool ExtensionActivityView::HasDrawer(const Extensions::EditorActivitySide side) const noexcept {
        return std::ranges::any_of(entries_, [this, side](const auto &entry) {
            return host_ && host_->IsLive(entry.projection.surface.descriptor.provider) && entry.projection.surface.open &&
                   entry.projection.surface.descriptor.activity->side == side;
        });
    }

    void ExtensionActivityView::Close(const Extensions::EditorActivitySide side) {
        if (!host_)
            return;
        for (const auto &entry : entries_)
            if (entry.projection.surface.descriptor.activity->side == side)
                static_cast<void>(host_->Registry().Close(entry.projection.surface.descriptor.id));
    }

    /** @copydoc ExtensionActivityView::DrawForm */
    void ExtensionActivityView::DrawForm(const Entry &entry) {
        const auto &surface = entry.projection.surface;
        if (!surface.form)
            return;
        for (std::size_t index = 0; index < surface.form->nodes.size(); ++index) {
            const auto &node = surface.form->nodes[index];
            const auto &base = Extensions::EditorUiNodeBaseOf(node);
            ImGui::PushID(base.id.value.c_str());
            if (const auto *action = std::get_if<Extensions::EditorUiActionNode>(&node.payload)) {
                if (Ui::Button({.label = entry.nodeText[index].c_str(),
                                .enabled = base.enabled,
                                .font = context_.theme.fonts.sans,
                                .style = {.width = Ui::StyleWidth::FillAvailable}}))
                    static_cast<void>(host_->QueueAction(surface.descriptor.provider, surface.descriptor.id, base.id.value,
                                                         action->action.value, entry.projection.revision));
            } else if (std::holds_alternative<Extensions::EditorUiContainerNode>(node.payload)) {
                if (!entry.nodeText[index].empty())
                    Ui::SectionTitle(entry.nodeText[index].c_str(), context_.theme.fonts);
            } else
                Ui::Hint(entry.nodeText[index].c_str(), context_.theme.fonts);
            ImGui::PopID();
        }
    }

    void ExtensionActivityView::DrawDrawer(const Extensions::EditorActivitySide side, const ImVec2 position, const ImVec2 size) {
        if (size.x <= 0 || size.y <= 0)
            return;
        for (auto &entry : entries_) {
            const auto &surface = entry.projection.surface;
            if (!host_ || !host_->IsLive(surface.descriptor.provider) || !surface.open || surface.descriptor.activity->side != side) {
                entry.focusPending = false;
                continue;
            }
            ImGui::SetNextWindowPos(position);
            ImGui::SetNextWindowSize(size);
            if (entry.focusPending) {
                ImGui::SetNextWindowFocus();
                entry.focusPending = false;
            }
            ImGui::PushID(surface.descriptor.id.c_str());
            const char *window = "##ExtensionBottomDrawer";
            if (side == Extensions::EditorActivitySide::Left)
                window = "##ExtensionLeftDrawer";
            else if (side == Extensions::EditorActivitySide::Right)
                window = "##ExtensionRightDrawer";
            ImGui::Begin(window, nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoSavedSettings);
            Ui::SectionTitle(entry.label.c_str(), context_.theme.fonts);
            ImGui::SameLine((std::max)(0.0F, ImGui::GetWindowContentRegionMax().x - Ui::ScaledLayoutValue(28.0F)));
            if (Ui::IconCloseButton("##CloseDrawer", {24, 24}))
                static_cast<void>(host_->Registry().Close(surface.descriptor.id));
            DrawForm(entry);
            ImGui::End();
            ImGui::PopID();
        }
    }
}  // namespace Horo::Editor
