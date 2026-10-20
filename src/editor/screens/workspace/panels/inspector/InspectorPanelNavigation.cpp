#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
    namespace {
        /** @brief Resolve localized labels without turning them into authored identity. */
        const char *Label(const EditorGuiContext &ctx, const char *key) {
            return ctx.localization.Get("editor", key).c_str();
        }

        /** @brief Preserve narrow-layout shared typography and show the complete selected value on focus/hover. */
        bool Select(const EditorGuiContext &ctx, const char *key, int &selected, const std::vector<std::string> &labels) {
            std::vector<const char *> entries;
            entries.reserve(labels.size());
            for (const auto &label : labels)
                entries.push_back(label.c_str());
            const bool changed = Ui::DrawComboPropRow(Label(ctx, key), key, selected, entries, ctx.theme.fonts);
            if (selected >= 0 && static_cast<std::size_t>(selected) < labels.size() && (ImGui::IsItemHovered() || ImGui::IsItemFocused()))
                Ui::ShowTooltip(labels[static_cast<std::size_t>(selected)].c_str(), &ctx.theme.fonts);
            return changed;
        }

        /** @brief Present an unresolved stable identity as an explicit sentinel; never select the first available descriptor. */
        template <class Id>
        void Identity(const EditorGuiContext &ctx, const char *key, Id &value, const std::vector<std::pair<Id, std::string>> &options) {
            std::vector<std::string> labels{std::string(Label(ctx, "workspace.navigation_inspector.missing")) + " " +
                                            std::to_string(value.Value())};
            int selected = 0;
            for (const auto &[id, name] : options) {
                labels.push_back(name);
                if (id == value)
                    selected = static_cast<int>(labels.size() - 1);
            }
            if (Select(ctx, key, selected, labels) && selected > 0)
                value = options[static_cast<std::size_t>(selected - 1)].first;
        }

        /** @brief Closed enum choices use localized shared rows and retain invalid source values until explicitly repaired. */
        template <class E> void Enum(const EditorGuiContext &ctx, const char *key, E &value, std::initializer_list<const char *> keys) {
            std::vector<std::string> labels;
            for (const auto *entry : keys)
                labels.emplace_back(Label(ctx, entry));
            int selected = static_cast<int>(value);
            if (Select(ctx, key, selected, labels))
                value = static_cast<E>(selected);
        }

        /** @brief Shared float editor leaves nonfinite or negative drafts outside committed state. */
        void Scalar(const EditorGuiContext &ctx, const char *key, float &value) {
            static_cast<void>(Ui::DrawFloatPropRow(Label(ctx, key), key, value, ctx.theme.fonts));
        }

        /** @brief Shared axis editor retains object-local coordinates without a world-space conversion. */
        void Vector(const EditorGuiContext &ctx, const char *key, Math::Vec3 &value) {
            std::array components{value.x, value.y, value.z};
            if (Ui::DrawFloat3PropRow(Label(ctx, key), key, components, ctx.theme.fonts).changed)
                value = {components[0], components[1], components[2]};
        }

        /** @brief Shared toggles do not activate runtime state while editing. */
        void Toggle(const EditorGuiContext &ctx, const char *key, bool &value) {
            static_cast<void>(Ui::CheckboxControl(Label(ctx, key), &value, ctx.theme.fonts));
        }

        /** @brief Bounds use positive half extents; validity is displayed independently of transient interaction. */
        void Bounds(const EditorGuiContext &ctx, Runtime::NavigationLocalBounds &bounds) {
            Vector(ctx, "workspace.navigation_inspector.center", bounds.center);
            Vector(ctx, "workspace.navigation_inspector.extents", bounds.halfExtents);
        }

        /** @brief Exact surface reference selector has no default or inferred hierarchy target. */
        void Surface(const EditorGuiContext &ctx, const char *key, Navigation::SurfaceId &id, const NavigationInspectorSession &session) {
            std::vector<std::pair<Navigation::SurfaceId, std::string>> options;
            for (const auto &surface : session.Surfaces())
                options.emplace_back(surface.id, std::to_string(surface.id.Value()));
            Identity(ctx, key, id, options);
        }

        /** @brief Resolve descriptor ownership from an exact authored surface rather than selecting a global registry. */
        const Navigation::NavigationDefinition *Definition(Navigation::SurfaceId surface, const NavigationInspectorSession &session,
                                                           const EditorWorkspaceViewModel &view) {
            const auto found = std::ranges::find(session.Surfaces(), surface, &Runtime::NavigationSurfaceComponent::id);
            if (found == session.Surfaces().end())
                return nullptr;
            for (const auto &entry : view.navigationDefinitions)
                if (entry.asset == found->definition)
                    return entry.definition.get();
            return nullptr;
        }

        /** @brief Edit an explicit profile set, retaining missing members for repair and never inventing a replacement. */
        void Profiles(const EditorGuiContext &ctx, std::vector<Navigation::NavigationAgentProfileId> &selected,
                      const Navigation::NavigationDefinition *definition, const Navigation::NavigationDefinition *other = nullptr) {
            Ui::FieldLabel(Label(ctx, "workspace.navigation_inspector.profiles"), ctx.theme.fonts);
            if (!definition) {
                Ui::Hint(Label(ctx, "workspace.navigation_inspector.degraded"), ctx.theme.fonts);
                return;
            }
            for (const auto &profile : definition->Profiles()) {
                if (other && std::ranges::none_of(other->Profiles(), [&](const auto &entry) {
                    return entry.id == profile.id;
                }))
                    continue;
                const auto found = std::ranges::find(selected, profile.id);
                bool checked = found != selected.end();
                ImGui::PushID(static_cast<int>(profile.id.Value()));
                if (Ui::CheckboxControl(profile.displayName.c_str(), &checked, ctx.theme.fonts)) {
                    if (checked)
                        selected.push_back(profile.id);
                    else
                        selected.erase(found);
                }
                if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
                    Ui::ShowTooltip(profile.displayName.c_str(), &ctx.theme.fonts);
                ImGui::PopID();
            }
            for (std::size_t index = 0; index < selected.size(); ++index) {
                if (std::ranges::any_of(definition->Profiles(), [&](const auto &p) {
                    return p.id == selected[index];
                }))
                    continue;
                ImGui::PushID(static_cast<int>(index));
                Ui::Hint((std::string(Label(ctx, "workspace.navigation_inspector.missing")) + " " + std::to_string(selected[index].Value()))
                             .c_str(),
                         ctx.theme.fonts);
                if (Ui::Button({.label = Label(ctx, "workspace.navigation_inspector.remove_missing"), .font = ctx.theme.fonts.sans})) {
                    selected.erase(selected.begin() + static_cast<std::ptrdiff_t>(index));
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
        }

        /** @brief Definition assets are selected by stable AssetId; absent descriptors remain visible and cannot activate. */
        void Draw(const EditorGuiContext &ctx, Runtime::NavigationSurfaceComponent &value, const NavigationInspectorSession &,
                  const EditorWorkspaceViewModel &view) {
            std::vector<std::string> labels{Label(ctx, "workspace.navigation_inspector.missing")};
            int selected{};
            for (const auto &entry : view.navigationDefinitions) {
                labels.push_back(entry.asset.ToString());
                if (entry.asset == value.definition)
                    selected = static_cast<int>(labels.size() - 1);
            }
            if (Select(ctx, "workspace.navigation_inspector.definition", selected, labels) && selected > 0)
                value.definition = view.navigationDefinitions[static_cast<std::size_t>(selected - 1)].asset;
            const Navigation::NavigationDefinition *definition{};
            for (const auto &entry : view.navigationDefinitions)
                if (entry.asset == value.definition)
                    definition = entry.definition.get();
            Profiles(ctx, value.profiles, definition);
            auto scope = value.bakeScope;
            Enum(ctx, "workspace.navigation_inspector.scope", scope,
                 {"workspace.navigation_inspector.subtree", "workspace.navigation_inspector.bounds"});
            if (scope != value.bakeScope) {
                value.bakeScope = scope;
                if (scope == Runtime::NavigationBakeScope::LocalBounds)
                    value.localBounds.emplace();
                else
                    value.localBounds.reset();
            }
            if (value.localBounds)
                Bounds(ctx, *value.localBounds);
        }

        /** @brief Region bounds and source-selection policy retain their stable surface binding. */
        void Draw(const EditorGuiContext &ctx, Runtime::NavigationRegionComponent &value, const NavigationInspectorSession &session,
                  const EditorWorkspaceViewModel &) {
            Surface(ctx, "workspace.navigation_inspector.surface", value.surface, session);
            Bounds(ctx, value.localBounds);
            Enum(ctx, "workspace.navigation_inspector.region_mode", value.mode,
                 {"workspace.navigation_inspector.include", "workspace.navigation_inspector.exclude"});
            Enum(ctx, "workspace.navigation_inspector.sources", value.sourceSelection,
                 {"workspace.navigation_inspector.explicit", "workspace.navigation_inspector.static"});
        }

        /** @brief Modifier operation transitions update their closed optional-field invariant together in the draft. */
        void Draw(const EditorGuiContext &ctx, Runtime::NavigationModifierComponent &value, const NavigationInspectorSession &session,
                  const EditorWorkspaceViewModel &view) {
            Surface(ctx, "workspace.navigation_inspector.surface", value.surface, session);
            int shape = static_cast<int>(value.volume.index());
            if (Select(ctx, "workspace.navigation_inspector.shape", shape,
                       {Label(ctx, "workspace.navigation_inspector.box"), Label(ctx, "workspace.navigation_inspector.cylinder")})) {
                if (shape == 0)
                    value.volume = Runtime::NavigationLocalBounds{};
                else
                    value.volume = Runtime::NavigationCylinderVolume{};
            }
            if (auto *box = std::get_if<Runtime::NavigationLocalBounds>(&value.volume))
                Bounds(ctx, *box);
            else {
                auto &cylinder = std::get<Runtime::NavigationCylinderVolume>(value.volume);
                Vector(ctx, "workspace.navigation_inspector.center", cylinder.center);
                Scalar(ctx, "workspace.navigation_inspector.radius", cylinder.radius);
                Scalar(ctx, "workspace.navigation_inspector.half_height", cylinder.halfHeight);
            }
            auto operation = value.operation;
            Enum(ctx, "workspace.navigation_inspector.operation", operation,
                 {"workspace.navigation_inspector.exclude", "workspace.navigation_inspector.area",
                  "workspace.navigation_inspector.area_cost"});
            if (operation != value.operation) {
                value.operation = operation;
                if (operation == Runtime::NavigationModifierOperation::Exclude)
                    value.area.reset();
                else if (!value.area)
                    value.area.emplace();
                if (operation == Runtime::NavigationModifierOperation::OverrideAreaAndCost) {
                    if (!value.traversalCost)
                        value.traversalCost = 1.0F;
                } else
                    value.traversalCost.reset();
            }
            if (value.area) {
                std::vector<std::pair<Navigation::NavigationAreaId, std::string>> options;
                if (const auto *definition = Definition(value.surface, session, view))
                    for (const auto &area : definition->Registry().Areas())
                        options.emplace_back(area.id, std::to_string(area.id.Value()));
                Identity(ctx, "workspace.navigation_inspector.area", *value.area, options);
            }
            if (value.traversalCost)
                Scalar(ctx, "workspace.navigation_inspector.cost", *value.traversalCost);
        }

        /** @brief Ordered endpoint controls preserve direction semantics and object-local positions. */
        void Endpoint(const EditorGuiContext &ctx, Runtime::NavigationLinkEndpoint &value, const NavigationInspectorSession &session) {
            Surface(ctx, "workspace.navigation_inspector.surface", value.surface, session);
            Vector(ctx, "workspace.navigation_inspector.position", value.localPosition);
            Scalar(ctx, "workspace.navigation_inspector.connection_radius", value.connectionRadiusMeters);
        }

        /** @brief Links retain ordered endpoints and an explicit compatible grounded profile set. */
        void Draw(const EditorGuiContext &ctx, Runtime::NavigationLinkComponent &value, const NavigationInspectorSession &session,
                  const EditorWorkspaceViewModel &view) {
            Ui::FieldLabel(Label(ctx, "workspace.navigation_inspector.start"), ctx.theme.fonts);
            ImGui::PushID("start");
            Endpoint(ctx, value.start, session);
            ImGui::PopID();
            Ui::FieldLabel(Label(ctx, "workspace.navigation_inspector.end"), ctx.theme.fonts);
            ImGui::PushID("end");
            Endpoint(ctx, value.end, session);
            ImGui::PopID();
            Enum(ctx, "workspace.navigation_inspector.kind", value.kind,
                 {"workspace.navigation_inspector.jump", "workspace.navigation_inspector.ladder", "workspace.navigation_inspector.door",
                  "workspace.navigation_inspector.teleport"});
            Enum(ctx, "workspace.navigation_inspector.direction", value.direction,
                 {"workspace.navigation_inspector.forward", "workspace.navigation_inspector.both"});
            Profiles(ctx, value.profiles, Definition(value.start.surface, session, view), Definition(value.end.surface, session, view));
            Scalar(ctx, "workspace.navigation_inspector.cost", value.traversalCost);
        }

        /** @brief Agent profile/filter selectors preserve identities and leave unresolved descriptors explicit. */
        void Draw(const EditorGuiContext &ctx, Runtime::NavigationAgentComponent &value, const NavigationInspectorSession &,
                  const EditorWorkspaceViewModel &view) {
            std::vector<std::pair<Navigation::NavigationAgentProfileId, std::string>> profiles;
            std::vector<std::pair<Navigation::NavigationFilterId, std::string>> filters;
            for (const auto &entry : view.navigationDefinitions)
                if (entry.definition) {
                    for (const auto &profile : entry.definition->Profiles())
                        profiles.emplace_back(profile.id, profile.displayName);
                    for (const auto &filter : entry.definition->Registry().Filters())
                        filters.emplace_back(filter.id, std::to_string(filter.id.Value()));
                }
            Identity(ctx, "workspace.navigation_inspector.profile", value.profile, profiles);
            Identity(ctx, "workspace.navigation_inspector.filter", value.filter, filters);
            bool overrideRadius = value.radiusOverride.has_value();
            const bool prior = overrideRadius;
            Toggle(ctx, "workspace.navigation_inspector.override_radius", overrideRadius);
            if (prior != overrideRadius) {
                if (overrideRadius)
                    value.radiusOverride = 0.5F;
                else
                    value.radiusOverride.reset();
            }
            if (value.radiusOverride)
                Scalar(ctx, "workspace.navigation_inspector.radius", *value.radiusOverride);
        }

        /** @brief Validate typed component drafts independently of any runtime activation. */
        bool Valid(const NavigationInspectorValue &value) {
            return std::visit([](const auto &component) {
                using T = std::decay_t<decltype(component)>;
                if constexpr (std::is_same_v<T, Runtime::NavigationSurfaceComponent>)
                    return Runtime::ValidateNavigationSurfaceComponent(component).HasValue();
                else if constexpr (std::is_same_v<T, Runtime::NavigationRegionComponent>)
                    return Runtime::ValidateNavigationRegionComponent(component).HasValue();
                else if constexpr (std::is_same_v<T, Runtime::NavigationModifierComponent>)
                    return Runtime::ValidateNavigationModifierComponent(component).HasValue();
                else if constexpr (std::is_same_v<T, Runtime::NavigationLinkComponent>)
                    return Runtime::ValidateNavigationLinkComponent(component).HasValue();
                else
                    return Runtime::ValidateNavigationAgentComponent(component).HasValue();
            }, value);
        }
    }  // namespace

    /** @copydoc InspectorPanel::DrawNavigation */
    void InspectorPanel::DrawNavigation(const EditorWorkspaceViewModel &view, EditorWorkspaceViewCommandData &command,
                                        const EditorGuiContext &context) {
        m_navigationSession.Begin(view);
        if (m_navigationSession.Drafts().empty())
            return;
        bool canEdit = view.physicsContext.canEdit && !view.sceneExternalConflict && view.playState == EditorPlayState::Idle &&
                       (!view.prefabSelection || !view.prefabHierarchyUnavailable);
        for (const auto &object : view.objects)
            if (std::ranges::find(view.selectedObjects, object.id) != view.selectedObjects.end() && object.effectivelyLocked)
                canEdit = false;
        if (view.navigationDefinitions.empty())
            Ui::Hint(Label(context, "workspace.navigation_inspector.degraded"), context.theme.fonts);
        constexpr std::array titles{"workspace.navigation_inspector.surface", "workspace.navigation_inspector.region",
                                    "workspace.navigation_inspector.modifier", "workspace.navigation_inspector.link",
                                    "workspace.navigation_inspector.agent"};
        bool valid = true;
        ImGui::BeginDisabled(!canEdit);
        std::size_t index{};
        for (auto &draft : m_navigationSession.Drafts()) {
            ImGui::PushID(static_cast<int>(index++));
            Ui::Card card({.id = "navigation"});
            card.DrawTitleBar({.id = "title", .title = Label(context, titles[draft.edit.value.index()]), .fonts = context.theme.fonts});
            if (card.BeginBody()) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, draft.label, Theme::Text(), context.theme.fonts);
                std::visit([&](auto &component) {
                    Toggle(context, "workspace.navigation_inspector.enabled", component.enabled);
                    Draw(context, component, m_navigationSession, view);
                }, draft.edit.value);
                if (!Valid(draft.edit.value))
                    Ui::ErrorText(Label(context, "workspace.navigation_inspector.invalid"), context.theme.fonts);
            }
            valid = Valid(draft.edit.value) && valid;
            card.Finish();
            ImGui::PopID();
        }
        const bool dirty = m_navigationSession.Dirty();
        const bool apply = Ui::Button(
            {.label = Label(context, "workspace.navigation_inspector.apply"),
             .enabled = dirty && valid && !view.navigationDefinitions.empty() && command.command == EditorWorkspaceViewCommand::None,
             .font = context.theme.fonts.sans,
             .style = {.width = Ui::StyleWidth::FillAvailable}});
        const bool cancel = Ui::Button({.label = Label(context, "workspace.navigation_inspector.cancel"),
                                        .enabled = dirty,
                                        .font = context.theme.fonts.sans,
                                        .style = {.width = Ui::StyleWidth::FillAvailable}}) ||
                            (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
        if (cancel)
            m_navigationSession.Cancel();
        else if (apply) {
            command.command = EditorWorkspaceViewCommand::UpdateNavigationComponents;
            command.navigationPayload = m_navigationSession.Request();
        }
        ImGui::EndDisabled();
        if (!view.navigationEditError.empty()) {
            Ui::ErrorText(Label(context, "workspace.navigation_inspector.rejected"), context.theme.fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, view.navigationEditError, Theme::Muted(), context.theme.fonts);
        }
    }
}  // namespace Horo::Editor
