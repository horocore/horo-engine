#include "EditorThemeTokensInternal.h"
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
                if (!ThemeTokenInternal::IsKnownRole(role, EditorThemeFontRole::Count) || role == EditorThemeFontRole::None)
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
            return ThemeTokenInternal::IsKnownRole(accessibility.colorVision, EditorThemeColorVisionMode::Count) &&
                   IsFiniteInRange(accessibility.textContrastMultiplier, 1.0F, 4.0F) &&
                   IsFiniteInRange(accessibility.colorVisionSeverity, 0.0F, 1.0F);
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
            !ThemeTokenInternal::IsKnownMask(frame.supportedTokenMask, EditorThemeColorRole::Count, EditorThemeColorRoleBit) ||
            !ThemeTokenInternal::IsKnownMask(frame.supportedTypographyMask, EditorThemeTypographyRole::Count,
                                             EditorThemeTypographyRoleBit) ||
            !ThemeTokenInternal::IsKnownMask(frame.supportedSpacingMask, EditorThemeSpacingRole::Count, EditorThemeSpacingRoleBit) ||
            !ThemeTokenInternal::IsKnownMask(frame.supportedSizeMask, EditorThemeSizeRole::Count, EditorThemeSizeRoleBit) ||
            !ThemeTokenInternal::IsKnownMask(frame.supportedRadiusMask, EditorThemeRadiusRole::Count, EditorThemeRadiusRoleBit) ||
            !ThemeTokenInternal::IsKnownMask(frame.supportedInteractionMask, EditorThemeInteractionRole::Count,
                                             EditorThemeInteractionRoleBit) ||
            !ThemeTokenInternal::IsKnownMask(frame.supportedMotionMask, EditorThemeMotionRole::Count, EditorThemeMotionRoleBit) ||
            !ThemeTokenInternal::IsKnownMask(frame.supportedFontMask, EditorThemeFontRole::Count, EditorThemeFontRoleBit) ||
            !ThemeTokenInternal::IsKnownMask(frame.supportedIconMask, EditorThemeIconRole::Count, EditorThemeIconRoleBit))
            return InvalidTheme("Theme token frame metadata is outside the supported contract.");
        if (!ValidateColors(frame.colors) || !ValidateTypography(frame.typography, limits.minimumReadableTextSize) ||
            !ValidateSpacing(frame.spacing) || !ValidateSizes(frame.metrics) || !ValidateRadii(frame.radii) ||
            !ValidateInteraction(frame.interaction) || !ValidateMotion(frame.motion, limits.maximumMotionSeconds) ||
            !ValidateFonts(frame.fonts) || !ValidateIcons(frame.icons) || !ValidateAccessibility(frame.accessibility))
            return InvalidTheme("Theme token frame contains a non-finite, unordered, or out-of-range resolved value.");
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions
