#include "EditorThemeTokensInternal.h"
#include "Horo/Extensions/EditorThemeTokens.h"

#include <array>

namespace Horo::Extensions {
    namespace {
        [[nodiscard]] EditorThemeTypographyRole TypographyFallback(const EditorThemeTypographyRole requested,
                                                                   const EditorThemeFrame &frame) noexcept {
            using enum EditorThemeTypographyRole;
            const auto supports = [&frame](const EditorThemeTypographyRole role) {
                return ThemeTokenInternal::IsSupported(frame.supportedTypographyMask, role, EditorThemeTypographyRoleBit);
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
                return ThemeTokenInternal::IsSupported(frame.supportedSpacingMask, role, EditorThemeSpacingRoleBit);
            };
            const std::array candidates =
                requested == XS || requested == Small ? std::array{Small, Medium, Large, None} : std::array{Medium, Large, Small, None};
            for (const EditorThemeSpacingRole candidate : candidates) {
                if (supports(candidate))
                    return candidate;
            }
            return None;
        }

        [[nodiscard]] EditorThemeSizeRole SizeFallback(const EditorThemeSizeRole, const EditorThemeFrame &frame) noexcept {
            using enum EditorThemeSizeRole;
            const std::array candidates{MediumControlHeight, SmallControlHeight, LargeControlHeight, None};
            for (const EditorThemeSizeRole candidate : candidates) {
                if (ThemeTokenInternal::IsSupported(frame.supportedSizeMask, candidate, EditorThemeSizeRoleBit))
                    return candidate;
            }
            return None;
        }
    }  // namespace

    /** @copydoc ResolveEditorThemeTypographyRole */
    EditorThemeTypographyRole ResolveEditorThemeTypographyRole(const EditorThemeTypographyRole requested,
                                                               const EditorThemeFrame &frame) noexcept {
        if (!ThemeTokenInternal::IsKnownRole(requested, EditorThemeTypographyRole::Count) || requested == EditorThemeTypographyRole::None)
            return EditorThemeTypographyRole::None;
        if (ThemeTokenInternal::IsSupported(frame.supportedTypographyMask, requested, EditorThemeTypographyRoleBit))
            return requested;
        return TypographyFallback(requested, frame);
    }

    /** @copydoc ResolveEditorThemeSpacingRole */
    EditorThemeSpacingRole ResolveEditorThemeSpacingRole(const EditorThemeSpacingRole requested, const EditorThemeFrame &frame) noexcept {
        if (!ThemeTokenInternal::IsKnownRole(requested, EditorThemeSpacingRole::Count) || requested == EditorThemeSpacingRole::None)
            return EditorThemeSpacingRole::None;
        if (ThemeTokenInternal::IsSupported(frame.supportedSpacingMask, requested, EditorThemeSpacingRoleBit))
            return requested;
        return SpacingFallback(requested, frame);
    }

    /** @copydoc ResolveEditorThemeSizeRole */
    EditorThemeSizeRole ResolveEditorThemeSizeRole(const EditorThemeSizeRole requested, const EditorThemeFrame &frame) noexcept {
        if (!ThemeTokenInternal::IsKnownRole(requested, EditorThemeSizeRole::Count) || requested == EditorThemeSizeRole::None)
            return EditorThemeSizeRole::None;
        if (ThemeTokenInternal::IsSupported(frame.supportedSizeMask, requested, EditorThemeSizeRoleBit))
            return requested;
        return SizeFallback(requested, frame);
    }

    /** @copydoc ResolveEditorThemeRadiusRole */
    EditorThemeRadiusRole ResolveEditorThemeRadiusRole(const EditorThemeRadiusRole requested, const EditorThemeFrame &frame) noexcept {
        if (!ThemeTokenInternal::IsKnownRole(requested, EditorThemeRadiusRole::Count) || requested == EditorThemeRadiusRole::None)
            return EditorThemeRadiusRole::None;
        if (ThemeTokenInternal::IsSupported(frame.supportedRadiusMask, requested, EditorThemeRadiusRoleBit))
            return requested;
        return ThemeTokenInternal::IsSupported(frame.supportedRadiusMask, EditorThemeRadiusRole::Control, EditorThemeRadiusRoleBit)
                   ? EditorThemeRadiusRole::Control
                   : EditorThemeRadiusRole::None;
    }

    /** @copydoc ResolveEditorThemeInteractionRole */
    EditorThemeInteractionRole ResolveEditorThemeInteractionRole(const EditorThemeInteractionRole requested,
                                                                 const EditorThemeFrame &frame) noexcept {
        if (!ThemeTokenInternal::IsKnownRole(requested, EditorThemeInteractionRole::Count) || requested == EditorThemeInteractionRole::None)
            return EditorThemeInteractionRole::None;
        if (ThemeTokenInternal::IsSupported(frame.supportedInteractionMask, requested, EditorThemeInteractionRoleBit))
            return requested;
        return ThemeTokenInternal::IsSupported(frame.supportedInteractionMask, EditorThemeInteractionRole::Default,
                                               EditorThemeInteractionRoleBit)
                   ? EditorThemeInteractionRole::Default
                   : EditorThemeInteractionRole::None;
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
            case RowGap:
                return frame.metrics.rowGap;
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
}  // namespace Horo::Extensions
