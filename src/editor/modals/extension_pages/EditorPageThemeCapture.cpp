#include "EditorPageThemeCapture.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <imgui.h>
#include <limits>

namespace Horo::Editor {
    namespace {
        using namespace Extensions;

        /** @brief Copies actual normalized GUI color values without publishing toolkit types. */
        EditorThemeColor Color(ImVec4 value) noexcept {
            return {value.x, value.y, value.z, value.w};
        }

        /** @brief Copies every shared palette role; modal overlay comes from the actual native style, not a second palette. */
        EditorThemeColorTokens Colors(const DesignSystem::ColorTokens &c, const ImGuiStyle &style) {
            return {Color(c.surfaceRoot),
                    Color(c.surfaceWindow),
                    Color(c.textPrimary),
                    Color(c.textMuted),
                    Color(c.textDim),
                    Color(c.border),
                    Color(c.actionPrimary),
                    Color(c.actionPrimaryHover),
                    Color(c.statusOk),
                    Color(c.statusWarn),
                    Color(c.statusError),
                    Color(c.surfaceRaised),
                    Color(c.surfaceHover),
                    Color(c.borderStrong),
                    Color(c.actionPrimaryHover),
                    Color(c.actionPrimaryActive),
                    Color(c.textOnActionPrimary),
                    Color(style.Colors[ImGuiCol_ModalWindowDimBg])};
        }

        /** @brief Converts already-scaled real component geometry into the form contract's logical values, exactly once. */
        void Geometry(EditorThemeFrame &frame, const DesignSystem::DesignTokens &t) {
            const float s = t.sizes.uiScale;
            const auto &small = t.components.sizes[1];
            const auto &medium = t.components.sizes[2];
            const auto &large = t.components.sizes[3];
            frame.uiScale = s;
            const auto &p = t.typography;
            frame.typography = {p.caption / s, p.label / s, p.body / s, p.cardTitle / s, p.title / s, p.heading / s, p.display / s};
            const auto &sp = t.components.spacing;
            const auto &g = t.spacing;
            frame.spacing = {sp[1] / s,
                             sp[2] / s,
                             sp[3] / s,
                             sp[4] / s,
                             sp[5] / s,
                             g.cardPadding / s,
                             g.gridGap / s,
                             g.bodyPaddingX / s,
                             g.bodyPaddingY / s,
                             g.sidebarPaddingX / s,
                             g.sidebarPaddingY / s,
                             g.propertyRowGap / s,
                             sp[2] / s};
            const auto &z = t.sizes;
            frame.metrics = {small.minimumHeight / s, medium.minimumHeight / s, large.minimumHeight / s, p.body / s,
                             g.propertyRowGap / s,    z.settingsWidth / s,      z.welcomeSideWidth / s,  z.welcomePadding / s,
                             z.modalWidth / s,        z.modalHeight / s,        z.modalHeaderHeight / s, z.modalFooterHeight / s,
                             z.modalSidebarWidth / s, z.settingsWidth / s,      z.settingsHeight / s,    small.iconSize / s,
                             medium.iconSize / s,     large.iconSize / s,       small.minimumHeight / s};
            frame.radii = {t.radii.control / s, t.radii.card / s, t.radii.modal / s, t.radii.card / s, t.radii.control / s};
            frame.icons = {small.iconSize / s, medium.iconSize / s, large.iconSize / s, 0};
            frame.fonts = {EditorThemeFontRole::Sans,
                           EditorThemeFontRole::SansCompact,
                           EditorThemeFontRole::SansEmphasis,
                           EditorThemeFontRole::SansCompact,
                           EditorThemeFontRole::Icon,
                           p.sansBase / s,
                           p.sansCompactBase / s,
                           p.sansEmphasisBase / s,
                           p.sansCompactBase / s,
                           Theme::FontPx::Icon};
        }

        /** @brief Hashes actual scalar geometry and style inputs in one fixed order, without padding or native resources. */
        void HashGeometry(Sha256Builder &hash, const DesignSystem::DesignTokens &t, const ImGuiStyle &style) {
            const auto &p = t.typography;
            const auto &g = t.spacing;
            const auto &z = t.sizes;
            const std::array numbers{p.sansBase,
                                     p.sansCompactBase,
                                     p.sansEmphasisBase,
                                     p.caption,
                                     p.label,
                                     p.body,
                                     p.cardTitle,
                                     p.title,
                                     p.heading,
                                     p.display,
                                     t.radii.control,
                                     t.radii.card,
                                     t.radii.modal,
                                     z.welcomeSideWidth,
                                     z.welcomePadding,
                                     z.modalWidth,
                                     z.modalHeight,
                                     z.modalHeaderHeight,
                                     z.modalFooterHeight,
                                     z.modalSidebarWidth,
                                     z.settingsWidth,
                                     z.settingsHeight,
                                     z.uiScale,
                                     g.cardPadding,
                                     g.gridGap,
                                     g.bodyPaddingX,
                                     g.bodyPaddingY,
                                     g.sidebarPaddingX,
                                     g.sidebarPaddingY,
                                     g.propertyRowGap,
                                     style.DisabledAlpha,
                                     style.FrameBorderSize};
            static_cast<void>(hash.Update(std::as_bytes(std::span{numbers})));
        }

