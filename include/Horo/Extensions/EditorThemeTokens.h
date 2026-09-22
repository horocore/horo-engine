#pragma once

/**
 * @file EditorThemeTokens.h
 * @brief Versioned backend-neutral semantic theme tokens for GUI extensions.
 */

#include "Horo/Foundation/Result.h"

#include <cstdint>

namespace Horo::Extensions {
    inline constexpr std::uint32_t EditorThemeTokenSchemaVersion = 1U;
    inline constexpr std::uint32_t EditorThemeTokenMinimumCompatibleSchemaVersion = 1U;

    /** @brief Semantic color role resolved by the host; extensions never provide theme literals. */
    enum class EditorThemeColorRole : std::uint8_t {
        None,
        Surface,
        SurfaceSubtle,
        TextPrimary,
        TextSecondary,
        TextDisabled,
        Border,
        Accent,
        Focus,
        Positive,
        Warning,
        Critical,
        SurfaceRaised,
        SurfaceHover,
        BorderStrong,
        AccentHover,
        AccentActive,
        OnAccent,
        Overlay,
        Count,
    };

    /** @brief Semantic visible-text role resolved by the host typography scale. */
    enum class EditorThemeTypographyRole : std::uint8_t {
        None,
        Caption,
        Label,
        Body,
        CardTitle,
        Title,
        Heading,
        Display,
        Count,
    };

    /** @brief Semantic spacing role resolved by the host layout scale. */
    enum class EditorThemeSpacingRole : std::uint8_t {
        None,
        XS,
        Small,
        Medium,
        Large,
        XL,
        CardPadding,
        GridGap,
        BodyPaddingX,
        BodyPaddingY,
        SidebarPaddingX,
        SidebarPaddingY,
        PropertyRowGap,
        ControlGap,
        Count,
    };

    /** @brief Semantic size role resolved by the host's DPI-aware geometry scale. */
    enum class EditorThemeSizeRole : std::uint8_t {
        None,
        SmallControlHeight,
        MediumControlHeight,
        LargeControlHeight,
        TextLineHeight,
        DefaultWidth,
        WelcomeSideWidth,
        WelcomePadding,
        ModalWidth,
        ModalHeight,
        ModalHeaderHeight,
        ModalFooterHeight,
        ModalSidebarWidth,
        SettingsWidth,
        SettingsHeight,
        IconSmall,
        IconMedium,
        IconLarge,
        MinimumInteractiveTarget,
        Count,
    };

    /** @brief Semantic corner-radius role resolved by the host design system. */
    enum class EditorThemeRadiusRole : std::uint8_t {
        None,
        Control,
        Card,
        Modal,
        Popup,
        FocusRing,
        Count,
    };

    /** @brief Semantic interaction state role resolved by the host interaction policy. */
    enum class EditorThemeInteractionRole : std::uint8_t {
        None,
        Default,
        Hover,
        Active,
        Focused,
        Disabled,
        ReadOnly,
        Selected,
        Loading,
        Pressed,
        Count,
    };

    /** @brief Semantic motion duration role resolved by the host motion policy. */
    enum class EditorThemeMotionRole : std::uint8_t {
        None,
        Instant,
        Fast,
        Normal,
        Slow,
        Hover,
        Press,
        Focus,
        Modal,
        Count,
    };

    /** @brief Semantic font role resolved to a host-owned font resource. */
    enum class EditorThemeFontRole : std::uint8_t {
        None,
        Sans,
        SansCompact,
        SansEmphasis,
        Monospace,
        Icon,
        Count,
    };

    /** @brief Semantic icon role resolved by the host icon registry. */
    enum class EditorThemeIconRole : std::uint8_t {
        None,
        Generic,
        Info,
        Warning,
        Error,
        Create,
        Rename,
        Duplicate,
        Delete,
        Reset,
        Check,
        Settings,
        More,
        Visibility,
        VisibilityOff,
        Lock,
        Search,
        Back,
        Forward,
        Folder,
        File,
        Play,
        Pause,
        Stop,
        Record,
        Count,
    };

