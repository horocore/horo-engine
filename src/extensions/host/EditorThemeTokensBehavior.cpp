#include "EditorThemeTokensInternal.h"
#include "Horo/Extensions/EditorThemeTokens.h"

namespace Horo::Extensions {
    /** @copydoc ResolveEditorThemeMotionRole */
    EditorThemeMotionRole ResolveEditorThemeMotionRole(const EditorThemeMotionRole requested, const EditorThemeFrame &frame) noexcept {
        using enum EditorThemeMotionRole;
        if (!ThemeTokenInternal::IsKnownRole(requested, Count) || requested == None)
            return None;
        if (frame.accessibility.reduceMotion)
            return ThemeTokenInternal::IsSupported(frame.supportedMotionMask, Instant, EditorThemeMotionRoleBit) ? Instant : None;
        if (ThemeTokenInternal::IsSupported(frame.supportedMotionMask, requested, EditorThemeMotionRoleBit))
            return requested;
        if (ThemeTokenInternal::IsSupported(frame.supportedMotionMask, Normal, EditorThemeMotionRoleBit))
            return Normal;
        if (ThemeTokenInternal::IsSupported(frame.supportedMotionMask, Fast, EditorThemeMotionRoleBit))
            return Fast;
        return ThemeTokenInternal::IsSupported(frame.supportedMotionMask, Instant, EditorThemeMotionRoleBit) ? Instant : None;
    }

    /** @copydoc ResolveEditorThemeFontRole */
    EditorThemeFontRole ResolveEditorThemeFontRole(const EditorThemeFontRole requested, const EditorThemeFrame &frame) noexcept {
        using enum EditorThemeFontRole;
        if (!ThemeTokenInternal::IsKnownRole(requested, Count) || requested == None)
            return None;
        if (ThemeTokenInternal::IsSupported(frame.supportedFontMask, requested, EditorThemeFontRoleBit))
            return requested;
        return ThemeTokenInternal::IsSupported(frame.supportedFontMask, Sans, EditorThemeFontRoleBit) ? Sans : None;
    }

    /** @copydoc ResolveEditorThemeIconRole */
    EditorThemeIconRole ResolveEditorThemeIconRole(const EditorThemeIconRole requested, const EditorThemeFrame &frame) noexcept {
        using enum EditorThemeIconRole;
        if (!ThemeTokenInternal::IsKnownRole(requested, Count) || requested == None)
            return None;
        if (ThemeTokenInternal::IsSupported(frame.supportedIconMask, requested, EditorThemeIconRoleBit))
            return requested;
        return ThemeTokenInternal::IsSupported(frame.supportedIconMask, Generic, EditorThemeIconRoleBit) ? Generic : None;
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
