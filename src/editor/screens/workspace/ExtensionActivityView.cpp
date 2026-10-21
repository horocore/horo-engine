#include "ExtensionActivityView.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/design_system/components/EditorUiCustomResources.h"
#include "editor/renderer/EditorGuiRenderer.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Horo::Editor {
    /** @copydoc ExtensionActivityView::OwnedTexture::OwnedTexture(IEditorGuiRenderer &, std::uintptr_t) */
    ExtensionActivityView::OwnedTexture::OwnedTexture(IEditorGuiRenderer &renderer, const std::uintptr_t id) noexcept
        : renderer_(&renderer), id_(id) {}

    /** @copydoc ExtensionActivityView::OwnedTexture::~OwnedTexture */
    ExtensionActivityView::OwnedTexture::~OwnedTexture() {
        Reset();
    }

    /** @copydoc ExtensionActivityView::OwnedTexture::OwnedTexture(OwnedTexture &&) */
    ExtensionActivityView::OwnedTexture::OwnedTexture(OwnedTexture &&other) noexcept
        : renderer_(std::exchange(other.renderer_, nullptr)), id_(std::exchange(other.id_, 0)) {}

    /** @copydoc ExtensionActivityView::OwnedTexture::operator= */
    ExtensionActivityView::OwnedTexture &ExtensionActivityView::OwnedTexture::operator=(OwnedTexture &&other) noexcept {
        if (this != &other) {
            Reset();
            renderer_ = std::exchange(other.renderer_, nullptr);
            id_ = std::exchange(other.id_, 0);
        }
        return *this;
    }

    /** @copydoc ExtensionActivityView::OwnedTexture::Id */
    std::uintptr_t ExtensionActivityView::OwnedTexture::Id() const noexcept {
        return id_;
    }

    /** @copydoc ExtensionActivityView::OwnedTexture::Reset */
    void ExtensionActivityView::OwnedTexture::Reset() noexcept {
        if (renderer_ && id_)
            renderer_->DestroyTexture(id_);
        renderer_ = nullptr;
        id_ = 0;
    }

    ExtensionActivityView::ExtensionActivityView(const EditorGuiContext &context, Extensions::EditorActivityHost *host,
                                                 IEditorGuiRenderer *renderer)
        : context_(context), host_(host), renderer_(renderer) {}

    ExtensionActivityView::~ExtensionActivityView() = default;

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

    /** @copydoc ExtensionActivityView::PreparePlots */
    void ExtensionActivityView::PreparePlots(Entry &entry, const Entry *previous) const {
        const auto &form = entry.projection.surface.form;
        if (!form)
            return;
        for (std::size_t i = 0; i < form->nodes.size(); ++i) {
            if (!std::holds_alternative<Extensions::EditorUiPlotNode>(form->nodes[i].payload))
                continue;
            Extensions::EditorUiPlotState state;
            if (previous && previous->projection.surface.form) {
                const auto &oldNodes = previous->projection.surface.form->nodes;
                const auto &id = Extensions::EditorUiNodeBaseOf(form->nodes[i]).id;
                const auto old = std::ranges::find_if(previous->plots, [&](const auto &candidate) {
                    return Extensions::EditorUiNodeBaseOf(oldNodes[candidate.first]).id == id;
                });
                if (old != previous->plots.end())
                    state = old->second;
            }
            entry.plots.emplace_back(i, state);
        }
    }

    /** @copydoc ExtensionActivityView::PrepareEntry */
    ExtensionActivityView::Entry ExtensionActivityView::PrepareEntry(const Extensions::EditorActivityProjection &projection,
                                                                     const std::string &locale) {
        const auto previous = std::ranges::find_if(entries_, [&projection](const auto &entry) {
            return entry.projection.surface.descriptor.id == projection.surface.descriptor.id &&
                   entry.projection.surface.descriptor.provider == projection.surface.descriptor.provider;
        });
        Entry entry{.projection = projection};
        if (previous != entries_.end())
            entry.offeredWidth = previous->offeredWidth;
        PreparePlots(entry, previous != entries_.end() ? std::to_address(previous) : nullptr);
        const auto &descriptor = projection.surface.descriptor;
        entry.label = host_->LocalizedText(descriptor.provider, descriptor.labelLocalizationKey, locale);
        entry.tooltip = host_->LocalizedText(descriptor.provider, descriptor.tooltipLocalizationKey, locale);
        PrepareNodeText(entry, locale);
        PrepareResources(entry, previous != entries_.end() ? std::to_address(previous) : nullptr);
        PrepareLayout(entry);
        return entry;
    }

    /** @copydoc ExtensionActivityView::PrepareResources */
    void ExtensionActivityView::PrepareResources(Entry &entry, const Entry *previous) {
        entry.token = previous != nullptr ? previous->token : nextToken_++;
        if (previous != nullptr) {
            entry.focusPending =
                previous->focusPending ||
                (entry.projection.surface.focused &&
                 (!previous->projection.surface.focused || !previous->projection.surface.open ||
                  previous->projection.surface.descriptor.activity->side != entry.projection.surface.descriptor.activity->side));
        } else {
            entry.focusPending = entry.projection.surface.focused;
        }
        // Existing textures stay with the published projection until every candidate is prepared.
        if ((previous == nullptr || previous->texture.Id() == 0) && renderer_ && entry.projection.icon) {
            const auto &icon = *entry.projection.icon;
            if (auto uploaded = renderer_->CreateTexture({icon.width, icon.height, icon.pixels}); uploaded.HasValue())
                entry.texture = OwnedTexture{*renderer_, uploaded.Value()};
        }
        if (entry.projection.surface.open &&
            (previous == nullptr || !previous->projection.surface.open ||
             previous->projection.surface.descriptor.activity->side != entry.projection.surface.descriptor.activity->side))
            entry.requestNativePanelClear = true;
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
                if (std::holds_alternative<Extensions::EditorUiActionNode>(node.payload))
                    text += "###action";
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
                const Ui::ScopedEditorUiIdentity identity{surface.descriptor.provider, surface.descriptor.id, entry.token};
                if (Ui::CheckboxControl(entry.label.c_str(), &visible, context_.theme.fonts))
                    static_cast<void>(host_->Registry().SetActivityVisibility(surface.descriptor.id, visible));
            }
            ImGui::EndPopup();
        }
    }

    /** @copydoc ExtensionActivityView::SetInputAdmitted */
    void ExtensionActivityView::SetInputAdmitted(const bool admitted) noexcept {
        inputAdmitted_ = admitted;
        for (auto &entry : entries_)
            if (entry.render)
                Ui::ApplyEditorUiInput(*entry.render, admitted && entry.projection.surface.activity.enabled);
        if (host_)
            host_->SetInputAdmitted(admitted);
    }

    void ExtensionActivityView::Update() {
        if (!host_)
            return;
        if (ImGui::GetCurrentContext())
            static_cast<void>(themeCapture_.Update(context_));
        ApplyPendingMove();
        const auto revision = host_->Registry().Revision();
        const auto &locale = context_.settings.settings.languageTag;
        if (revision == revision_ && locale == locale_) {
            for (auto &entry : entries_)
                PrepareLayout(entry);
            return;
        }
        std::vector<Entry> next;
        next.reserve(host_->Prepared().size());
        for (const auto &projection : host_->Prepared()) {
            next.push_back(PrepareEntry(projection, locale));
        }
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
        std::array<std::string, 3> moveLabels{context_.localization.Get("editor", "workspace.activity.move_left"),
                                              context_.localization.Get("editor", "workspace.activity.move_right"),
                                              context_.localization.Get("editor", "workspace.activity.move_bottom")};
        auto moveEarlierLabel = context_.localization.Get("editor", "workspace.activity.move_earlier");
        auto moveLaterLabel = context_.localization.Get("editor", "workspace.activity.move_later");
        std::string nextLocale{locale};
        CommitEntries(std::move(next));
        moveLabels_ = std::move(moveLabels);
        moveEarlierLabel_ = std::move(moveEarlierLabel);
        moveLaterLabel_ = std::move(moveLaterLabel);
        revision_ = revision;
        locale_ = std::move(nextLocale);
    }

    /** @copydoc ExtensionActivityView::CommitEntries */
    void ExtensionActivityView::CommitEntries(std::vector<Entry> next) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<Entry> && std::is_nothrow_move_assignable_v<Entry>);
        for (auto &entry : next) {
            if (const auto previous = std::ranges::find_if(entries_,
                                                           [&entry](const Entry &old) {
                return old.projection.surface.descriptor.id == entry.projection.surface.descriptor.id &&
                       old.projection.surface.descriptor.provider == entry.projection.surface.descriptor.provider;
            });
                previous != entries_.end() && !entry.texture.Id())
                entry.texture = std::move(previous->texture);
            if (entry.requestNativePanelClear)
                nativePanelClear_[static_cast<std::size_t>(entry.projection.surface.descriptor.activity->side)] = entry.token;
        }
        entries_ = std::move(next);
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
        const Ui::ScopedEditorUiInput inputScope{inputAdmitted_};
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
            const Ui::ScopedEditorUiIdentity identity{surface.descriptor.provider, surface.descriptor.id, entry.token};
            if (Ui::ActivityButton({.tooltip = entry.tooltip.c_str(),
                                    .texture = entry.texture.Id(),
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

    /** @copydoc ExtensionActivityView::PrepareLayout */
    void ExtensionActivityView::PrepareLayout(Entry &entry) const {
        const auto theme = themeCapture_.Frame();
        if (!theme || !entry.projection.surface.form)
            return;
        if (entry.render && entry.render->themeRevision == theme->revision && entry.preparedWidth == entry.offeredWidth)
            return;
        auto render = Extensions::BuildEditorUiRenderSnapshot(*entry.projection.surface.form, *theme, entry.offeredWidth / theme->uiScale);
        if (render.HasValue()) {
            entry.render.emplace(std::move(render).Value());
            entry.preparedWidth = entry.offeredWidth;
            Ui::ApplyEditorUiInput(*entry.render, inputAdmitted_ && entry.projection.surface.activity.enabled);
        } else
            entry.render.reset();
    }

    /** @copydoc ExtensionActivityView::Accessibility */
    std::span<const Extensions::EditorUiAccessibleNode> ExtensionActivityView::Accessibility(std::string_view surface) const noexcept {
        for (const auto &entry : entries_)
            if (entry.render && entry.projection.surface.descriptor.id == surface && host_ &&
                host_->IsLive(entry.projection.surface.descriptor.provider))
                return entry.render->accessibility;
        return {};
    }

    /** @copydoc ExtensionActivityView::DrawFormNode */
    void ExtensionActivityView::DrawFormNode(Entry &entry, std::size_t index, float width, bool enabled) const {
        const auto &surface = entry.projection.surface;
        const auto &node = surface.form->nodes[index];
        const auto &base = Extensions::EditorUiNodeBaseOf(node);
        const auto &geometry = entry.render->nodes[index];
        const auto fonts = Ui::EditorUiFonts(context_.theme.fonts, geometry.font);
        std::visit([&]<class T>(const T &typed) {
            if constexpr (std::is_same_v<T, Extensions::EditorUiPlotNode>) {
                DrawPlot(entry, typed, index, enabled);
            } else if constexpr (std::is_same_v<T, Extensions::EditorUiActionNode>) {
                if (Ui::Button({.label = entry.nodeText[index].c_str(),
                                .size = {width / entry.render->uiScale, geometry.height / entry.render->uiScale},
                                .enabled = enabled,
                                .font = fonts.sans}))
                    static_cast<void>(host_->QueueAction(surface.descriptor.provider, surface.descriptor.id, base.id.value,
                                                         typed.action.value, entry.projection.revision));
            } else if constexpr (std::is_same_v<T, Extensions::EditorUiContainerNode>) {
                if (!entry.nodeText[index].empty())
                    Ui::SectionTitle(entry.nodeText[index].c_str(), fonts);
            } else
                Ui::Hint(entry.nodeText[index].c_str(), fonts);
        }, node.payload);
    }

    /** @copydoc ExtensionActivityView::DrawForm */
    void ExtensionActivityView::DrawForm(Entry &entry) const {
        const auto &surface = entry.projection.surface;
        if (!surface.form || !entry.render)
            return;
        entry.offeredWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0F);
        const auto origin = ImGui::GetCursorScreenPos();
        float bottom{};
        for (std::size_t index = 0; index < surface.form->nodes.size(); ++index) {
            const auto &node = surface.form->nodes[index];
            const auto &base = Extensions::EditorUiNodeBaseOf(node);
            const auto &geometry = entry.render->nodes[index];
            const float width = std::max(1.0F, std::min(geometry.width, entry.offeredWidth - geometry.x));
            ImGui::SetCursorScreenPos({origin.x + geometry.x, origin.y + geometry.y});
            ImGui::PushClipRect({origin.x + geometry.x, origin.y + geometry.y},
                                {origin.x + geometry.x + width, origin.y + geometry.y + geometry.height}, true);
            ImGui::PushID(base.id.value.c_str());
            const bool enabled = inputAdmitted_ && surface.activity.enabled && geometry.enabled;
            ImGui::BeginDisabled(!enabled);
            DrawFormNode(entry, index, width, enabled);
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::PopClipRect();
            bottom = std::max(bottom, geometry.y + geometry.height);
        }
        ImGui::SetCursorScreenPos({origin.x, origin.y + bottom});
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
            if (entry.focusPending && inputAdmitted_) {
                ImGui::SetNextWindowFocus();
                entry.focusPending = false;
            }
            const char *window = "##ExtensionBottomDrawer";
            if (side == Extensions::EditorActivitySide::Left)
                window = "##ExtensionLeftDrawer";
            else if (side == Extensions::EditorActivitySide::Right)
                window = "##ExtensionRightDrawer";
            ImGui::Begin(window, nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
            {
                const Ui::ScopedEditorUiInput inputScope{inputAdmitted_};
                const Ui::ScopedEditorUiIdentity identity{surface.descriptor.provider, surface.descriptor.id, entry.token};
                Ui::SectionTitle(entry.label.c_str(), context_.theme.fonts);
                ImGui::SameLine((std::max)(0.0F, ImGui::GetWindowContentRegionMax().x - Ui::ScaledLayoutValue(28.0F)));
                if (Ui::IconCloseButton("##CloseDrawer", {24, 24}))
                    static_cast<void>(host_->Registry().Close(surface.descriptor.id));
                DrawForm(entry);
            }
            ImGui::End();
        }
    }
}  // namespace Horo::Editor