    /** @brief Semantic frame-change categories used for live theme invalidation. */
    enum class EditorThemeChange : std::uint32_t {
        None = 0U,
        Colors = 1U << 0U,
        Typography = 1U << 1U,
        Spacing = 1U << 2U,
        Sizes = 1U << 3U,
        Radii = 1U << 4U,
        Interaction = 1U << 5U,
        Motion = 1U << 6U,
        Fonts = 1U << 7U,
        Icons = 1U << 8U,
        Accessibility = 1U << 9U,
        Dpi = 1U << 10U,
    };

    /** @brief Color-vision accessibility mode resolved into the active frame. */
    enum class EditorThemeColorVisionMode : std::uint8_t {
        None,
        Protanopia,
        Deuteranopia,
        Tritanopia,
        Achromatopsia,
        Count,
    };

    namespace ThemeTokenDetail {
        [[nodiscard]] constexpr std::uint64_t RoleBit(const std::uint8_t value, const std::uint8_t count) noexcept {
            return value == 0U || value >= count || value >= 64U ? 0ULL : (1ULL << (value - 1U));
        }

        [[nodiscard]] constexpr std::uint64_t AllRoleMask(const std::uint8_t count) noexcept {
            return count <= 1U ? 0ULL : ((1ULL << (count - 1U)) - 1ULL);
        }
    }  // namespace ThemeTokenDetail

    /** @brief Returns the support-mask bit for one color role. @param role Role to encode. @return Mask bit or zero for None/invalid. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeColorRoleBit(const EditorThemeColorRole role) noexcept {
        return ThemeTokenDetail::RoleBit(static_cast<std::uint8_t>(role), static_cast<std::uint8_t>(EditorThemeColorRole::Count));
    }

    /** @brief Returns the support-mask bit for one typography role. @param role Role to encode. @return Mask bit or zero for None/invalid.
     */
    [[nodiscard]] constexpr std::uint64_t EditorThemeTypographyRoleBit(const EditorThemeTypographyRole role) noexcept {
        return ThemeTokenDetail::RoleBit(static_cast<std::uint8_t>(role), static_cast<std::uint8_t>(EditorThemeTypographyRole::Count));
    }

    /** @brief Returns the support-mask bit for one spacing role. @param role Role to encode. @return Mask bit or zero for None/invalid. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeSpacingRoleBit(const EditorThemeSpacingRole role) noexcept {
        return ThemeTokenDetail::RoleBit(static_cast<std::uint8_t>(role), static_cast<std::uint8_t>(EditorThemeSpacingRole::Count));
    }

    /** @brief Returns the support-mask bit for one size role. @param role Role to encode. @return Mask bit or zero for None/invalid. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeSizeRoleBit(const EditorThemeSizeRole role) noexcept {
        return ThemeTokenDetail::RoleBit(static_cast<std::uint8_t>(role), static_cast<std::uint8_t>(EditorThemeSizeRole::Count));
    }

    /** @brief Returns the support-mask bit for one radius role. @param role Role to encode. @return Mask bit or zero for None/invalid. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeRadiusRoleBit(const EditorThemeRadiusRole role) noexcept {
        return ThemeTokenDetail::RoleBit(static_cast<std::uint8_t>(role), static_cast<std::uint8_t>(EditorThemeRadiusRole::Count));
    }

    /** @brief Returns the support-mask bit for one interaction role. @param role Role to encode. @return Mask bit or zero for None/invalid.
     */
    [[nodiscard]] constexpr std::uint64_t EditorThemeInteractionRoleBit(const EditorThemeInteractionRole role) noexcept {
        return ThemeTokenDetail::RoleBit(static_cast<std::uint8_t>(role), static_cast<std::uint8_t>(EditorThemeInteractionRole::Count));
    }

