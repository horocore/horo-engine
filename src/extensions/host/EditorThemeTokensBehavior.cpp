#include "EditorThemeTokensInternal.h"
#include "Horo/Extensions/EditorThemeTokens.h"

namespace Horo::Extensions {
    /** @copydoc ResolveEditorThemeMotionRole */
    EditorThemeMotionRole ResolveEditorThemeMotionRole(const EditorThemeMotionRole requested, const EditorThemeFrame &frame) noexcept {
        if (!ThemeTokenInternal::IsKnownRole(requested, EditorThemeMotionRole::Count) || requested == EditorThemeMotionRole::None)
            return EditorThemeMotionRole::None;
        if (frame.accessibility.reduceMotion || frame.motion.reduceMotion)
            return ThemeTokenInternal::IsSupported(frame.supportedMotionMask, EditorThemeMotionRole::Instant, EditorThemeMotionRoleBit)
                       ? EditorThemeMotionRole::Instant
                       : EditorThemeMotionRole::None;
        if (ThemeTokenInternal::IsSupported(frame.supportedMotionMask, requested, EditorThemeMotionRoleBit))
            return requested;
        if (ThemeTokenInternal::IsSupported(frame.supportedMotionMask, EditorThemeMotionRole::Normal, EditorThemeMotionRoleBit))
            return EditorThemeMotionRole::Normal;
        if (ThemeTokenInternal::IsSupported(frame.supportedMotionMask, EditorThemeMotionRole::Fast, EditorThemeMotionRoleBit))
            return EditorThemeMotionRole::Fast;
        return ThemeTokenInternal::IsSupported(frame.supportedMotionMask, EditorThemeMotionRole::Instant, EditorThemeMotionRoleBit)
                   ? EditorThemeMotionRole::Instant
                   : EditorThemeMotionRole::None;
    }

    /** @copydoc ResolveEditorThemeFontRole */
    EditorThemeFontRole ResolveEditorThemeFontRole(const EditorThemeFontRole requested, const EditorThemeFrame &frame) noexcept {
        if (!ThemeTokenInternal::IsKnownRole(requested, EditorThemeFontRole::Count) || requested == EditorThemeFontRole::None)
            return EditorThemeFontRole::None;
        if (ThemeTokenInternal::IsSupported(frame.supportedFontMask, requested, EditorThemeFontRoleBit))
            return requested;
        return ThemeTokenInternal::IsSupported(frame.supportedFontMask, EditorThemeFontRole::Sans, EditorThemeFontRoleBit)
                   ? EditorThemeFontRole::Sans
                   : EditorThemeFontRole::None;
    }

    /** @copydoc ResolveEditorThemeIconRole */
    EditorThemeIconRole ResolveEditorThemeIconRole(const EditorThemeIconRole requested, const EditorThemeFrame &frame) noexcept {
        if (!ThemeTokenInternal::IsKnownRole(requested, EditorThemeIconRole::Count) || requested == EditorThemeIconRole::None)
            return EditorThemeIconRole::None;
        if (ThemeTokenInternal::IsSupported(frame.supportedIconMask, requested, EditorThemeIconRoleBit))
            return requested;
        return ThemeTokenInternal::IsSupported(frame.supportedIconMask, EditorThemeIconRole::Generic, EditorThemeIconRoleBit)
                   ? EditorThemeIconRole::Generic
                   : EditorThemeIconRole::None;
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
