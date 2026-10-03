#pragma once

#include "Horo/Editor/DesignSystem/DesignTokens.h"

#include <imgui.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Horo::Editor::Theme {

    // ─────────────────────────────────────────────────────────────────────────
    // Palette — exact match for the CSS custom properties in the HTML mockups:
    //   --bg0 #0a0c0f   --bg1 #12151a   --bg2 #181c21   --bg3 #1f242b
    //   --hover #232830 --bd #2a2f37    --bd2 #3a4049
    //   --txt #e8e4d9   --mut #9a958a   --dim #5e5b54
    //   --a #04A5FC     --ok #5fb88a    --warn #e8a33d  --err #d4524a
    //
    // These read from the active DesignTokens so theme switching
    // affects all custom-drawn editor UI, not just ImGui native widgets.
    // ─────────────────────────────────────────────────────────────────────────

    /** @brief Returns the active (theme-aware) design token set. */
    [[nodiscard]] const DesignSystem::DesignTokens &GetActiveTokens();

    [[nodiscard]] inline ImVec4 Bg0() {
        return GetActiveTokens().colors.surfaceRoot;
    }

    [[nodiscard]] inline ImVec4 Bg1() {
        return GetActiveTokens().colors.surfaceWindow;
    }

    [[nodiscard]] inline ImVec4 Bg2() {
        return GetActiveTokens().colors.surfacePanel;
    }

    [[nodiscard]] inline ImVec4 Bg3() {
        return GetActiveTokens().colors.surfaceRaised;
    }

    [[nodiscard]] inline ImVec4 Hover() {
        return GetActiveTokens().colors.surfaceHover;
    }

    [[nodiscard]] inline ImVec4 Border() {
        return GetActiveTokens().colors.border;
    }

    [[nodiscard]] inline ImVec4 BorderStrong() {
        return GetActiveTokens().colors.borderStrong;
    }

    [[nodiscard]] inline ImVec4 Text() {
        return GetActiveTokens().colors.textPrimary;
    }

    [[nodiscard]] inline ImVec4 Muted() {
        return GetActiveTokens().colors.textMuted;
    }

    [[nodiscard]] inline ImVec4 Dim() {
        return GetActiveTokens().colors.textDim;
    }

    [[nodiscard]] inline ImVec4 Accent() {
        return GetActiveTokens().colors.actionPrimary;
    }

    [[nodiscard]] inline ImVec4 AccentHover() {
        return GetActiveTokens().colors.actionPrimaryHover;
    }

    [[nodiscard]] inline ImVec4 AccentActive() {
        return GetActiveTokens().colors.actionPrimaryActive;
    }

    [[nodiscard]] inline ImVec4 AccentSoft() {
        return GetActiveTokens().colors.actionPrimarySoft;
    }

    [[nodiscard]] inline ImVec4 Ok() {
        return GetActiveTokens().colors.statusOk;
    }

    [[nodiscard]] inline ImVec4 Warn() {
        return GetActiveTokens().colors.statusWarn;
    }

    [[nodiscard]] inline ImVec4 Err() {
        return GetActiveTokens().colors.statusError;
    }

    [[nodiscard]] inline ImVec4 ErrSoft() {
        ImVec4 c = GetActiveTokens().colors.statusError;
        c.w = 0.12F;
        return c;
    }

    [[nodiscard]] inline ImVec4 DarkText() {
        return GetActiveTokens().colors.textOnActionPrimary;
    }

    [[nodiscard]] inline ImVec4 Shadow() {
        return {0.0F, 0.0F, 0.0F, 0.55F};
    }

    /** @brief Blends two theme colors without escaping the active palette. */
    [[nodiscard]] inline ImVec4 Mix(const ImVec4 &from, const ImVec4 &to, const float amount) {
        return {
            from.x + (to.x - from.x) * amount,
            from.y + (to.y - from.y) * amount,
            from.z + (to.z - from.z) * amount,
            from.w + (to.w - from.w) * amount,
        };
    }

    /** @brief Returns the subtly accent-cooled generic panel-card surface. */
    [[nodiscard]] inline ImVec4 CardSurface() {
        return Mix(Bg1(), Accent(), 0.025F);
    }

    /** @brief Returns the raised generic card-header surface. */
    [[nodiscard]] inline ImVec4 CardHeaderSurface() {
        return Mix(Bg2(), Accent(), 0.03F);
    }

    /** @brief Returns the cool border treatment shared by panel cards and fields. */
    [[nodiscard]] inline ImVec4 CardBorder() {
        return Mix(Border(), Accent(), 0.02F);
    }

    /** @brief Compatibility alias for Inspector call sites migrating to generic cards. */
    [[nodiscard]] inline ImVec4 InspectorCardSurface() {
        return CardSurface();
    }

    /** @brief Compatibility alias for Inspector call sites migrating to generic cards. */
    [[nodiscard]] inline ImVec4 InspectorHeaderSurface() {
        return CardHeaderSurface();
    }

    /** @brief Returns the Inspector field surface between window and raised tiers. */
    [[nodiscard]] inline ImVec4 InspectorFieldSurface() {
        return Mix(Mix(Bg1(), Bg3(), 0.55F), Accent(), 0.025F);
    }

    /** @brief Returns the slightly deeper object-name field surface used above Inspector cards. */
    [[nodiscard]] inline ImVec4 InspectorTitleFieldSurface() {
        return Mix(Mix(Bg1(), Bg3(), 0.4F), Accent(), 0.02F);
    }

    /** @brief Returns the cool side-dock border treatment used by Inspector cards and fields. */
    [[nodiscard]] inline ImVec4 InspectorBorder() {
        return CardBorder();
    }

    /** @brief Returns the compact editor-menu surface used by root and nested popups. */
    [[nodiscard]] inline ImVec4 MenuSurface() {
        return Mix(Bg1(), Accent(), 0.015F);
    }

    /** @brief Returns the stronger cool border used around editor-menu popups. */
    [[nodiscard]] inline ImVec4 MenuBorder() {
        return Mix(BorderStrong(), Accent(), 0.03F);
    }

    /** @brief Returns the elevated, opaque surface shared by editor tooltips. */
    [[nodiscard]] inline ImVec4 TooltipSurface() {
        return Mix(Bg3(), Bg0(), 0.20F);
    }

    /** @brief Returns the subtly accent-tinted border shared by editor tooltips. */
    [[nodiscard]] inline ImVec4 TooltipBorder() {
        return Mix(BorderStrong(), Accent(), 0.12F);
    }

    /** @brief Shared elevated toolbar surface used by every bottom-dock tab. */
    [[nodiscard]] inline ImVec4 BottomDockToolbarSurface() {
        return Mix(Bg1(), Bg2(), 0.35F);
    }

    /** @brief Shared content surface directly below a bottom-dock toolbar. */
    [[nodiscard]] inline ImVec4 BottomDockContentSurface() {
        return Bg1();
    }

    /** @brief Shared field and action surface inside bottom-dock toolbars. */
    [[nodiscard]] inline ImVec4 BottomDockControlSurface() {
        return Mix(Bg2(), Bg3(), 0.20F);
    }

    /** @brief Shared elevated card surface used by bottom-dock metric summaries. */
    [[nodiscard]] inline ImVec4 BottomDockMetricSurface() {
        return Mix(Bg1(), Bg2(), 0.45F);
    }

    [[nodiscard]] inline ImVec4 ConsoleFooterSurface() {
        return Mix(Bg1(), Bg0(), 0.28F);
    }

    [[nodiscard]] inline ImVec4 ConsoleRowBorder() {
        return Mix(Border(), Bg0(), 0.48F);
    }

    [[nodiscard]] inline ImVec4 ConsoleInfo() {
        return Mix(Accent(), Text(), 0.10F);
    }

    [[nodiscard]] inline ImVec4 ConsoleWarning() {
        return Mix(Warn(), Text(), 0.08F);
    }

    [[nodiscard]] inline ImVec4 ConsoleError() {
        return Mix(Err(), Text(), 0.08F);
    }

    [[nodiscard]] inline ImVec4 ConsoleErrorSurface() {
        ImVec4 color = Mix(Bg0(), Err(), 0.17F);
        color.w = 1.0F;
        return color;
    }

    [[nodiscard]] inline ImVec4 ConsoleErrorBorder() {
        ImVec4 color = Mix(Border(), Err(), 0.45F);
        color.w = 0.65F;
        return color;
    }

    [[nodiscard]] inline ImVec4 ConsoleSourceText() {
        return Mix(Text(), Muted(), 0.28F);
    }

    [[nodiscard]] inline ImVec4 ConsoleMessageText() {
        return Mix(Text(), Muted(), 0.12F);
    }

    [[nodiscard]] inline ImU32 U32(const ImVec4 &c) {
        return ImGui::GetColorU32(c);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Fonts — the three font atlas entries loaded by the application
    // ─────────────────────────────────────────────────────────────────────────
    struct Fonts {
        ImFont *sans = nullptr;
        ImFont *sansCompact = nullptr;
        ImFont *sansEmphasis = nullptr;
        ImFont *icon = nullptr; /**< Material Symbols Outlined for editor UI icons. */
    };

    namespace FontPx {
        constexpr float Sans = DesignSystem::DefaultDesignTokens().typography.sansBase;
        constexpr float SansCompact = DesignSystem::DefaultDesignTokens().typography.sansCompactBase;
        constexpr float SansEmphasis = DesignSystem::DefaultDesignTokens().typography.sansEmphasisBase;
        constexpr float Icon = 20.0f; /**< Pixel size for Material Symbols icon font. */
    }  // namespace FontPx

    /** @brief Theme-resolved semantic visible-text sizes. */
    namespace TextPx {
        /** @brief Returns the supporting metadata and secondary-text size. @return Theme-resolved logical pixels. */
        [[nodiscard]] inline float Caption() {
            return DesignSystem::TypographyFor(GetActiveTokens(), DesignSystem::TypographyRole::Caption);
        }

        /** @brief Returns the controls, tabs, tree rows, badges, and field-label size. @return Theme-resolved logical pixels. */
        [[nodiscard]] inline float Label() {
            return DesignSystem::TypographyFor(GetActiveTokens(), DesignSystem::TypographyRole::Label);
        }

        /** @brief Returns the standard paragraph and primary-content size. @return Theme-resolved logical pixels. */
        [[nodiscard]] inline float Body() {
            return DesignSystem::TypographyFor(GetActiveTokens(), DesignSystem::TypographyRole::Body);
        }

        /** @brief Returns the compact card and component-section title size. @return Theme-resolved logical pixels. */
        [[nodiscard]] inline float CardTitle() {
            return DesignSystem::TypographyFor(GetActiveTokens(), DesignSystem::TypographyRole::CardTitle);
        }

        /** @brief Returns the panel and modal-title size. @return Theme-resolved logical pixels. */
        [[nodiscard]] inline float Title() {
            return DesignSystem::TypographyFor(GetActiveTokens(), DesignSystem::TypographyRole::Title);
        }

        /** @brief Returns the section-heading size. @return Theme-resolved logical pixels. */
        [[nodiscard]] inline float Heading() {
            return DesignSystem::TypographyFor(GetActiveTokens(), DesignSystem::TypographyRole::Heading);
        }

        /** @brief Returns the top-level screen-heading size. @return Theme-resolved logical pixels. */
        [[nodiscard]] inline float Display() {
            return DesignSystem::TypographyFor(GetActiveTokens(), DesignSystem::TypographyRole::Display);
        }
    }  // namespace TextPx

    [[nodiscard]] constexpr float Scale(float targetPx, float basePx) {
        return targetPx / basePx;
    }

    inline void PushFont(ImFont *f) {
        if (f)
            ImGui::PushFont(f);
    }

    inline void PopFont(const ImFont *f) {
        if (f)
            ImGui::PopFont();
    }

    struct ScopedFont {
        ImFont *font;

        explicit ScopedFont(ImFont *f) : font(f) {
            PushFont(font);
        }

        ~ScopedFont() {
            PopFont(font);
        }

        ScopedFont(const ScopedFont &) = delete;
        ScopedFont &operator=(const ScopedFont &) = delete;
    };

    struct ScopedFontScale {
        explicit ScopedFontScale(float scale) {
            ImGui::SetWindowFontScale(scale);
        }

        ~ScopedFontScale() {
            ImGui::SetWindowFontScale(1.0F);
        }

        ScopedFontScale(const ScopedFontScale &) = delete;
        ScopedFontScale &operator=(const ScopedFontScale &) = delete;
    };

    struct ScopedTextStyle {
        [[no_unique_address]] ScopedFont font;
        [[no_unique_address]] ScopedFontScale scale;

        ScopedTextStyle(ImFont *f, float targetPx, float basePx) : font(f), scale(Scale(targetPx, basePx)) {}
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Layout metrics
    // ─────────────────────────────────────────────────────────────────────────
    namespace Layout {
        constexpr float Radius = DesignSystem::DefaultDesignTokens().radii.control;
        constexpr float RadiusCard = DesignSystem::DefaultDesignTokens().radii.card;
        constexpr float RadiusModal = DesignSystem::DefaultDesignTokens().radii.modal;
        constexpr float WelcomeOuterPad = 40.0F;
        constexpr float WelcomeCardW = 900.0F;
        constexpr float WelcomeSideW = DesignSystem::DefaultDesignTokens().sizes.welcomeSideWidth;
        constexpr float WelcomePad = DesignSystem::DefaultDesignTokens().sizes.welcomePadding;
        constexpr float ModalW = DesignSystem::DefaultDesignTokens().sizes.modalWidth;
        constexpr float ModalH = DesignSystem::DefaultDesignTokens().sizes.modalHeight;
        constexpr float HeaderH = DesignSystem::DefaultDesignTokens().sizes.modalHeaderHeight;
        constexpr float FooterH = DesignSystem::DefaultDesignTokens().sizes.modalFooterHeight;
        constexpr float SidebarW = DesignSystem::DefaultDesignTokens().sizes.modalSidebarWidth;
        constexpr float SidebarPadX = DesignSystem::DefaultDesignTokens().spacing.sidebarPaddingX;
        constexpr float SidebarPadY = DesignSystem::DefaultDesignTokens().spacing.sidebarPaddingY;
        constexpr float BodyPadX = DesignSystem::DefaultDesignTokens().spacing.bodyPaddingX;
        constexpr float BodyPadY = DesignSystem::DefaultDesignTokens().spacing.bodyPaddingY;
        constexpr float CardPad = DesignSystem::DefaultDesignTokens().spacing.cardPadding;
        constexpr float GridGap = DesignSystem::DefaultDesignTokens().spacing.gridGap;
        constexpr float SettingsW = DesignSystem::DefaultDesignTokens().sizes.settingsWidth;
        constexpr float SettingsH = DesignSystem::DefaultDesignTokens().sizes.settingsHeight;
        constexpr float ControlW = 260.0F;
    }  // namespace Layout

    // ─────────────────────────────────────────────────────────────────────────
    // Theme preset & runtime switching
    // ─────────────────────────────────────────────────────────────────────────
    enum class Preset {
        HoroDark = 0,
        Midnight = 1,
        Light = 2,
        // Custom themes start at index 3+
    };

    struct ThemeStringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }

        [[nodiscard]] std::size_t operator()(const std::string &value) const noexcept {
            return (*this)(std::string_view{value});
        }

        [[nodiscard]] std::size_t operator()(const char *value) const noexcept {
            return (*this)(std::string_view{value});
        }
    };

    /**
     * @brief A loaded theme entry — either built-in or from a JSON file.
     */
    struct ThemeEntry {
        std::string name;       /**< Display name, for example `Monokai`. */
        std::string sourcePath; /**< Empty for built-ins; source JSON path for custom themes. */
        std::unordered_map<std::string, ImVec4, ThemeStringHash, std::equal_to<>> colors; /**< ImGui color name to RGBA. */
        DesignSystem::DesignTokens designTokens = DesignSystem::DefaultDesignTokens();    /**< Unscaled theme token baseline. */
        bool isBuiltIn = true;                                                            /**< Whether the entry ships with Horo. */
    };

    /** @brief Returns the list of all discovered themes (built-in + custom). */
    [[nodiscard]] const std::vector<ThemeEntry> &GetThemeList();

    /** @brief Scans `~/.horo/themes/` for JSON theme files. Call once at startup. */
    void RefreshThemeList(const char *additionalPath = nullptr);

    /** @brief Loads a single theme JSON from the given path. Returns true on success. */
    [[nodiscard]] bool LoadThemeFromJson(const char *path, ThemeEntry &outEntry);

    /** @brief Activates a theme by index into GetThemeList(). */
    void SelectThemeByIndex(int index);

    /**
     * @brief Compatibility hook for the temporarily disabled global UI scale setting.
     * @param percent Persisted scale value; currently ignored while UI scaling is disabled.
     */
    void SetUiScalePercent(int percent);

    /** @brief Returns the currently active theme index. */
    [[nodiscard]] int GetActiveThemeIndex();

    void Apply(ImGuiStyle &style);
    void SetThemePreset(Preset preset);
    [[nodiscard]] Preset GetThemePreset();
    void ApplyCurrentTheme();

}  // namespace Horo::Editor::Theme