    /** @brief Returns the support-mask bit for one motion role. @param role Role to encode. @return Mask bit or zero for None/invalid. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeMotionRoleBit(const EditorThemeMotionRole role) noexcept {
        return ThemeTokenDetail::RoleBit(static_cast<std::uint8_t>(role), static_cast<std::uint8_t>(EditorThemeMotionRole::Count));
    }

    /** @brief Returns the support-mask bit for one font role. @param role Role to encode. @return Mask bit or zero for None/invalid. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeFontRoleBit(const EditorThemeFontRole role) noexcept {
        return ThemeTokenDetail::RoleBit(static_cast<std::uint8_t>(role), static_cast<std::uint8_t>(EditorThemeFontRole::Count));
    }

    /** @brief Returns the support-mask bit for one icon role. @param role Role to encode. @return Mask bit or zero for None/invalid. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeIconRoleBit(const EditorThemeIconRole role) noexcept {
        return ThemeTokenDetail::RoleBit(static_cast<std::uint8_t>(role), static_cast<std::uint8_t>(EditorThemeIconRole::Count));
    }

    /** @brief Returns the change-mask bit for one frame-change category. @param change Category to encode. @return Mask bit or zero for
     * None/invalid. */
    [[nodiscard]] constexpr std::uint32_t EditorThemeChangeBit(const EditorThemeChange change) noexcept {
        switch (change) {
            case EditorThemeChange::Colors:
            case EditorThemeChange::Typography:
            case EditorThemeChange::Spacing:
            case EditorThemeChange::Sizes:
            case EditorThemeChange::Radii:
            case EditorThemeChange::Interaction:
            case EditorThemeChange::Motion:
            case EditorThemeChange::Fonts:
            case EditorThemeChange::Icons:
            case EditorThemeChange::Accessibility:
            case EditorThemeChange::Dpi:
                return static_cast<std::uint32_t>(change);
            case EditorThemeChange::None:
            default:
                return 0U;
        }
    }

    /** @brief Returns all known color-role support bits. @return Complete color-role mask. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeAllColorRoleMask() noexcept {
        return ThemeTokenDetail::AllRoleMask(static_cast<std::uint8_t>(EditorThemeColorRole::Count));
    }

    /** @brief Returns all known typography-role support bits. @return Complete typography-role mask. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeAllTypographyRoleMask() noexcept {
        return ThemeTokenDetail::AllRoleMask(static_cast<std::uint8_t>(EditorThemeTypographyRole::Count));
    }

    /** @brief Returns all known spacing-role support bits. @return Complete spacing-role mask. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeAllSpacingRoleMask() noexcept {
        return ThemeTokenDetail::AllRoleMask(static_cast<std::uint8_t>(EditorThemeSpacingRole::Count));
    }

    /** @brief Returns all known size-role support bits. @return Complete size-role mask. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeAllSizeRoleMask() noexcept {
        return ThemeTokenDetail::AllRoleMask(static_cast<std::uint8_t>(EditorThemeSizeRole::Count));
    }

    /** @brief Returns all known radius-role support bits. @return Complete radius-role mask. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeAllRadiusRoleMask() noexcept {
        return ThemeTokenDetail::AllRoleMask(static_cast<std::uint8_t>(EditorThemeRadiusRole::Count));
    }

    /** @brief Returns all known interaction-role support bits. @return Complete interaction-role mask. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeAllInteractionRoleMask() noexcept {
        return ThemeTokenDetail::AllRoleMask(static_cast<std::uint8_t>(EditorThemeInteractionRole::Count));
    }

    /** @brief Returns all known motion-role support bits. @return Complete motion-role mask. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeAllMotionRoleMask() noexcept {
        return ThemeTokenDetail::AllRoleMask(static_cast<std::uint8_t>(EditorThemeMotionRole::Count));
    }

    /** @brief Returns all known font-role support bits. @return Complete font-role mask. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeAllFontRoleMask() noexcept {
        return ThemeTokenDetail::AllRoleMask(static_cast<std::uint8_t>(EditorThemeFontRole::Count));
    }

    /** @brief Returns all known icon-role support bits. @return Complete icon-role mask. */
    [[nodiscard]] constexpr std::uint64_t EditorThemeAllIconRoleMask() noexcept {
        return ThemeTokenDetail::AllRoleMask(static_cast<std::uint8_t>(EditorThemeIconRole::Count));
    }

