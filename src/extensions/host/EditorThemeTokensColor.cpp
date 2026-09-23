#include "EditorThemeTokensInternal.h"
#include "Horo/Extensions/EditorThemeTokens.h"

#include <array>

namespace Horo::Extensions {
    namespace {
        [[nodiscard]] EditorThemeColorRole FirstSupportedColor(const EditorThemeFrame &frame,
                                                               const std::array<EditorThemeColorRole, 4> candidates) noexcept {
            for (const EditorThemeColorRole candidate : candidates) {
                if (ThemeTokenInternal::IsSupported(frame.supportedTokenMask, candidate, EditorThemeColorRoleBit))
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
                case Accent:
                    return FirstSupportedColor(frame, {Accent, TextPrimary, None, None});
                case AccentHover:
                case AccentActive:
                case OnAccent:
                    return FirstSupportedColor(frame, {Accent, TextPrimary, None, None});
                case TextSecondary:
                    return FirstSupportedColor(frame, {TextPrimary, None, None, None});
                case Overlay:
                    return FirstSupportedColor(frame, {SurfaceSubtle, Surface, None, None});
                case Surface:
                case TextPrimary:
                case None:
                case Count:
                default:
                    return None;
            }
        }
    }  // namespace

    /** @copydoc ResolveEditorThemeColorRole */
    EditorThemeColorRole ResolveEditorThemeColorRole(const EditorThemeColorRole requested, const EditorThemeFrame &frame) noexcept {
        using enum EditorThemeColorRole;
        if (!ThemeTokenInternal::IsKnownRole(requested, Count) || requested == None)
            return None;
        if (ThemeTokenInternal::IsSupported(frame.supportedTokenMask, requested, EditorThemeColorRoleBit))
            return requested;
        return ColorFallback(requested, frame);
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
}  // namespace Horo::Extensions
