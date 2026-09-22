#include "Horo/Extensions/EditorThemeTokens.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace Horo::Extensions {
    namespace {
        [[nodiscard]] Result<void> InvalidTheme(const std::string_view reason) {
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorThemeTokenInvalid, std::string{reason}));
        }

        [[nodiscard]] bool IsFiniteInRange(const float value, const float minimum, const float maximum) noexcept {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        }

        [[nodiscard]] bool IsFiniteNonNegative(const float value) noexcept {
            return std::isfinite(value) && value >= 0.0F;
        }

        template <typename Role, typename BitFunction>
        [[nodiscard]] bool IsSupported(const std::uint64_t mask, const Role role, const BitFunction bitFunction) noexcept {
            const std::uint64_t bit = bitFunction(role);
            return bit != 0U && (mask & bit) != 0U;
        }

        template <typename Role, typename BitFunction>
        [[nodiscard]] bool IsKnownMask(const std::uint64_t mask, const Role countRole, const BitFunction bitFunction) noexcept {
            const auto count = static_cast<std::uint8_t>(countRole);
            const std::uint64_t known = ThemeTokenDetail::AllRoleMask(count);
            return (mask & ~known) == 0U;
        }

        [[nodiscard]] bool ValidateColors(const EditorThemeColorTokens &colors) noexcept {
            const std::array values{&colors.surface,      &colors.surfaceSubtle, &colors.textPrimary, &colors.textSecondary,
                                    &colors.textDisabled, &colors.border,        &colors.accent,      &colors.focus,
                                    &colors.positive,     &colors.warning,       &colors.critical,    &colors.surfaceRaised,
                                    &colors.surfaceHover, &colors.borderStrong,  &colors.accentHover, &colors.accentActive,
                                    &colors.onAccent,     &colors.overlay};
            for (const EditorThemeColor *const color : values) {
                if (!color->IsValid())
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool ValidateTypography(const EditorThemeTypographyTokens &tokens, const float minimumReadable) noexcept {
            if (!IsFiniteInRange(tokens.caption, minimumReadable, 4096.0F) || !IsFiniteInRange(tokens.label, minimumReadable, 4096.0F) ||
                !IsFiniteInRange(tokens.body, minimumReadable, 4096.0F) || !IsFiniteInRange(tokens.cardTitle, minimumReadable, 4096.0F) ||
                !IsFiniteInRange(tokens.title, minimumReadable, 4096.0F) || !IsFiniteInRange(tokens.heading, minimumReadable, 4096.0F) ||
                !IsFiniteInRange(tokens.display, minimumReadable, 4096.0F))
                return false;
            return tokens.body <= tokens.cardTitle && tokens.cardTitle <= tokens.title && tokens.title <= tokens.heading &&
                   tokens.heading <= tokens.display;
        }

        [[nodiscard]] bool ValidateSpacing(const EditorThemeSpacingTokens &tokens) noexcept {
            const std::array values{tokens.xs,           tokens.small,           tokens.medium,          tokens.large,
                                    tokens.xl,           tokens.cardPadding,     tokens.gridGap,         tokens.bodyPaddingX,
                                    tokens.bodyPaddingY, tokens.sidebarPaddingX, tokens.sidebarPaddingY, tokens.propertyRowGap,
                                    tokens.controlGap};
            for (const float value : values) {
                if (!IsFiniteNonNegative(value))
                    return false;
            }
            return tokens.xs <= tokens.small && tokens.small <= tokens.medium && tokens.medium <= tokens.large && tokens.large <= tokens.xl;
        }

        [[nodiscard]] bool ValidateSizes(const EditorThemeSizeTokens &tokens) noexcept {
            const std::array values{tokens.smallControlHeight,
                                    tokens.mediumControlHeight,
                                    tokens.largeControlHeight,
                                    tokens.textLineHeight,
                                    tokens.rowGap,
                                    tokens.defaultWidth,
                                    tokens.welcomeSideWidth,
                                    tokens.welcomePadding,
                                    tokens.modalWidth,
                                    tokens.modalHeight,
                                    tokens.modalHeaderHeight,
                                    tokens.modalFooterHeight,
                                    tokens.modalSidebarWidth,
                                    tokens.settingsWidth,
                                    tokens.settingsHeight,
                                    tokens.iconSmall,
                                    tokens.iconMedium,
                                    tokens.iconLarge,
                                    tokens.minimumInteractiveTarget};
            for (const float value : values) {
                if (!std::isfinite(value) || value <= 0.0F)
                    return false;
            }
            return tokens.smallControlHeight <= tokens.mediumControlHeight && tokens.mediumControlHeight <= tokens.largeControlHeight &&
                   tokens.iconSmall <= tokens.iconMedium && tokens.iconMedium <= tokens.iconLarge;
        }

        [[nodiscard]] bool ValidateRadii(const EditorThemeRadiusTokens &tokens) noexcept {
            return IsFiniteNonNegative(tokens.control) && IsFiniteNonNegative(tokens.card) && IsFiniteNonNegative(tokens.modal) &&
                   IsFiniteNonNegative(tokens.popup) && IsFiniteNonNegative(tokens.focusRing);
        }

        [[nodiscard]] bool ValidateInteraction(const EditorThemeInteractionTokens &tokens) noexcept {
            return IsFiniteInRange(tokens.hoverOpacity, 0.0F, 1.0F) && IsFiniteInRange(tokens.activeOpacity, 0.0F, 1.0F) &&
                   IsFiniteInRange(tokens.disabledOpacity, 0.0F, 1.0F) && IsFiniteNonNegative(tokens.focusRingWidth) &&
                   IsFiniteNonNegative(tokens.focusRingOffset) && IsFiniteInRange(tokens.pressedScale, 0.0F, 1.0F);
        }

        [[nodiscard]] bool ValidateMotion(const EditorThemeMotionTokens &tokens, const float maximumSeconds) noexcept {
            const std::array values{tokens.instantSeconds, tokens.fastSeconds,  tokens.normalSeconds, tokens.slowSeconds,
                                    tokens.hoverSeconds,   tokens.pressSeconds, tokens.focusSeconds,  tokens.modalSeconds};
            for (const float value : values) {
                if (!IsFiniteInRange(value, 0.0F, maximumSeconds))
                    return false;
            }
            return tokens.instantSeconds <= tokens.fastSeconds && tokens.fastSeconds <= tokens.normalSeconds &&
                   tokens.normalSeconds <= tokens.slowSeconds;
        }

        [[nodiscard]] bool ValidateFonts(const EditorThemeFontTokens &tokens) noexcept {
            const std::array roles{tokens.body, tokens.compact, tokens.emphasis, tokens.technical, tokens.icons};
            for (const EditorThemeFontRole role : roles) {
                if (static_cast<std::uint8_t>(role) >= static_cast<std::uint8_t>(EditorThemeFontRole::Count) ||
                    role == EditorThemeFontRole::None)
                    return false;
            }
            const std::array sizes{tokens.sansBase, tokens.sansCompactBase, tokens.sansEmphasisBase, tokens.monospaceBase, tokens.iconBase};
            for (const float size : sizes) {
                if (!std::isfinite(size) || size <= 0.0F)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool ValidateIcons(const EditorThemeIconTokens &tokens) noexcept {
            return std::isfinite(tokens.smallSize) && tokens.smallSize > 0.0F && std::isfinite(tokens.mediumSize) &&
                   tokens.mediumSize > 0.0F && std::isfinite(tokens.largeSize) && tokens.largeSize > 0.0F &&
                   tokens.smallSize <= tokens.mediumSize && tokens.mediumSize <= tokens.largeSize && std::isfinite(tokens.strokeWidth) &&
                   tokens.strokeWidth > 0.0F;
        }

        [[nodiscard]] bool ValidateAccessibility(const EditorThemeAccessibility &accessibility) noexcept {
            return static_cast<std::uint8_t>(accessibility.colorVision) < static_cast<std::uint8_t>(EditorThemeColorVisionMode::Count) &&
                   IsFiniteInRange(accessibility.textContrastMultiplier, 1.0F, 4.0F) &&
                   IsFiniteInRange(accessibility.colorVisionSeverity, 0.0F, 1.0F);
        }

        [[nodiscard]] EditorThemeColorRole FirstSupportedColor(const EditorThemeFrame &frame,
                                                               const std::array<EditorThemeColorRole, 4> candidates) noexcept {
            for (const EditorThemeColorRole candidate : candidates) {
                if (IsSupported(frame.supportedTokenMask, candidate, EditorThemeColorRoleBit))
                    return candidate;
            }
            return EditorThemeColorRole::None;
        }

        [[nodiscard]] EditorThemeColorRole ColorFallback(const EditorThemeColorRole requested, const EditorThemeFrame &frame) noexcept {
            using enum EditorThemeColorRole;
            switch (requested) {
                case SurfaceSubtle:
                    return FirstSupportedColor(frame, {Surface, SurfaceRaised, SurfaceHover, None});
                case SurfaceRaised:
                case SurfaceHover:
                    return FirstSupportedColor(frame, {Surface, SurfaceSubtle, None, None});
                case TextDisabled:
                    return FirstSupportedColor(frame, {TextSecondary, TextPrimary, None, None});
                case Border:
                case BorderStrong:
                case Focus:
                    return FirstSupportedColor(frame, {Accent, TextSecondary, TextPrimary, None});
                case Positive:
                case Warning:
                case Critical:
                    return FirstSupportedColor(frame, {Accent, TextPrimary, TextSecondary, None});
                case AccentHover:
                case AccentActive:
                case OnAccent:
                    return FirstSupportedColor(frame, {Accent, TextPrimary, None, None});
                case Overlay:
                    return FirstSupportedColor(frame, {SurfaceSubtle, Surface, None, None});
                case Surface:
                case TextPrimary:
                case TextSecondary:
                case Accent:
                case None:
                case Count:
                default:
                    return None;
            }
        }

        [[nodiscard]] EditorThemeTypographyRole TypographyFallback(const EditorThemeTypographyRole requested,
                                                                   const EditorThemeFrame &frame) noexcept {
            using enum EditorThemeTypographyRole;
            const auto supports = [&frame](const EditorThemeTypographyRole role) {
                return IsSupported(frame.supportedTypographyMask, role, EditorThemeTypographyRoleBit);
            };
            const std::array candidates = requested == Caption || requested == Label ? std::array{Body, CardTitle, Title, None}
                                                                                     : std::array{Body, Label, Caption, None};
            for (const EditorThemeTypographyRole candidate : candidates) {
                if (supports(candidate))
                    return candidate;
            }
            return None;
        }

        [[nodiscard]] EditorThemeSpacingRole SpacingFallback(const EditorThemeSpacingRole requested,
                                                             const EditorThemeFrame &frame) noexcept {
            using enum EditorThemeSpacingRole;
            const auto supports = [&frame](const EditorThemeSpacingRole role) {
                return IsSupported(frame.supportedSpacingMask, role, EditorThemeSpacingRoleBit);
            };
            const std::array candidates =
                requested == XS || requested == Small ? std::array{Small, Medium, Large, None} : std::array{Medium, Large, Small, None};
            for (const EditorThemeSpacingRole candidate : candidates) {
                if (supports(candidate))
                    return candidate;
            }
            return None;
        }

        [[nodiscard]] EditorThemeSizeRole SizeFallback(const EditorThemeSizeRole requested, const EditorThemeFrame &frame) noexcept {
            using enum EditorThemeSizeRole;
            const auto supports = [&frame](const EditorThemeSizeRole role) {
                return IsSupported(frame.supportedSizeMask, role, EditorThemeSizeRoleBit);
            };
            const std::array candidates{MediumControlHeight, SmallControlHeight, LargeControlHeight, None};
            for (const EditorThemeSizeRole candidate : candidates) {
                if (supports(candidate))
                    return candidate;
            }
            return None;
        }

        [[nodiscard]] bool IsKnownRole(const EditorThemeRadiusRole role) noexcept {
            return static_cast<std::uint8_t>(role) < static_cast<std::uint8_t>(EditorThemeRadiusRole::Count);
        }

        [[nodiscard]] bool IsKnownRole(const EditorThemeInteractionRole role) noexcept {
            return static_cast<std::uint8_t>(role) < static_cast<std::uint8_t>(EditorThemeInteractionRole::Count);
        }

        [[nodiscard]] bool IsKnownRole(const EditorThemeMotionRole role) noexcept {
            return static_cast<std::uint8_t>(role) < static_cast<std::uint8_t>(EditorThemeMotionRole::Count);
        }

        [[nodiscard]] bool IsKnownRole(const EditorThemeIconRole role) noexcept {
            return static_cast<std::uint8_t>(role) < static_cast<std::uint8_t>(EditorThemeIconRole::Count);
        }
    }  // namespace

    /** @copydoc EditorThemeColor::IsValid */
    bool EditorThemeColor::IsValid() const noexcept {
        return IsFiniteInRange(red, 0.0F, 1.0F) && IsFiniteInRange(green, 0.0F, 1.0F) && IsFiniteInRange(blue, 0.0F, 1.0F) &&
               IsFiniteInRange(alpha, 0.0F, 1.0F);
    }

    /** @copydoc ValidateEditorThemeFrame */
    Result<void> ValidateEditorThemeFrame(const EditorThemeFrame &frame, const EditorThemeTokenLimits &limits) {
        if (frame.schemaVersion < EditorThemeTokenMinimumCompatibleSchemaVersion || frame.schemaVersion > EditorThemeTokenSchemaVersion)
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorThemeTokenVersionUnsupported));
        if (frame.revision == 0U || !IsFiniteInRange(frame.uiScale, limits.minimumUiScale, limits.maximumUiScale) ||
            (frame.changeMask & ~EditorThemeAllChangeMask()) != 0U ||
            !IsKnownMask(frame.supportedTokenMask, EditorThemeColorRole::Count, EditorThemeColorRoleBit) ||
            !IsKnownMask(frame.supportedTypographyMask, EditorThemeTypographyRole::Count, EditorThemeTypographyRoleBit) ||
            !IsKnownMask(frame.supportedSpacingMask, EditorThemeSpacingRole::Count, EditorThemeSpacingRoleBit) ||
            !IsKnownMask(frame.supportedSizeMask, EditorThemeSizeRole::Count, EditorThemeSizeRoleBit) ||
            !IsKnownMask(frame.supportedRadiusMask, EditorThemeRadiusRole::Count, EditorThemeRadiusRoleBit) ||
            !IsKnownMask(frame.supportedInteractionMask, EditorThemeInteractionRole::Count, EditorThemeInteractionRoleBit) ||
            !IsKnownMask(frame.supportedMotionMask, EditorThemeMotionRole::Count, EditorThemeMotionRoleBit) ||
            !IsKnownMask(frame.supportedFontMask, EditorThemeFontRole::Count, EditorThemeFontRoleBit) ||
            !IsKnownMask(frame.supportedIconMask, EditorThemeIconRole::Count, EditorThemeIconRoleBit))
            return InvalidTheme("Theme token frame metadata is outside the supported contract.");
        if (!ValidateColors(frame.colors) || !ValidateTypography(frame.typography, limits.minimumReadableTextSize) ||
            !ValidateSpacing(frame.spacing) || !ValidateSizes(frame.metrics) || !ValidateRadii(frame.radii) ||
            !ValidateInteraction(frame.interaction) || !ValidateMotion(frame.motion, limits.maximumMotionSeconds) ||
            !ValidateFonts(frame.fonts) || !ValidateIcons(frame.icons) || !ValidateAccessibility(frame.accessibility))
            return InvalidTheme("Theme token frame contains a non-finite, unordered, or out-of-range resolved value.");
        return Result<void>::Success();
    }

    /** @copydoc ResolveEditorThemeColorRole */
    EditorThemeColorRole ResolveEditorThemeColorRole(const EditorThemeColorRole requested, const EditorThemeFrame &frame) noexcept {
        if (requested == EditorThemeColorRole::None)
            return EditorThemeColorRole::None;
        if (IsSupported(frame.supportedTokenMask, requested, EditorThemeColorRoleBit))
            return requested;
        return ColorFallback(requested, frame);
    }

    /** @copydoc ResolveEditorThemeTypographyRole */
    EditorThemeTypographyRole ResolveEditorThemeTypographyRole(const EditorThemeTypographyRole requested,
                                                               const EditorThemeFrame &frame) noexcept {
        if (requested == EditorThemeTypographyRole::None)
            return EditorThemeTypographyRole::None;
        if (IsSupported(frame.supportedTypographyMask, requested, EditorThemeTypographyRoleBit))
            return requested;
        return TypographyFallback(requested, frame);
    }

    /** @copydoc ResolveEditorThemeSpacingRole */
    EditorThemeSpacingRole ResolveEditorThemeSpacingRole(const EditorThemeSpacingRole requested, const EditorThemeFrame &frame) noexcept {
        if (requested == EditorThemeSpacingRole::None)
            return EditorThemeSpacingRole::None;
        if (IsSupported(frame.supportedSpacingMask, requested, EditorThemeSpacingRoleBit))
            return requested;
        return SpacingFallback(requested, frame);
    }

    /** @copydoc ResolveEditorThemeSizeRole */
    EditorThemeSizeRole ResolveEditorThemeSizeRole(const EditorThemeSizeRole requested, const EditorThemeFrame &frame) noexcept {
        if (requested == EditorThemeSizeRole::None)
            return EditorThemeSizeRole::None;
        if (IsSupported(frame.supportedSizeMask, requested, EditorThemeSizeRoleBit))
            return requested;
        return SizeFallback(requested, frame);
    }

    /** @copydoc ResolveEditorThemeRadiusRole */
    EditorThemeRadiusRole ResolveEditorThemeRadiusRole(const EditorThemeRadiusRole requested, const EditorThemeFrame &frame) noexcept {
        if (!IsKnownRole(requested) || requested == EditorThemeRadiusRole::None)
            return EditorThemeRadiusRole::None;
        if (IsSupported(frame.supportedRadiusMask, requested, EditorThemeRadiusRoleBit))
            return requested;
        return IsSupported(frame.supportedRadiusMask, EditorThemeRadiusRole::Control, EditorThemeRadiusRoleBit)
                   ? EditorThemeRadiusRole::Control
                   : EditorThemeRadiusRole::None;
    }

    /** @copydoc ResolveEditorThemeInteractionRole */
    EditorThemeInteractionRole ResolveEditorThemeInteractionRole(const EditorThemeInteractionRole requested,
                                                                 const EditorThemeFrame &frame) noexcept {
        if (!IsKnownRole(requested) || requested == EditorThemeInteractionRole::None)
            return EditorThemeInteractionRole::None;
        if (IsSupported(frame.supportedInteractionMask, requested, EditorThemeInteractionRoleBit))
            return requested;
        return IsSupported(frame.supportedInteractionMask, EditorThemeInteractionRole::Default, EditorThemeInteractionRoleBit)
                   ? EditorThemeInteractionRole::Default
                   : EditorThemeInteractionRole::None;
    }

    /** @copydoc ResolveEditorThemeMotionRole */
    EditorThemeMotionRole ResolveEditorThemeMotionRole(const EditorThemeMotionRole requested, const EditorThemeFrame &frame) noexcept {
        if (!IsKnownRole(requested) || requested == EditorThemeMotionRole::None)
            return EditorThemeMotionRole::None;
        if (frame.accessibility.reduceMotion || frame.motion.reduceMotion)
            return IsSupported(frame.supportedMotionMask, EditorThemeMotionRole::Instant, EditorThemeMotionRoleBit)
                       ? EditorThemeMotionRole::Instant
                       : EditorThemeMotionRole::None;
        if (IsSupported(frame.supportedMotionMask, requested, EditorThemeMotionRoleBit))
            return requested;
        if (IsSupported(frame.supportedMotionMask, EditorThemeMotionRole::Normal, EditorThemeMotionRoleBit))
            return EditorThemeMotionRole::Normal;
        if (IsSupported(frame.supportedMotionMask, EditorThemeMotionRole::Fast, EditorThemeMotionRoleBit))
            return EditorThemeMotionRole::Fast;
        return IsSupported(frame.supportedMotionMask, EditorThemeMotionRole::Instant, EditorThemeMotionRoleBit)
                   ? EditorThemeMotionRole::Instant
                   : EditorThemeMotionRole::None;
    }

    /** @copydoc ResolveEditorThemeFontRole */
    EditorThemeFontRole ResolveEditorThemeFontRole(const EditorThemeFontRole requested, const EditorThemeFrame &frame) noexcept {
        if (requested == EditorThemeFontRole::None ||
            static_cast<std::uint8_t>(requested) >= static_cast<std::uint8_t>(EditorThemeFontRole::Count))
            return EditorThemeFontRole::None;
        if (IsSupported(frame.supportedFontMask, requested, EditorThemeFontRoleBit))
            return requested;
        return IsSupported(frame.supportedFontMask, EditorThemeFontRole::Sans, EditorThemeFontRoleBit) ? EditorThemeFontRole::Sans
                                                                                                       : EditorThemeFontRole::None;
    }

    /** @copydoc ResolveEditorThemeIconRole */
    EditorThemeIconRole ResolveEditorThemeIconRole(const EditorThemeIconRole requested, const EditorThemeFrame &frame) noexcept {
        if (!IsKnownRole(requested) || requested == EditorThemeIconRole::None)
            return EditorThemeIconRole::None;
        if (IsSupported(frame.supportedIconMask, requested, EditorThemeIconRoleBit))
            return requested;
        return IsSupported(frame.supportedIconMask, EditorThemeIconRole::Generic, EditorThemeIconRoleBit) ? EditorThemeIconRole::Generic
                                                                                                          : EditorThemeIconRole::None;
    }

    /** @copydoc ColorFor */
    EditorThemeColor ColorFor(const EditorThemeFrame &frame, const EditorThemeColorRole role) noexcept {
        using enum EditorThemeColorRole;
        switch (ResolveEditorThemeColorRole(role, frame)) {
            case Surface:
                return frame.colors.surface;
            case SurfaceSubtle:
                return frame.colors.surfaceSubtle;
            case TextPrimary:
                return frame.colors.textPrimary;
            case TextSecondary:
                return frame.colors.textSecondary;
            case TextDisabled:
                return frame.colors.textDisabled;
            case Border:
                return frame.colors.border;
            case Accent:
                return frame.colors.accent;
            case Focus:
                return frame.colors.focus;
            case Positive:
                return frame.colors.positive;
            case Warning:
                return frame.colors.warning;
            case Critical:
                return frame.colors.critical;
            case SurfaceRaised:
                return frame.colors.surfaceRaised;
            case SurfaceHover:
                return frame.colors.surfaceHover;
            case BorderStrong:
                return frame.colors.borderStrong;
            case AccentHover:
                return frame.colors.accentHover;
            case AccentActive:
                return frame.colors.accentActive;
            case OnAccent:
                return frame.colors.onAccent;
            case Overlay:
                return frame.colors.overlay;
            case None:
            case Count:
            default:
                return {};
        }
    }

    /** @copydoc TypographyFor */
    float TypographyFor(const EditorThemeFrame &frame, const EditorThemeTypographyRole role) noexcept {
        using enum EditorThemeTypographyRole;
        switch (ResolveEditorThemeTypographyRole(role, frame)) {
            case Caption:
                return frame.typography.caption;
            case Label:
                return frame.typography.label;
            case Body:
                return frame.typography.body;
            case CardTitle:
                return frame.typography.cardTitle;
            case Title:
                return frame.typography.title;
            case Heading:
                return frame.typography.heading;
            case Display:
                return frame.typography.display;
            case None:
            case Count:
            default:
                return 0.0F;
        }
    }

    /** @copydoc SpacingFor */
    float SpacingFor(const EditorThemeFrame &frame, const EditorThemeSpacingRole role) noexcept {
        using enum EditorThemeSpacingRole;
        switch (ResolveEditorThemeSpacingRole(role, frame)) {
            case XS:
                return frame.spacing.xs;
            case Small:
                return frame.spacing.small;
            case Medium:
                return frame.spacing.medium;
            case Large:
                return frame.spacing.large;
            case XL:
                return frame.spacing.xl;
            case CardPadding:
                return frame.spacing.cardPadding;
            case GridGap:
                return frame.spacing.gridGap;
            case BodyPaddingX:
                return frame.spacing.bodyPaddingX;
            case BodyPaddingY:
                return frame.spacing.bodyPaddingY;
            case SidebarPaddingX:
                return frame.spacing.sidebarPaddingX;
            case SidebarPaddingY:
                return frame.spacing.sidebarPaddingY;
            case PropertyRowGap:
                return frame.spacing.propertyRowGap;
            case ControlGap:
                return frame.spacing.controlGap;
            case None:
            case Count:
            default:
                return 0.0F;
        }
    }

    /** @copydoc SizeFor */
    float SizeFor(const EditorThemeFrame &frame, const EditorThemeSizeRole role) noexcept {
        using enum EditorThemeSizeRole;
        switch (ResolveEditorThemeSizeRole(role, frame)) {
            case SmallControlHeight:
                return frame.metrics.smallControlHeight;
            case MediumControlHeight:
                return frame.metrics.mediumControlHeight;
            case LargeControlHeight:
                return frame.metrics.largeControlHeight;
            case TextLineHeight:
                return frame.metrics.textLineHeight;
            case DefaultWidth:
                return frame.metrics.defaultWidth;
            case WelcomeSideWidth:
                return frame.metrics.welcomeSideWidth;
            case WelcomePadding:
                return frame.metrics.welcomePadding;
            case ModalWidth:
                return frame.metrics.modalWidth;
            case ModalHeight:
                return frame.metrics.modalHeight;
            case ModalHeaderHeight:
                return frame.metrics.modalHeaderHeight;
            case ModalFooterHeight:
                return frame.metrics.modalFooterHeight;
            case ModalSidebarWidth:
                return frame.metrics.modalSidebarWidth;
            case SettingsWidth:
                return frame.metrics.settingsWidth;
            case SettingsHeight:
                return frame.metrics.settingsHeight;
            case IconSmall:
                return frame.metrics.iconSmall;
            case IconMedium:
                return frame.metrics.iconMedium;
            case IconLarge:
                return frame.metrics.iconLarge;
            case MinimumInteractiveTarget:
                return frame.metrics.minimumInteractiveTarget;
            case None:
            case Count:
            default:
                return 0.0F;
        }
    }

    /** @copydoc RadiusFor */
    float RadiusFor(const EditorThemeFrame &frame, const EditorThemeRadiusRole role) noexcept {
        using enum EditorThemeRadiusRole;
        switch (ResolveEditorThemeRadiusRole(role, frame)) {
            case Control:
                return frame.radii.control;
            case Card:
                return frame.radii.card;
            case Modal:
                return frame.radii.modal;
            case Popup:
                return frame.radii.popup;
            case FocusRing:
                return frame.radii.focusRing;
            case None:
            case Count:
            default:
                return 0.0F;
        }
    }

    /** @copydoc MotionFor */
    float MotionFor(const EditorThemeFrame &frame, const EditorThemeMotionRole role) noexcept {
        using enum EditorThemeMotionRole;
        switch (ResolveEditorThemeMotionRole(role, frame)) {
            case Instant:
                return frame.motion.instantSeconds;
            case Fast:
                return frame.motion.fastSeconds;
            case Normal:
                return frame.motion.normalSeconds;
            case Slow:
                return frame.motion.slowSeconds;
            case Hover:
                return frame.motion.hoverSeconds;
            case Press:
                return frame.motion.pressSeconds;
            case Focus:
                return frame.motion.focusSeconds;
            case Modal:
                return frame.motion.modalSeconds;
            case None:
            case Count:
            default:
                return 0.0F;
        }
    }

    /** @copydoc FontSizeFor */
    float FontSizeFor(const EditorThemeFrame &frame, const EditorThemeFontRole role) noexcept {
        using enum EditorThemeFontRole;
        switch (ResolveEditorThemeFontRole(role, frame)) {
            case Sans:
                return frame.fonts.sansBase;
            case SansCompact:
                return frame.fonts.sansCompactBase;
            case SansEmphasis:
                return frame.fonts.sansEmphasisBase;
            case Monospace:
                return frame.fonts.monospaceBase;
            case Icon:
                return frame.fonts.iconBase;
            case None:
            case Count:
            default:
                return 0.0F;
        }
    }
}  // namespace Horo::Extensions