    /** @brief Returns all known frame-change bits. @return Complete change mask. */
    [[nodiscard]] constexpr std::uint32_t EditorThemeAllChangeMask() noexcept {
        return static_cast<std::uint32_t>(EditorThemeChange::Colors) | static_cast<std::uint32_t>(EditorThemeChange::Typography) |
               static_cast<std::uint32_t>(EditorThemeChange::Spacing) | static_cast<std::uint32_t>(EditorThemeChange::Sizes) |
               static_cast<std::uint32_t>(EditorThemeChange::Radii) | static_cast<std::uint32_t>(EditorThemeChange::Interaction) |
               static_cast<std::uint32_t>(EditorThemeChange::Motion) | static_cast<std::uint32_t>(EditorThemeChange::Fonts) |
               static_cast<std::uint32_t>(EditorThemeChange::Icons) | static_cast<std::uint32_t>(EditorThemeChange::Accessibility) |
               static_cast<std::uint32_t>(EditorThemeChange::Dpi);
    }

    /** @brief Backend-neutral normalized RGBA value resolved for one color role. */
    struct EditorThemeColor final {
        float red{};
        float green{};
        float blue{};
        float alpha{1.0F};

        /** @brief Checks that every channel is finite and normalized. @return True when the color is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Resolved semantic color values for one immutable theme frame. */
    struct EditorThemeColorTokens final {
        EditorThemeColor surface{0.039F, 0.047F, 0.059F, 1.0F};
        EditorThemeColor surfaceSubtle{0.071F, 0.082F, 0.102F, 1.0F};
        EditorThemeColor textPrimary{0.910F, 0.894F, 0.851F, 1.0F};
        EditorThemeColor textSecondary{0.604F, 0.584F, 0.541F, 1.0F};
        EditorThemeColor textDisabled{0.369F, 0.357F, 0.329F, 1.0F};
        EditorThemeColor border{0.165F, 0.184F, 0.216F, 1.0F};
        EditorThemeColor accent{0.016F, 0.647F, 0.988F, 1.0F};
        EditorThemeColor focus{0.180F, 0.706F, 0.992F, 1.0F};
        EditorThemeColor positive{0.373F, 0.722F, 0.541F, 1.0F};
        EditorThemeColor warning{0.910F, 0.639F, 0.239F, 1.0F};
        EditorThemeColor critical{0.831F, 0.322F, 0.290F, 1.0F};
        EditorThemeColor surfaceRaised{0.122F, 0.141F, 0.169F, 1.0F};
        EditorThemeColor surfaceHover{0.137F, 0.157F, 0.188F, 1.0F};
        EditorThemeColor borderStrong{0.227F, 0.251F, 0.286F, 1.0F};
        EditorThemeColor accentHover{0.180F, 0.706F, 0.992F, 1.0F};
        EditorThemeColor accentActive{0.000F, 0.500F, 0.820F, 1.0F};
        EditorThemeColor onAccent{0.020F, 0.075F, 0.110F, 1.0F};
        EditorThemeColor overlay{0.000F, 0.000F, 0.000F, 0.55F};
    };

    /** @brief Resolved semantic visible-text sizes in logical UI pixels. */
    struct EditorThemeTypographyTokens final {
        float caption{14.0F};
        float label{14.0F};
        float body{16.0F};
        float cardTitle{16.0F};
        float title{18.0F};
        float heading{22.0F};
        float display{28.0F};
    };

    /** @brief Resolved semantic spacing values in logical UI pixels. */
    struct EditorThemeSpacingTokens final {
        float xs{4.0F};
        float small{8.0F};
        float medium{12.0F};
        float large{16.0F};
        float xl{24.0F};
        float cardPadding{18.0F};
        float gridGap{14.0F};
        float bodyPaddingX{28.0F};
        float bodyPaddingY{24.0F};
        float sidebarPaddingX{14.0F};
        float sidebarPaddingY{18.0F};
        float propertyRowGap{8.0F};
        float controlGap{8.0F};
    };

