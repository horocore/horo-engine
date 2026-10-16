#include "EditorPageView.h"

#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <imgui.h>

namespace Horo::Editor {
    using namespace Extensions;

    /** @copydoc EditorPageView::EditorPageView */
    EditorPageView::EditorPageView(const EditorGuiContext &context, std::shared_ptr<EditorPageHost> host,
                                   std::shared_ptr<const EditorThemeFrame> theme)
        : context_(context), host_(std::move(host)), theme_(std::move(theme)) {}

    /** @copydoc EditorPageView::Select */
    void EditorPageView::Select(EditorSurfaceProviderIdentity provider, std::string id) {
        provider_ = std::move(provider);
        id_ = std::move(id);
        bufferRevision_ = 0;
        render_.reset();
        renderRevision_ = 0;
        failure_.reset();
    }

    /** @copydoc EditorPageView::Current */
    const EditorPageProjection *EditorPageView::Current() const noexcept {
        if (!host_)
            return nullptr;
        const auto found = std::ranges::find_if(host_->Prepared(), [&](const auto &page) {
            return page.surface.provider == provider_ && page.surface.id == id_;
        });
        return found == host_->Prepared().end() ? nullptr : std::to_address(found);
    }

    /** @copydoc EditorPageView::Text */
    const char *EditorPageView::Text(std::string_view key) const {
        return host_->LocalizedText(provider_, key, context_.settings.settings.languageTag).data();
    }

    /** @copydoc EditorPageView::Accept */
    void EditorPageView::Accept(Result<void> result) {
        if (result.HasError())
            failure_ = std::move(result).ErrorValue();
        else
            failure_.reset();
    }

    /** @copydoc EditorPageView::Cancel */
    void EditorPageView::Cancel() noexcept {
        if (!host_ || id_.empty())
            return;
        try {
            Accept(host_->Cancel(provider_, id_));
        } catch (...) {
        }
    }

