#include "NavigationSettingsDraft.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <imgui.h>

namespace Horo::Editor::SettingsModalInternal {
    namespace {
        namespace Nav = Horo::Navigation;

        /** @brief Resolves a complete localized label. */
        std::string Text(const EditorGuiContext &ctx, std::string_view key) {
            return ctx.localization.Get("editor", key);
        }

        /** @brief Shared boolean control edits only the user preference draft. */
        void Overlay(std::string_view key, bool &value, const EditorGuiContext &ctx) {
            const auto label = Text(ctx, key);
            Ui::SettingRow(label.c_str(), "", ctx.theme.fonts, [&label, &value, &ctx] {
                static_cast<void>(Ui::ToggleControl(label.c_str(), &value, ctx.theme.fonts));
            });
        }

        /** @brief Draws all seven independently unit-labelled project ceilings. */
        bool Capacities(Nav::NavigationCapacityLimits &limits, const EditorGuiContext &ctx) {
            bool changed{};
            std::array<std::uint64_t, 7> values{limits.maximumAgents,
                                                limits.maximumSurfaces,
                                                limits.maximumResidentTiles,
                                                limits.maximumConcurrentQueries,
                                                limits.maximumBytesPerResidentTile,
                                                limits.maximumResidentMemoryBytes,
                                                limits.maximumWorkUnitsPerTick};
            for (std::size_t index = 0; index < values.size(); ++index)
                changed |= NavigationUnsignedField(std::format("settings.navigation.capacity.{}", index), values[index], ctx,
                                                   index < 4 ? UINT32_MAX : UINT64_MAX);
            if (values[0] <= UINT32_MAX && values[1] <= UINT32_MAX && values[2] <= UINT32_MAX && values[3] <= UINT32_MAX)
                limits = {static_cast<std::uint32_t>(values[0]),
                          static_cast<std::uint32_t>(values[1]),
                          static_cast<std::uint32_t>(values[2]),
                          static_cast<std::uint32_t>(values[3]),
                          values[4],
                          values[5],
                          values[6]};
            return changed;
        }

        /** @brief Presents a closed typed enumeration with localized labels instead of numeric magic values. */
        template <std::size_t Count, class Enum> bool EnumField(const std::string_view key, Enum &value, const EditorGuiContext &ctx) {
            std::array<std::string, Count> labels;
            std::array<const char *, Count> names;
            for (std::size_t index = 0; index < Count; ++index) {
                labels[index] = Text(ctx, std::format("{}.{}", key, index));
                names[index] = labels[index].c_str();
            }
            auto selected = static_cast<int>(value);
            bool changed{};
            const auto title = Text(ctx, key);
            Ui::SettingRow(title.c_str(), "", ctx.theme.fonts, [&changed, &title, &selected, &names, &ctx] {
                changed = Ui::ComboControl(title.c_str(), &selected, names.data(), static_cast<int>(names.size()), ctx.theme.fonts);
            });
            if (changed && selected >= 0 && static_cast<std::size_t>(selected) < Count)
                value = static_cast<Enum>(selected);
            return changed;
        }

        /** @brief Draws required/optional capabilities and the exact query family/quality envelope. */
        bool Query(Nav::NavigationProjectProfileInput &profile, const EditorGuiContext &ctx) {
            bool changed{};
            changed |= EnumField<static_cast<std::size_t>(Nav::NavigationQueryKind::Count)>("settings.navigation.query_kind",
                                                                                            profile.maximumQuery.query, ctx);
            changed |= EnumField<static_cast<std::size_t>(Nav::NavigationQualityLevel::Count)>("settings.navigation.query_quality",
                                                                                               profile.maximumQuery.quality, ctx);
            auto nodes = static_cast<std::uint64_t>(profile.maximumQuery.limits.maximumNodeExpansions);
            auto points = static_cast<std::uint64_t>(profile.maximumQuery.limits.maximumResultPoints);
            changed |= NavigationUnsignedField("settings.navigation.query_nodes", nodes, ctx, UINT32_MAX);
            changed |= NavigationUnsignedField("settings.navigation.query_points", points, ctx, UINT32_MAX);
            profile.maximumQuery.limits.maximumNodeExpansions = static_cast<std::uint32_t>(nodes);
            profile.maximumQuery.limits.maximumResultPoints = static_cast<std::uint32_t>(points);
            const auto before = profile.maximumQuery.limits.maximumSearchDistanceMeters;
            const auto distance = Text(ctx, "settings.navigation.query_distance");
            Ui::SettingRow(distance.c_str(), "", ctx.theme.fonts, [&profile, &ctx] {
                Ui::InputFloatControl("##navigation-distance", &profile.maximumQuery.limits.maximumSearchDistanceMeters, ctx.theme.fonts);
            });
            changed |= before != profile.maximumQuery.limits.maximumSearchDistanceMeters;
            for (std::size_t index = 0; index < profile.capabilities.size(); ++index) {
                bool required = profile.capabilities[index] == Nav::NavigationCapabilityRequirement::Required;
                const auto prior = required;
                Overlay(std::format("settings.navigation.capability.{}", index), required, ctx);
                if (prior != required) {
                    changed = true;
                    profile.capabilities[index] =
                        required ? Nav::NavigationCapabilityRequirement::Required : Nav::NavigationCapabilityRequirement::Optional;
                }
            }
            return changed;
        }

