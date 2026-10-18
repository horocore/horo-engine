#pragma once
#include "Horo/Editor/NavigationProjectSettingsSession.h"
#include "SettingsModalInternal.h"

namespace Horo::Editor {
    /** @brief Transient controls; all committed authoring remains in the project session. */
    struct NavigationSettingsDraft final {
        int selectedDefinition{};
        std::uint64_t definitionOpening{};
        std::vector<std::string> definitionLabels;
        std::vector<const char *> definitionNames;
        int catalog{};
        std::string identity{"1"};
        std::string replacement;
        std::string name;
        Navigation::NavigationAgentBuildGeometry geometry;
        float traversalCost{1};
        std::string sourceIdentity;
        std::string flags{"0"};
        std::string excludedFlags{"0"};
        std::string overrideArea;
        float overrideCost{1};
        std::vector<Navigation::NavigationAreaCostOverride> overrides;
        std::optional<Application::NavigationProjectSettingsInput> policy;
        std::shared_ptr<NavigationSettingsReview> review;
    };
}  // namespace Horo::Editor

namespace Horo::Editor {
    /** @brief Persistent widget state; the session retains all document authority. */
    struct NavigationDefinitionEditorState final {
        NavigationProjectSettingsSessionBorrow session;
        std::shared_ptr<NavigationSettingsDraft> draft;
        std::string status;
        bool requestFocus{};
        bool requestClose{};
    };

    /** @brief Draws a persistent asset document, with an exact fenced review before destructive publication. */
    void DrawNavigationDefinitionEditor(NavigationDefinitionEditorState &state, const EditorGuiContext &context, Assets::AssetId asset);
}  // namespace Horo::Editor

namespace Horo::Editor::SettingsModalInternal {
    /** @brief Draws policy-only project controls and separate user-overlay preferences. */
    void DrawNavigationPolicy(SettingsState &state, NavigationSettingsDraft &draft, const EditorGuiContext &context,
                              const NavigationDefinitionSnapshot &definition);
    /** @brief Draws a bounded labelled integer control without narrowing a project limit. */
    bool NavigationUnsignedField(std::string_view key, std::uint64_t &value, const EditorGuiContext &context,
                                 std::uint64_t maximum = UINT64_MAX);
}  // namespace Horo::Editor::SettingsModalInternal