    /** @brief Resolved semantic geometry values; the first fields preserve the form-kit metrics contract. */
    struct EditorThemeSizeTokens final {
        float smallControlHeight{24.0F};
        float mediumControlHeight{32.0F};
        float largeControlHeight{40.0F};
        float textLineHeight{20.0F};
        float rowGap{8.0F};
        float defaultWidth{480.0F};
        float welcomeSideWidth{280.0F};
        float welcomePadding{32.0F};
        float modalWidth{900.0F};
        float modalHeight{680.0F};
        float modalHeaderHeight{58.0F};
        float modalFooterHeight{52.0F};
        float modalSidebarWidth{220.0F};
        float settingsWidth{620.0F};
        float settingsHeight{440.0F};
        float iconSmall{12.0F};
        float iconMedium{16.0F};
        float iconLarge{24.0F};
        float minimumInteractiveTarget{24.0F};
    };

    /** @brief Resolved semantic corner radii in logical UI pixels. */
    struct EditorThemeRadiusTokens final {
        float control{4.0F};
        float card{6.0F};
        float modal{8.0F};
        float popup{6.0F};
        float focusRing{2.0F};
    };

    /** @brief Resolved interaction geometry and state-affordance values. */
    struct EditorThemeInteractionTokens final {
        float hoverOpacity{0.12F};
        float activeOpacity{0.20F};
        float disabledOpacity{0.45F};
        float focusRingWidth{2.0F};
        float focusRingOffset{1.0F};
        float pressedScale{0.98F};
        bool keyboardFocusVisible{true};
    };

    /** @brief Resolved motion timings and accessibility motion policy. */
    struct EditorThemeMotionTokens final {
        float instantSeconds{0.0F};
        float fastSeconds{0.10F};
        float normalSeconds{0.18F};
        float slowSeconds{0.30F};
        float hoverSeconds{0.12F};
        float pressSeconds{0.08F};
        float focusSeconds{0.15F};
        float modalSeconds{0.22F};
        bool reduceMotion{};
    };

    /** @brief Semantic font roles and their resolved logical sizes. */
    struct EditorThemeFontTokens final {
        EditorThemeFontRole body{EditorThemeFontRole::Sans};
        EditorThemeFontRole compact{EditorThemeFontRole::SansCompact};
        EditorThemeFontRole emphasis{EditorThemeFontRole::SansEmphasis};
        EditorThemeFontRole technical{EditorThemeFontRole::Monospace};
        EditorThemeFontRole icons{EditorThemeFontRole::Icon};
        float sansBase{16.0F};
        float sansCompactBase{14.0F};
        float sansEmphasisBase{16.0F};
        float monospaceBase{14.0F};
        float iconBase{16.0F};
    };

    /** @brief Semantic icon sizing and stroke values; glyph resources remain host-owned. */
    struct EditorThemeIconTokens final {
        float smallSize{12.0F};
        float mediumSize{16.0F};
        float largeSize{24.0F};
        float strokeWidth{1.5F};
    };

    /** @brief Effective accessibility settings captured with the theme frame. */
    struct EditorThemeAccessibility final {
        bool highContrast{};
        bool reduceMotion{};
        bool disableFlashEffects{};
        float textContrastMultiplier{1.0F};
        EditorThemeColorVisionMode colorVision{EditorThemeColorVisionMode::None};
        float colorVisionSeverity{1.0F};
    };

    /** @brief Bounds used when validating a host-supplied theme frame. */
    struct EditorThemeTokenLimits final {
        float minimumUiScale{0.5F};
        float maximumUiScale{3.0F};
        float minimumReadableTextSize{14.0F};
        float maximumMotionSeconds{60.0F};
    };

