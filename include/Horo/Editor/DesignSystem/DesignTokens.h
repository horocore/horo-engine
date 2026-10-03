/**
 * @file DesignTokens.h
 * @brief Typed, theme-resolved design metrics shared by editor UI primitives.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <imgui.h>

namespace Horo::Editor::DesignSystem {

    /** @brief Shared t-shirt size used by every editor primitive. */
    enum class ComponentSize : std::size_t {
        XS,
        Small,
        Medium,
        Large,
        XL,
    };

    /** @brief Theme-backed spacing step used by component style properties. */
    enum class SpacingSize : std::size_t {
        None,
        XS,
        Small,
        Medium,
        Large,
        XL,
    };

    /** @brief Geometry and typography resolved for one component size. */
    struct ComponentSizeMetrics {
        float fontSize;      /**< Text size in logical UI pixels. */
        float paddingX;      /**< Default horizontal frame padding. */
        float paddingY;      /**< Default vertical frame padding. */
        float minimumHeight; /**< Minimum interaction height. */
        float iconSize;      /**< Square icon drawing extent. */
    };

    /** @brief Shared primitive sizing and spacing scale. */
    struct ComponentTokens {
        std::array<ComponentSizeMetrics, 5> sizes;
        std::array<float, 6> spacing;
    };

    /**
     * @brief Semantic color tokens consumed by editor GUI components.
     *
     * These values are the packaged default dark theme for the bootstrap GUI. They
     * intentionally live outside feature screens so screens compose tokens instead
     * of embedding visual literals in screen code.
     */
    struct ColorTokens {
        ImVec4 surfaceRoot;
        ImVec4 surfaceWindow;
        ImVec4 surfacePanel;
        ImVec4 surfaceRaised;
        ImVec4 surfaceHover;
        ImVec4 border;
        ImVec4 borderStrong;
        ImVec4 textPrimary;
        ImVec4 textMuted;
        ImVec4 textDim;
        ImVec4 actionPrimary;
        ImVec4 actionPrimaryHover;
        ImVec4 actionPrimaryActive;
        ImVec4 actionPrimarySoft;
        ImVec4 statusOk;
        ImVec4 statusWarn;
        ImVec4 statusError;
        ImVec4 textOnActionPrimary;
    };

    /** @brief Semantic typography sizes for the bootstrap GUI. */
    struct TypographyTokens {
        float sansBase;         /**< Standard sans-serif atlas size. */
        float sansCompactBase;  /**< Compact sans-serif atlas size. */
        float sansEmphasisBase; /**< Emphasized sans-serif atlas size. */
        float caption;          /**< Supporting metadata and secondary text. */
        float label;            /**< Controls, tabs, tree rows, and field labels. */
        float body;             /**< Standard paragraphs and primary content. */
        float cardTitle;        /**< Compact titles inside cards and component sections. */
        float title;            /**< Panel and modal titles. */
        float heading;          /**< Section headings comparable to an H2. */
        float display;          /**< Top-level screen headings comparable to an H1. */
    };

    /** @brief Semantic visible-text roles shared by every editor surface. */
    enum class TypographyRole : std::size_t {
        Caption,
        Label,
        Body,
        CardTitle,
        Title,
        Heading,
        Display,
    };

    /**
     * @brief Enforces the editor's readable minimum and semantic size ordering.
     * @param typography Typography token set to normalize in place.
     */
    inline void NormalizeTypographyTokens(TypographyTokens &typography) noexcept {
        constexpr float minimumReadableSize = 16.0F;
        typography.sansBase = std::max(typography.sansBase, minimumReadableSize);
        typography.sansCompactBase = std::max(typography.sansCompactBase, minimumReadableSize);
        typography.sansEmphasisBase = std::max(typography.sansEmphasisBase, minimumReadableSize);
        typography.caption = std::max(typography.caption, minimumReadableSize);
        typography.label = std::max(typography.label, minimumReadableSize);
        typography.body = std::max(typography.body, minimumReadableSize);
        typography.cardTitle = std::max(typography.cardTitle, typography.body);
        typography.title = std::max(typography.title, typography.cardTitle);
        typography.heading = std::max(typography.heading, typography.title);
        typography.display = std::max(typography.display, typography.heading);
    }

    /** @brief Shared shape tokens for editor GUI components. */
    struct RadiusTokens {
        float control;
        float card;
        float modal;
    };

    /** @brief Shared layout and control dimensions for editor GUI components. */
    struct SizeTokens {
        float welcomeSideWidth;
        float welcomePadding;
        float modalWidth;
        float modalHeight;
        float modalHeaderHeight;
        float modalFooterHeight;
        float modalSidebarWidth;
        float settingsWidth;
        float settingsHeight;
        float uiScale;
    };

    /** @brief Shared spacing tokens for editor GUI components. */
    struct SpacingTokens {
        float cardPadding;
        float gridGap;
        float bodyPaddingX;
        float bodyPaddingY;
        float sidebarPaddingX;
        float sidebarPaddingY;
        float propertyRowGap; /**< Vertical separation between adjacent Inspector property controls. */
    };

    /** @brief Resolved immutable editor design tokens for one rendered frame. */
    struct DesignTokens {
        ColorTokens colors;
        TypographyTokens typography;
        RadiusTokens radii;
        SizeTokens sizes;
        SpacingTokens spacing;
        ComponentTokens components;
    };

    /**
     * @brief Normalizes typography roles and prevents component text from dropping below caption size.
     * @param tokens Design token set to normalize in place.
     */
    inline void NormalizeDesignTokens(DesignTokens &tokens) noexcept {
        NormalizeTypographyTokens(tokens.typography);
        for (ComponentSizeMetrics &metrics : tokens.components.sizes)
            metrics.fontSize = std::max(metrics.fontSize, tokens.typography.caption);
    }

    /**
     * @brief Returns the resolved metrics for one shared component size.
     * @param tokens Active resolved design tokens.
     * @param size Requested t-shirt size.
     * @return Theme and display-scale resolved component metrics.
     */
    [[nodiscard]] inline const ComponentSizeMetrics &MetricsFor(const DesignTokens &tokens, const ComponentSize size) noexcept {
        return tokens.components.sizes[static_cast<std::size_t>(size)];
    }

    /**
     * @brief Returns one resolved style-spacing value.
     * @param tokens Active resolved design tokens.
     * @param size Requested semantic spacing step.
     * @return Theme and display-scale resolved spacing value.
     */
    [[nodiscard]] inline float SpacingFor(const DesignTokens &tokens, const SpacingSize size) noexcept {
        return tokens.components.spacing[static_cast<std::size_t>(size)];
    }

    /** @brief Applies one global UI scale to resolved component metrics. */
    inline void ApplyGlobalUiScale(DesignTokens &tokens, const float scale) noexcept {
        const float clamped = std::clamp(scale, 1.0F, 2.0F);
        tokens.sizes.uiScale = clamped;

        // Resolve all numeric presentation metrics from one scale so component size,
        // typography, spacing and layout grow together.
        tokens.typography.sansBase *= clamped;
        tokens.typography.sansCompactBase *= clamped;
        tokens.typography.sansEmphasisBase *= clamped;
        tokens.typography.caption *= clamped;
        tokens.typography.label *= clamped;
        tokens.typography.body *= clamped;
        tokens.typography.cardTitle *= clamped;
        tokens.typography.title *= clamped;
        tokens.typography.heading *= clamped;
        tokens.typography.display *= clamped;

        tokens.radii.control *= clamped;
        tokens.radii.card *= clamped;
        tokens.radii.modal *= clamped;

        tokens.sizes.welcomeSideWidth *= clamped;
        tokens.sizes.welcomePadding *= clamped;
        tokens.sizes.modalWidth *= clamped;
        tokens.sizes.modalHeight *= clamped;
        tokens.sizes.modalHeaderHeight *= clamped;
        tokens.sizes.modalFooterHeight *= clamped;
        tokens.sizes.modalSidebarWidth *= clamped;
        tokens.sizes.settingsWidth *= clamped;
        tokens.sizes.settingsHeight *= clamped;
        tokens.spacing.cardPadding *= clamped;
        tokens.spacing.gridGap *= clamped;
        tokens.spacing.bodyPaddingX *= clamped;
        tokens.spacing.bodyPaddingY *= clamped;
        tokens.spacing.sidebarPaddingX *= clamped;
        tokens.spacing.sidebarPaddingY *= clamped;
        tokens.spacing.propertyRowGap *= clamped;

        for (ComponentSizeMetrics &metrics : tokens.components.sizes) {
            metrics.fontSize *= clamped;
            metrics.paddingX *= clamped;
            metrics.paddingY *= clamped;
            metrics.minimumHeight *= clamped;
            metrics.iconSize *= clamped;
        }
        for (float &spacing : tokens.components.spacing)
            spacing *= clamped;
    }

    /**
     * @brief Returns the packaged default editor design tokens.
     * @return Unscaled packaged baseline used before custom theme overrides are applied.
     */
    [[nodiscard]] constexpr DesignTokens DefaultDesignTokens() noexcept {
        return DesignTokens{
            ColorTokens{
                ImVec4{0.039F, 0.047F, 0.059F, 1.0F},
                ImVec4{0.071F, 0.082F, 0.102F, 1.0F},
                ImVec4{0.094F, 0.110F, 0.129F, 1.0F},
                ImVec4{0.122F, 0.141F, 0.169F, 1.0F},
                ImVec4{0.137F, 0.157F, 0.188F, 1.0F},
                ImVec4{0.165F, 0.184F, 0.216F, 1.0F},
                ImVec4{0.227F, 0.251F, 0.286F, 1.0F},
                ImVec4{0.910F, 0.894F, 0.851F, 1.0F},
                ImVec4{0.604F, 0.584F, 0.541F, 1.0F},
                ImVec4{0.369F, 0.357F, 0.329F, 1.0F},
                ImVec4{0.016F, 0.647F, 0.988F, 1.0F},
                ImVec4{0.180F, 0.706F, 0.992F, 1.0F},
                ImVec4{0.000F, 0.500F, 0.820F, 1.0F},
                ImVec4{0.016F, 0.647F, 0.988F, 0.15F},
                ImVec4{0.373F, 0.722F, 0.541F, 1.0F},
                ImVec4{0.910F, 0.639F, 0.239F, 1.0F},
                ImVec4{0.831F, 0.322F, 0.290F, 1.0F},
                ImVec4{0.020F, 0.075F, 0.110F, 1.0F},
            },
            TypographyTokens{
                .sansBase = 18.0F,
                .sansCompactBase = 18.0F,
                .sansEmphasisBase = 18.0F,
                .caption = 18.0F,
                .label = 18.0F,
                .body = 18.0F,
                .cardTitle = 18.0F,
                .title = 20.0F,
                .heading = 24.0F,
                .display = 30.0F,
            },
            RadiusTokens{4.0F, 6.0F, 8.0F},
            SizeTokens{280.0F, 32.0F, 900.0F, 680.0F, 58.0F, 52.0F, 220.0F, 620.0F, 440.0F, 1.0F},
            SpacingTokens{
                .cardPadding = 18.0F,
                .gridGap = 14.0F,
                .bodyPaddingX = 28.0F,
                .bodyPaddingY = 24.0F,
                .sidebarPaddingX = 14.0F,
                .sidebarPaddingY = 18.0F,
                .propertyRowGap = 8.0F,
            },
            ComponentTokens{
                std::array{
                    ComponentSizeMetrics{18.0F, 8.0F, 3.0F, 24.0F, 12.0F},
                    ComponentSizeMetrics{18.0F, 10.0F, 5.0F, 28.0F, 14.0F},
                    ComponentSizeMetrics{18.0F, 14.0F, 7.0F, 32.0F, 16.0F},
                    ComponentSizeMetrics{18.0F, 18.0F, 10.0F, 40.0F, 20.0F},
                    ComponentSizeMetrics{20.0F, 22.0F, 13.0F, 48.0F, 24.0F},
                },
                std::array{0.0F, 4.0F, 8.0F, 12.0F, 16.0F, 24.0F},
            },
        };
    }

    /**
     * @brief Resolves a semantic visible-text size from the active theme tokens.
     * @param tokens Active resolved design tokens.
     * @param role Semantic text role requested by the UI surface.
     * @return Theme-resolved size in logical UI pixels.
     */
    [[nodiscard]] constexpr float TypographyFor(const DesignTokens &tokens, const TypographyRole role) noexcept {
        switch (role) {
            case TypographyRole::Caption:
                return tokens.typography.caption;
            case TypographyRole::Label:
                return tokens.typography.label;
            case TypographyRole::Body:
                return tokens.typography.body;
            case TypographyRole::CardTitle:
                return tokens.typography.cardTitle;
            case TypographyRole::Title:
                return tokens.typography.title;
            case TypographyRole::Heading:
                return tokens.typography.heading;
            case TypographyRole::Display:
                return tokens.typography.display;
        }
        return tokens.typography.body;
    }

}  // namespace Horo::Editor::DesignSystem