        /** @brief Captures a local structural signature over actual owner inputs only; never hashes struct padding or persists native
         * pointers. */
        Sha256Digest Signature(const DesignSystem::DesignTokens &t, const ImGuiStyle &style) {
            Sha256Builder hash;
            const auto color = [&](ImVec4 value) {
                const std::array values{value.x, value.y, value.z, value.w};
                static_cast<void>(hash.Update(std::as_bytes(std::span{values})));
            };
            const auto &c = t.colors;
            for (const auto value :
                 {c.surfaceRoot, c.surfaceWindow, c.surfacePanel, c.surfaceRaised, c.surfaceHover, c.border, c.borderStrong, c.textPrimary,
                  c.textMuted, c.textDim, c.actionPrimary, c.actionPrimaryHover, c.actionPrimaryActive, c.actionPrimarySoft, c.statusOk,
                  c.statusWarn, c.statusError, c.textOnActionPrimary})
                color(value);
            color(style.Colors[ImGuiCol_ModalWindowDimBg]);
            HashGeometry(hash, t, style);
            for (const auto &size : t.components.sizes) {
                const std::array values{size.fontSize, size.paddingX, size.paddingY, size.minimumHeight, size.iconSize};
                static_cast<void>(hash.Update(std::as_bytes(std::span{values})));
            }
            static_cast<void>(hash.Update(std::as_bytes(std::span{t.components.spacing})));
            return hash.Finalize();
        }
    }  // namespace

    /** @copydoc EditorPageThemeCapture::EditorPageThemeCapture */
    EditorPageThemeCapture::EditorPageThemeCapture() : frame_(std::make_shared<Extensions::EditorThemeFrame>()) {
        frame_->revision = 0;
    }

    /** @copydoc EditorPageThemeCapture::Frame */
    std::shared_ptr<const Extensions::EditorThemeFrame> EditorPageThemeCapture::Frame() const noexcept {
        return frame_;
    }

    /** @copydoc EditorPageThemeCapture::Update */
    Result<void> EditorPageThemeCapture::Update(const EditorGuiContext &context) {
        if (exhausted_ || !ImGui::GetCurrentContext())
            return Result<void>::Failure(MakeError(Extensions::ExtensionErrors::EditorThemeTokenInvalid));
        if (!context.theme.fonts.sans || !context.theme.fonts.sansCompact || !context.theme.fonts.sansEmphasis ||
            !context.theme.fonts.icon) {
            frame_->revision = 0;
            return Result<void>::Failure(MakeError(Extensions::ExtensionErrors::EditorThemeTokenInvalid));
        }
        const auto &tokens = Theme::GetActiveTokens();
        const auto &style = ImGui::GetStyle();
        const auto digest = Signature(tokens, style);
        const auto &f = context.theme.fonts;
        const std::array<const ImFont *, 4> fonts{f.sans, f.sansCompact, f.sansEmphasis, f.icon};
        if (signature_ == digest && fonts == fonts_ && frame_->revision != 0)
            return Result<void>::Success();
        if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
            exhausted_ = true;
            frame_->revision = 0;
            return Result<void>::Failure(MakeError(Extensions::ExtensionErrors::EditorThemeTokenInvalid));
        }
        EditorThemeFrame candidate;
        candidate.revision = revision_ + 1;
        candidate.colors = Colors(tokens.colors, style);
        Geometry(candidate, tokens);
        // Current shared controls publish no motion/accessibility override authority; unsupported motion/font roles remain explicit.
        candidate.supportedMotionMask = 0;
        candidate.motion = {0, 0, 0, 0, 0, 0, 0, 0};
        candidate.supportedFontMask &= ~EditorThemeFontRoleBit(EditorThemeFontRole::Monospace);
        candidate.interaction = {0, 0, style.DisabledAlpha, style.FrameBorderSize / tokens.sizes.uiScale, 0, 1, true};
        candidate.accessibility = {false, false, false, 1, EditorThemeColorVisionMode::None, 1};
        if (auto valid = ValidateEditorThemeFrame(candidate); valid.HasError()) {
            frame_->revision = 0;
            return valid;
        }
        *frame_ = candidate;
        revision_ = candidate.revision;
        signature_ = digest;
        fonts_ = fonts;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