    /**
     * @brief Complete immutable, frame-scoped theme evidence supplied to an extension adapter.
     *
     * A host publishes a new value at an editor frame boundary whenever any active
     * built-in, custom, project, accessibility, or DPI input changes. Extensions
     * must retain neither this value nor a derived color/metric cache across frames.
     * `supportedTokenMask` is the legacy form-kit name for the color-role mask.
     */
    struct EditorThemeFrame final {
        std::uint32_t schemaVersion{EditorThemeTokenSchemaVersion};
        std::uint64_t revision{1};
        float uiScale{1.0F};
        std::uint32_t changeMask{EditorThemeAllChangeMask()};
        std::uint64_t supportedTokenMask{EditorThemeAllColorRoleMask()};
        std::uint64_t supportedTypographyMask{EditorThemeAllTypographyRoleMask()};
        std::uint64_t supportedSpacingMask{EditorThemeAllSpacingRoleMask()};
        std::uint64_t supportedSizeMask{EditorThemeAllSizeRoleMask()};
        std::uint64_t supportedRadiusMask{EditorThemeAllRadiusRoleMask()};
        std::uint64_t supportedInteractionMask{EditorThemeAllInteractionRoleMask()};
        std::uint64_t supportedMotionMask{EditorThemeAllMotionRoleMask()};
        std::uint64_t supportedFontMask{EditorThemeAllFontRoleMask()};
        std::uint64_t supportedIconMask{EditorThemeAllIconRoleMask()};
        EditorThemeColorTokens colors;
        EditorThemeTypographyTokens typography;
        EditorThemeSpacingTokens spacing;
        EditorThemeSizeTokens metrics;
        EditorThemeRadiusTokens radii;
        EditorThemeInteractionTokens interaction;
        EditorThemeMotionTokens motion;
        EditorThemeFontTokens fonts;
        EditorThemeIconTokens icons;
        EditorThemeAccessibility accessibility;
    };

    /**
     * @brief Validates one complete frame without registering services or selecting a backend.
     * @param frame Host-resolved immutable theme evidence.
     * @param limits Finite validation bounds for scale, text, and motion values.
     * @return Success or a typed extension theme-contract error.
     */
    [[nodiscard]] Result<void> ValidateEditorThemeFrame(const EditorThemeFrame &frame, const EditorThemeTokenLimits &limits = {});

    /**
     * @brief Resolves an unavailable color role through the stable semantic fallback chain.
     * @param requested Requested color role.
     * @param frame Frame whose supported-role mask is authoritative.
     * @return Requested role, a safe supported fallback, or None.
     */
    [[nodiscard]] EditorThemeColorRole ResolveEditorThemeColorRole(EditorThemeColorRole requested, const EditorThemeFrame &frame) noexcept;

    /**
     * @brief Resolves an unavailable typography role through the stable fallback chain.
     * @param requested Requested typography role.
     * @param frame Frame whose supported-role mask is authoritative.
     * @return Requested role, a safe supported fallback, or None.
     */
    [[nodiscard]] EditorThemeTypographyRole ResolveEditorThemeTypographyRole(EditorThemeTypographyRole requested,
                                                                             const EditorThemeFrame &frame) noexcept;

    /**
     * @brief Resolves an unavailable spacing role through the stable fallback chain.
     * @param requested Requested spacing role.
     * @param frame Frame whose supported-role mask is authoritative.
     * @return Requested role, a safe supported fallback, or None.
     */
    [[nodiscard]] EditorThemeSpacingRole ResolveEditorThemeSpacingRole(EditorThemeSpacingRole requested,
                                                                       const EditorThemeFrame &frame) noexcept;

    /**
     * @brief Resolves an unavailable size role through the stable fallback chain.
     * @param requested Requested size role.
     * @param frame Frame whose supported-role mask is authoritative.
     * @return Requested role, a safe supported fallback, or None.
     */
    [[nodiscard]] EditorThemeSizeRole ResolveEditorThemeSizeRole(EditorThemeSizeRole requested, const EditorThemeFrame &frame) noexcept;

    /**
     * @brief Resolves an unavailable radius role through the stable fallback chain.
     * @param requested Requested radius role.
     * @param frame Frame whose supported-role mask is authoritative.
     * @return Requested role, a safe supported fallback, or None.
     */
    [[nodiscard]] EditorThemeRadiusRole ResolveEditorThemeRadiusRole(EditorThemeRadiusRole requested,
                                                                     const EditorThemeFrame &frame) noexcept;