        /** @brief Reports exact host metadata without constructing, discovering or selecting a concrete provider. */
        void Compatibility(const SettingsState &state, const EditorGuiContext &ctx) {
            const auto title = Text(ctx, "settings.navigation.compatibility");
            Ui::SectionTitle(title.c_str(), ctx.theme.fonts);
            const auto evidence = state.navigationSettings.Resolve()->TargetEvidence();
            const auto policy = state.navigationSettings.Resolve()->Policy();
            if (evidence.empty() || !policy) {
                ImGui::TextWrapped("%s", Text(ctx, "settings.navigation.compatibility_unknown").c_str());
                return;
            }
            for (const auto &target : evidence) {
                const auto result = Application::CheckNavigationProjectCompatibility(*policy, target);
                const auto status = Text(ctx, result.HasValue() ? "settings.navigation.compatible" : "settings.navigation.incompatible");
                ImGui::TextWrapped("%s / %s: %s", target.target.c_str(), target.provider.c_str(), status.c_str());
                if (result.HasError())
                    ImGui::TextWrapped("%s", result.ErrorValue().message.c_str());
            }
        }

        /** @brief Creates a policy only after an explicit user gesture, without selecting a live provider. */
        void EnsurePolicyDraft(SettingsState &state, NavigationSettingsDraft &draft, const EditorGuiContext &ctx,
                               const NavigationDefinitionSnapshot &definition) {
            if (!draft.policy) {
                if (const auto policy = state.navigationSettings.Resolve()->Policy())
                    draft.policy = policy->Input();
                else if (Ui::Button({.label = Text(ctx, "settings.navigation.create_policy").c_str()})) {
                    Nav::NavigationProjectProfileInput profile{Nav::NavigationProjectProfileId::Create(1).Value(),
                                                               Nav::NavigationProjectProfileRevision::Create(1).Value(),
                                                               {64, 8, 128, 8, 1024 * 1024, 128 * 1024 * 1024, 8192},
                                                               {Nav::NavigationQueryKind::Path,
                                                                Nav::NavigationQualityLevel::Balanced,
                                                                {1024, 128, 1000}}};
                    draft.policy = Application::NavigationProjectSettingsInput{1, definition.asset, "recast_detour", profile};
                    state.navigationProjectDraftDirty = true;
                }
            }
        }

        /** @brief Publishes the exact validated successor separately from local user preferences. */
        void SavePolicyDraft(SettingsState &state, Application::NavigationProjectSettingsInput &policy, const EditorGuiContext &ctx) {
            if (Ui::Button({.label = Text(ctx, "settings.navigation.save_policy").c_str(), .enabled = state.navigationProjectDraftDirty})) {
                const auto previous = state.navigationSettings.Resolve()->Policy();
                if (!previous ||
                    (previous->Input().revision != UINT64_MAX && previous->Input().runtimeProfile.revision.Value() != UINT64_MAX)) {
                    policy.revision = previous ? previous->Input().revision + 1 : 1;
                    policy.runtimeProfile.revision =
                        Nav::NavigationProjectProfileRevision::Create(previous ? previous->Input().runtimeProfile.revision.Value() + 1 : 1)
                            .Value();
                    auto result = state.navigationSettings.Resolve()->SavePolicy(policy);
                    if (result.HasError())
                        state.navigationStatus = result.ErrorValue().message;
                    else {
                        state.navigationProjectDraftDirty = result.Value().durabilityError.has_value();
                        state.navigationStatus = result.Value().durabilityError ? result.Value().durabilityError->message
                                                                                : Text(ctx, "settings.navigation.saved");
                    }
                }
            }
        }

        /** @brief Reuses asset labels and pointer storage for the complete captured opening; rendering does not rebuild the catalog. */
        void DefinitionChoices(NavigationSettingsDraft &draft, const std::span<const NavigationDefinitionSnapshot> definitions,
                               const EditorGuiContext &ctx) {
            if (draft.definitionOpening != definitions.front().session || draft.definitionNames.size() != definitions.size()) {
                draft.definitionLabels.clear();
                draft.definitionNames.clear();
                draft.definitionLabels.reserve(definitions.size());
                draft.definitionNames.reserve(definitions.size());
                for (const auto &definition : definitions)
                    draft.definitionLabels.push_back(definition.asset.ToString());
                for (const auto &label : draft.definitionLabels)
                    draft.definitionNames.push_back(label.c_str());
                draft.definitionOpening = definitions.front().session;
            }
            draft.selectedDefinition = std::clamp(draft.selectedDefinition, 0, static_cast<int>(draft.definitionNames.size()) - 1);
            static_cast<void>(Ui::ComboControl("##navigation-definition-binding", &draft.selectedDefinition, draft.definitionNames.data(),
                                               static_cast<int>(draft.definitionNames.size()), ctx.theme.fonts));
        }
    }  // namespace