    /** @copydoc EditorPageView::Synchronize */
    void EditorPageView::Synchronize(const EditorPageProjection &page) {
        if (bufferRevision_ == page.revision)
            return;
        for (std::size_t i = 0; i < page.form.nodes.size(); ++i)
            std::visit([&](const auto &node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, EditorUiNumberNode>) {
                    buffers_[i].reserve(20);
                    buffers_[i] = std::to_string(std::get<std::int64_t>(node.value));
                } else if constexpr (std::is_same_v<T, EditorUiTextFieldNode>) {
                    buffers_[i].reserve(node.maximumBytes);
                    buffers_[i] = node.value;
                } else
                    buffers_[i].clear();
            }, page.form.nodes[i].payload);
        bufferRevision_ = page.revision;
    }

    /** @copydoc EditorPageView::PrepareRender */
    bool EditorPageView::PrepareRender(const EditorPageProjection &page) {
        if (!theme_ || theme_->revision == 0) {
            if (!failure_)
                failure_ = MakeError(ExtensionErrors::EditorThemeTokenInvalid);
            return false;
        }
        const float width = ImGui::GetContentRegionAvail().x;
        if (render_ && renderRevision_ == page.revision && render_->themeRevision == theme_->revision && render_->availableWidth == width)
            return true;
        auto prepared = BuildEditorUiRenderSnapshot(page.form, *theme_, width / theme_->uiScale);
        if (prepared.HasError()) {
            failure_ = prepared.ErrorValue();
            render_.reset();
            return false;
        }
        render_.emplace(std::move(prepared).Value());
        renderRevision_ = page.revision;
        OrderRender(page);
        return true;
    }

    /** @copydoc EditorPageView::OrderRender */
    void EditorPageView::OrderRender(const EditorPageProjection &page) {
        std::array<std::size_t, 256> depths{};
        for (std::size_t i = 0; i < page.form.nodes.size(); ++i) {
            renderOrder_[i] = i;
            auto parent = EditorUiNodeBaseOf(page.form.nodes[i]).parent;
            while (parent.IsValid() && depths[i] < 16) {
                ++depths[i];
                const auto found = std::ranges::find_if(page.form.nodes, [&](const auto &node) {
                    return EditorUiNodeBaseOf(node).id == parent;
                });
                if (found == page.form.nodes.end())
                    break;
                parent = EditorUiNodeBaseOf(*found).parent;
            }
        }
        std::sort(renderOrder_.begin(), renderOrder_.begin() + static_cast<std::ptrdiff_t>(page.form.nodes.size()), [&](auto a, auto b) {
            if (render_->nodes[a].y != render_->nodes[b].y)
                return render_->nodes[a].y < render_->nodes[b].y;
            if (depths[a] != depths[b])
                return depths[a] < depths[b];
            return a < b;
        });
    }

    /** @copydoc EditorPageView::DrawNode */
    void EditorPageView::DrawNode(const EditorUiNode &node, const EditorPageProjection &page, std::size_t index) {
        const auto &base = EditorUiNodeBaseOf(node);
        ImGui::PushID(base.id.value.c_str());
        const bool editable = page.state == EditorPageState::Editing || page.state == EditorPageState::Failed;
        bool admitted = base.enabled && !base.readOnly;
        auto parent = base.parent;
        for (std::size_t depth = 0; parent.IsValid() && depth < 16; ++depth) {
            const auto found = std::ranges::find_if(page.form.nodes, [&](const auto &candidate) {
                return EditorUiNodeBaseOf(candidate).id == parent;
            });
            if (found == page.form.nodes.end()) {
                admitted = false;
                break;
            }
            const auto &ancestor = EditorUiNodeBaseOf(*found);
            admitted = admitted && ancestor.enabled && !ancestor.readOnly;
            parent = ancestor.parent;
        }
        ImGui::BeginDisabled(!admitted || !editable);
        if (page.focused == base.id && ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + render_->nodes[index].width);
        if (!base.label.value.empty())
            Ui::FieldLabel(Text(base.label.value), context_.theme.fonts);
        ImGui::SetNextItemWidth(render_->nodes[index].width);
        DrawValue(node, page, index);
        const bool focused = ImGui::IsItemFocused();
        if (!base.description.value.empty())
            ImGui::TextWrapped("%s", Text(base.description.value));
        ImGui::PopTextWrapPos();
        if (focused && editable && !base.readOnly)
            static_cast<void>(host_->Focus(provider_, id_, page.revision, base.id));
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    /** @copydoc EditorPageView::DrawValue */
    void EditorPageView::DrawValue(const EditorUiNode &node, const EditorPageProjection &page, std::size_t index) {
        const auto &base = EditorUiNodeBaseOf(node);
        std::visit([&](const auto &typed) {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, EditorUiBooleanNode>) {
                auto value = typed.value;
                if (Ui::ToggleControl("value", &value, context_.theme.fonts))
                    Accept(host_->SetValue(provider_, id_, page.revision, {typed.binding, value}));
            } else if constexpr (std::is_same_v<T, EditorUiNumberNode>) {
                if (Ui::InputTextControl("value", buffers_[index], 21, context_.theme.fonts)) {
                    auto value = ParseEditorPageInteger(buffers_[index]);
                    if (value.HasError())
                        failure_ = value.ErrorValue();
                    else
                        Accept(host_->SetValue(provider_, id_, page.revision, {typed.binding, value.Value()}));
                }
            } else if constexpr (std::is_same_v<T, EditorUiTextFieldNode>) {
                if (Ui::InputTextControl("value", buffers_[index], typed.maximumBytes + 1, context_.theme.fonts))
                    Accept(host_->SetValue(provider_, id_, page.revision, {typed.binding, buffers_[index]}));
            } else if constexpr (std::is_same_v<T, EditorUiActionNode>) {
                if (Ui::Button({.label = Text(base.label.value)}))
                    Accept(host_->Apply(provider_, id_, page.revision, typed.action));
            } else if constexpr (std::is_same_v<T, EditorUiTextNode> || std::is_same_v<T, EditorUiLabelNode>) {
                const auto *text = typed.text.kind == EditorUiTextKind::LocalizationKey ? Text(typed.text.value) : typed.text.value.c_str();
                ImGui::TextWrapped("%s", text);
            }
        }, node.payload);
    }

    /** @copydoc EditorPageView::DrawForm */
    void EditorPageView::DrawForm(const EditorPageProjection &page) {
        const auto origin = ImGui::GetCursorScreenPos();
        float extra{}, bottom{};
        for (std::size_t ordinal = 0; ordinal < page.form.nodes.size(); ++ordinal) {
            const auto i = renderOrder_[ordinal];
            const auto &geometry = render_->nodes[i];
            ImGui::SetCursorScreenPos({origin.x + geometry.x, origin.y + geometry.y + extra});
            ImGui::SetNextItemWidth(geometry.width);
            const float before = ImGui::GetCursorScreenPos().y;
            if (std::holds_alternative<EditorUiContainerNode>(page.form.nodes[i].payload)) {
                const auto &base = EditorUiNodeBaseOf(page.form.nodes[i]);
                if (base.label.value.empty())
                    continue;
                Ui::FieldLabel(Text(base.label.value), context_.theme.fonts);
                extra += ImGui::GetCursorScreenPos().y - before;
                bottom = std::max(bottom, geometry.y + extra);
                continue;
            }
            DrawNode(page.form.nodes[i], page, i);
            const float consumed = ImGui::GetCursorScreenPos().y - before;
            extra += std::max(0.0F, consumed - geometry.height);
            bottom = std::max(bottom, geometry.y + geometry.height + extra);
        }
        ImGui::SetCursorScreenPos({origin.x, origin.y + bottom});
    }

    /** @copydoc EditorPageView::DrawPage */
    void EditorPageView::DrawPage() {
        const auto *page = Current();
        if (!page)
            return;
        Synchronize(*page);
        if (!PrepareRender(*page)) {
            if (failure_)
                ImGui::TextWrapped("%s", failure_->message.c_str());
            return;
        }
        DrawForm(*page);
        for (const auto &finding : page->findings)
            ImGui::TextWrapped("%s", Text(finding.message.value));
        if (page->failure)
            ImGui::TextWrapped("%s", page->failure->message.c_str());
        if (failure_)
            ImGui::TextWrapped("%s", failure_->message.c_str());
        const bool editable = page->state == EditorPageState::Editing || page->state == EditorPageState::Failed;
        const auto &apply = context_.localization.Get("editor", "settings.extension_pages.apply");
        const auto &reset = context_.localization.Get("editor", "settings.extension_pages.reset");
        const auto &cancel = context_.localization.Get("editor", "settings.extension_pages.cancel");
        if (Ui::Button({.label = apply.c_str(), .enabled = editable && !failure_}))
            Accept(host_->Apply(provider_, id_, page->revision));
        ImGui::SameLine();
        if (Ui::Button({.label = reset.c_str(), .enabled = editable}))
            Accept(host_->Reset(provider_, id_, page->revision));
        ImGui::SameLine();
        if (Ui::Button({.label = cancel.c_str()}))
            Cancel();
    }

    /** @copydoc EditorPageView::DrawSettings */
    void EditorPageView::DrawSettings() {
        const auto *current = Current();
        const bool maySwitch = !current || (!current->dirty && current->state != EditorPageState::Validating);
        for (const auto &page : host_->Prepared())
            if (page.surface.kind == EditorSurfaceKind::SettingsPage || page.surface.kind == EditorSurfaceKind::Modal) {
                ImGui::PushID(page.surface.id.c_str());
                const auto label =
                    host_->LocalizedText(page.surface.provider, page.surface.labelLocalizationKey, context_.settings.settings.languageTag);
                if (Ui::Button({.label = label.data(), .enabled = maySwitch})) {
                    if (page.surface.kind == EditorSurfaceKind::Modal) {
                        ImGui::PopID();
                        Accept(host_->Begin(page.surface.provider, page.surface.id));
                        return;
                    }
                    if (!current || current->surface.provider != page.surface.provider || current->surface.id != page.surface.id) {
                        if (current)
                            Cancel();
                        Select(page.surface.provider, page.surface.id);
                        Accept(host_->Begin(provider_, id_));
                    }
                }
                ImGui::PopID();
            }
        DrawPage();
    }
}  // namespace Horo::Editor