    /**
     * @brief Resolves an unavailable interaction role through the stable fallback chain.
     * @param requested Requested interaction role.
     * @param frame Frame whose supported-role mask is authoritative.
     * @return Requested role, a safe supported fallback, or None.
     */
    [[nodiscard]] EditorThemeInteractionRole ResolveEditorThemeInteractionRole(EditorThemeInteractionRole requested,
                                                                               const EditorThemeFrame &frame) noexcept;

    /**
     * @brief Resolves an unavailable motion role and honors reduced-motion policy.
     * @param requested Requested motion role.
     * @param frame Frame whose supported-role mask and accessibility policy are authoritative.
     * @return Requested role, Instant for reduced motion, or None.
     */
    [[nodiscard]] EditorThemeMotionRole ResolveEditorThemeMotionRole(EditorThemeMotionRole requested,
                                                                     const EditorThemeFrame &frame) noexcept;

    /**
     * @brief Resolves an unavailable font role through the stable fallback chain.
     * @param requested Requested font role.
     * @param frame Frame whose supported-role mask is authoritative.
     * @return Requested role, Sans, or None.
     */
    [[nodiscard]] EditorThemeFontRole ResolveEditorThemeFontRole(EditorThemeFontRole requested, const EditorThemeFrame &frame) noexcept;

    /**
     * @brief Resolves an unavailable icon role through the stable fallback chain.
     * @param requested Requested icon role.
     * @param frame Frame whose supported-role mask is authoritative.
     * @return Requested role, Generic, or None.
     */
    [[nodiscard]] EditorThemeIconRole ResolveEditorThemeIconRole(EditorThemeIconRole requested, const EditorThemeFrame &frame) noexcept;

    /**
     * @brief Returns the resolved color value for a semantic role.
     * @param frame Immutable theme frame.
     * @param role Requested semantic color role.
     * @return Resolved color; invalid or unavailable roles return a safe transparent value.
     */
    [[nodiscard]] EditorThemeColor ColorFor(const EditorThemeFrame &frame, EditorThemeColorRole role) noexcept;

    /**
     * @brief Returns the resolved typography size for a semantic role.
     * @param frame Immutable theme frame.
     * @param role Requested semantic typography role.
     * @return Resolved logical pixel size, or zero for None.
     */
    [[nodiscard]] float TypographyFor(const EditorThemeFrame &frame, EditorThemeTypographyRole role) noexcept;

    /**
     * @brief Returns the resolved spacing value for a semantic role.
     * @param frame Immutable theme frame.
     * @param role Requested semantic spacing role.
     * @return Resolved logical pixel spacing, or zero for None.
     */
    [[nodiscard]] float SpacingFor(const EditorThemeFrame &frame, EditorThemeSpacingRole role) noexcept;

    /**
     * @brief Returns the resolved size value for a semantic role.
     * @param frame Immutable theme frame.
     * @param role Requested semantic size role.
     * @return Resolved logical pixel size, or zero for None.
     */
    [[nodiscard]] float SizeFor(const EditorThemeFrame &frame, EditorThemeSizeRole role) noexcept;

    /**
     * @brief Returns the resolved radius value for a semantic role.
     * @param frame Immutable theme frame.
     * @param role Requested semantic radius role.
     * @return Resolved logical pixel radius, or zero for None.
     */
    [[nodiscard]] float RadiusFor(const EditorThemeFrame &frame, EditorThemeRadiusRole role) noexcept;

    /**
     * @brief Returns the resolved motion duration for a semantic role.
     * @param frame Immutable theme frame.
     * @param role Requested semantic motion role.
     * @return Resolved duration in seconds, or zero for None/reduced motion.
     */
    [[nodiscard]] float MotionFor(const EditorThemeFrame &frame, EditorThemeMotionRole role) noexcept;

    /**
     * @brief Returns the resolved font size for a semantic font role.
     * @param frame Immutable theme frame.
     * @param role Requested semantic font role.
     * @return Resolved logical pixel size, or zero for None.
     */
    [[nodiscard]] float FontSizeFor(const EditorThemeFrame &frame, EditorThemeFontRole role) noexcept;
}  // namespace Horo::Extensions