    /** @copydoc NavigationUnsignedField */
    bool NavigationUnsignedField(const std::string_view key, std::uint64_t &value, const EditorGuiContext &ctx,
                                 const std::uint64_t maximum) {
        auto text = std::to_string(value);
        const auto label = Text(ctx, key);
        bool changed{};
        Ui::SettingRow(label.c_str(), "", ctx.theme.fonts, [&changed, &label, &text, &ctx] {
            changed = Ui::InputTextControl(label.c_str(), text, 20, ctx.theme.fonts);
        });
        if (!changed)
            return false;
        std::uint64_t candidate{};
        if (const auto parsed = std::from_chars(text.data(), text.data() + text.size(), candidate);
            text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || candidate > maximum) {
            ImGui::TextWrapped("%s", Text(ctx, "settings.navigation.invalid_number").c_str());
            return false;
        }
        value = candidate;
        return true;
    }

    /** @copydoc DrawNavigationPolicy */
    void DrawNavigationPolicy(SettingsState &state, NavigationSettingsDraft &draft, const EditorGuiContext &ctx,
                              const NavigationDefinitionSnapshot &definition) {
        const auto title = Text(ctx, "settings.navigation.project_policy");
        Ui::SectionTitle(title.c_str(), ctx.theme.fonts);
        EnsurePolicyDraft(state, draft, ctx, definition);
        if (draft.policy) {
            auto &policy = *draft.policy;
            const auto provider = Text(ctx, "settings.navigation.default_provider");
            Ui::SettingRow(provider.c_str(), "", ctx.theme.fonts, [&state, &policy, &ctx] {
                state.navigationProjectDraftDirty |=
                    Ui::InputTextControl("##navigation-provider", policy.defaultProvider, 128, ctx.theme.fonts);
            });
            if (Ui::Button({.label = Text(ctx, "settings.navigation.bind_definition").c_str()})) {
                policy.definition = definition.asset;
                state.navigationProjectDraftDirty = true;
            }
            ImGui::TextUnformatted(policy.definition.ToString().c_str());
            state.navigationProjectDraftDirty |= Capacities(policy.runtimeProfile.capacities, ctx);
            state.navigationProjectDraftDirty |= Query(policy.runtimeProfile, ctx);
            SavePolicyDraft(state, policy, ctx);
        }
        Compatibility(state, ctx);
        const auto local = Text(ctx, "settings.navigation.local_overlay");
        Ui::SectionTitle(local.c_str(), ctx.theme.fonts);
        Overlay("settings.navigation.show_areas", state.navigationOverlay.showAreas, ctx);
        Overlay("settings.navigation.show_paths", state.navigationOverlay.showPaths, ctx);
        Overlay("settings.navigation.show_agents", state.navigationOverlay.showAgents, ctx);
    }

    /** @copydoc DrawNavigationSettings */
    void DrawNavigationSettings(SettingsState &state, const EditorGuiContext &ctx) {
        if (!state.navigationSettings.Resolve()) {
            ImGui::TextWrapped("%s", ctx.localization.Get("editor", "settings.navigation.unavailable").c_str());
            if (!state.navigationStatus.empty())
                ImGui::TextWrapped("%s", state.navigationStatus.c_str());
            return;
        }
        if (!state.navigationDraft)
            state.navigationDraft = std::make_shared<NavigationSettingsDraft>();
        auto &draft = *state.navigationDraft;
        const auto definitions = state.navigationSettings.Resolve()->Definitions();
        if (definitions.HasError()) {
            state.navigationStatus = definitions.ErrorValue().message;
            return;
        }
        if (definitions.Value().empty()) {
            ImGui::TextWrapped("%s", ctx.localization.Get("editor", "settings.navigation.no_definition").c_str());
            return;
        }
        DefinitionChoices(draft, definitions.Value(), ctx);
        const auto &definition = definitions.Value()[static_cast<std::size_t>(draft.selectedDefinition)];
        if (Ui::Button({.label = ctx.localization.Get("editor", "settings.navigation.open_definition").c_str()}))
            state.navigationSettings.Resolve()->RequestOpenDefinition(definition.asset);
        DrawNavigationPolicy(state, draft, ctx, definition);
        if (!state.navigationStatus.empty())
            ImGui::TextWrapped("%s", state.navigationStatus.c_str());
    }
}  // namespace Horo::Editor::SettingsModalInternal
