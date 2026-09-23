#include "Horo/Extensions/EditorThemeTokens.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

namespace Horo::Extensions::Tests {
    namespace {
        [[nodiscard]] std::uint32_t Changes(const EditorThemeChange first, const EditorThemeChange second) noexcept {
            return static_cast<std::uint32_t>(first) | static_cast<std::uint32_t>(second);
        }

        template <typename ResultType> void RequireErrorCode(const ResultType &result, const char *const code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }

        template <typename Mutator> void RequireInvalidThemeFrame(const Mutator &mutate) {
            EditorThemeFrame frame;
            mutate(frame);
            RequireErrorCode(ValidateEditorThemeFrame(frame), "editor_theme_token_invalid");
        }
    }  // namespace

    TEST_CASE("Theme token frame validates as one complete immutable contract", "[Extensions][EditorThemeTokens]") {
        const EditorThemeFrame frame;
        const auto validation = ValidateEditorThemeFrame(frame);

        REQUIRE(validation.HasValue());
        CHECK(frame.schemaVersion == EditorThemeTokenSchemaVersion);
        CHECK(frame.supportedTokenMask == EditorThemeAllColorRoleMask());
        CHECK(frame.supportedTypographyMask == EditorThemeAllTypographyRoleMask());
        CHECK(frame.supportedSpacingMask == EditorThemeAllSpacingRoleMask());
        CHECK(frame.supportedSizeMask == EditorThemeAllSizeRoleMask());
        CHECK(frame.supportedRadiusMask == EditorThemeAllRadiusRoleMask());
        CHECK(frame.supportedInteractionMask == EditorThemeAllInteractionRoleMask());
        CHECK(frame.supportedMotionMask == EditorThemeAllMotionRoleMask());
        CHECK(frame.supportedFontMask == EditorThemeAllFontRoleMask());
        CHECK(frame.supportedIconMask == EditorThemeAllIconRoleMask());
        CHECK(EditorThemeColorRoleBit(EditorThemeColorRole::None) == 0U);
        CHECK(EditorThemeColorRoleBit(EditorThemeColorRole::Accent) != 0U);
        CHECK(EditorThemeChangeBit(EditorThemeChange::Dpi) != 0U);
        CHECK((frame.changeMask & EditorThemeAllChangeMask()) == EditorThemeAllChangeMask());
    }

    TEST_CASE("Theme token accessors resolve every semantic category without raw literals", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame frame;
        frame.colors.accent = {0.1F, 0.2F, 0.3F, 1.0F};

        CHECK(ColorFor(frame, EditorThemeColorRole::Accent).red == Catch::Approx(0.1F));
        CHECK(TypographyFor(frame, EditorThemeTypographyRole::Heading) == Catch::Approx(22.0F));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::CardPadding) == Catch::Approx(18.0F));
        CHECK(SizeFor(frame, EditorThemeSizeRole::RowGap) == Catch::Approx(8.0F));
        CHECK(SizeFor(frame, EditorThemeSizeRole::MinimumInteractiveTarget) == Catch::Approx(24.0F));
        CHECK(RadiusFor(frame, EditorThemeRadiusRole::FocusRing) == Catch::Approx(2.0F));
        CHECK(MotionFor(frame, EditorThemeMotionRole::Modal) == Catch::Approx(0.22F));
        CHECK(FontSizeFor(frame, EditorThemeFontRole::Monospace) == Catch::Approx(14.0F));

        frame.supportedTokenMask =
            EditorThemeColorRoleBit(EditorThemeColorRole::Surface) | EditorThemeColorRoleBit(EditorThemeColorRole::TextPrimary);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::SurfaceSubtle, frame) == EditorThemeColorRole::Surface);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Critical, frame) == EditorThemeColorRole::TextPrimary);
        CHECK(ColorFor(frame, EditorThemeColorRole::Critical).red == Catch::Approx(frame.colors.textPrimary.red));

        frame.supportedTypographyMask = EditorThemeTypographyRoleBit(EditorThemeTypographyRole::Body);
        CHECK(ResolveEditorThemeTypographyRole(EditorThemeTypographyRole::Display, frame) == EditorThemeTypographyRole::Body);
        frame.supportedSpacingMask = EditorThemeSpacingRoleBit(EditorThemeSpacingRole::Medium);
        CHECK(ResolveEditorThemeSpacingRole(EditorThemeSpacingRole::XL, frame) == EditorThemeSpacingRole::Medium);
        frame.supportedSizeMask = EditorThemeSizeRoleBit(EditorThemeSizeRole::MediumControlHeight);
        CHECK(ResolveEditorThemeSizeRole(EditorThemeSizeRole::ModalWidth, frame) == EditorThemeSizeRole::MediumControlHeight);
        frame.supportedRadiusMask = EditorThemeRadiusRoleBit(EditorThemeRadiusRole::Control);
        CHECK(ResolveEditorThemeRadiusRole(EditorThemeRadiusRole::Modal, frame) == EditorThemeRadiusRole::Control);
        frame.supportedInteractionMask = EditorThemeInteractionRoleBit(EditorThemeInteractionRole::Default);
        CHECK(ResolveEditorThemeInteractionRole(EditorThemeInteractionRole::Selected, frame) == EditorThemeInteractionRole::Default);
        frame.supportedFontMask = EditorThemeFontRoleBit(EditorThemeFontRole::Sans);
        CHECK(ResolveEditorThemeFontRole(EditorThemeFontRole::Monospace, frame) == EditorThemeFontRole::Sans);
        frame.supportedIconMask = EditorThemeIconRoleBit(EditorThemeIconRole::Generic);
        CHECK(ResolveEditorThemeIconRole(EditorThemeIconRole::Delete, frame) == EditorThemeIconRole::Generic);
    }

    TEST_CASE("Theme token color accessors map every supported role", "[Extensions][EditorThemeTokens]") {
        const EditorThemeFrame frame;

        CHECK(ColorFor(frame, EditorThemeColorRole::Surface).red == Catch::Approx(frame.colors.surface.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::SurfaceSubtle).red == Catch::Approx(frame.colors.surfaceSubtle.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::TextPrimary).red == Catch::Approx(frame.colors.textPrimary.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::TextSecondary).red == Catch::Approx(frame.colors.textSecondary.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::TextDisabled).red == Catch::Approx(frame.colors.textDisabled.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::Border).red == Catch::Approx(frame.colors.border.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::Accent).red == Catch::Approx(frame.colors.accent.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::Focus).red == Catch::Approx(frame.colors.focus.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::Positive).red == Catch::Approx(frame.colors.positive.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::Warning).red == Catch::Approx(frame.colors.warning.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::Critical).red == Catch::Approx(frame.colors.critical.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::SurfaceRaised).red == Catch::Approx(frame.colors.surfaceRaised.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::SurfaceHover).red == Catch::Approx(frame.colors.surfaceHover.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::BorderStrong).red == Catch::Approx(frame.colors.borderStrong.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::AccentHover).red == Catch::Approx(frame.colors.accentHover.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::AccentActive).red == Catch::Approx(frame.colors.accentActive.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::OnAccent).red == Catch::Approx(frame.colors.onAccent.red));
        CHECK(ColorFor(frame, EditorThemeColorRole::Overlay).red == Catch::Approx(frame.colors.overlay.red));
    }

    TEST_CASE("Theme token typography accessors map every supported role", "[Extensions][EditorThemeTokens]") {
        const EditorThemeFrame frame;
        CHECK(TypographyFor(frame, EditorThemeTypographyRole::Caption) == Catch::Approx(frame.typography.caption));
        CHECK(TypographyFor(frame, EditorThemeTypographyRole::Label) == Catch::Approx(frame.typography.label));
        CHECK(TypographyFor(frame, EditorThemeTypographyRole::Body) == Catch::Approx(frame.typography.body));
        CHECK(TypographyFor(frame, EditorThemeTypographyRole::CardTitle) == Catch::Approx(frame.typography.cardTitle));
        CHECK(TypographyFor(frame, EditorThemeTypographyRole::Title) == Catch::Approx(frame.typography.title));
        CHECK(TypographyFor(frame, EditorThemeTypographyRole::Heading) == Catch::Approx(frame.typography.heading));
        CHECK(TypographyFor(frame, EditorThemeTypographyRole::Display) == Catch::Approx(frame.typography.display));
    }

    TEST_CASE("Theme token spacing accessors map every supported role", "[Extensions][EditorThemeTokens]") {
        const EditorThemeFrame frame;
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::XS) == Catch::Approx(frame.spacing.xs));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::Small) == Catch::Approx(frame.spacing.small));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::Medium) == Catch::Approx(frame.spacing.medium));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::Large) == Catch::Approx(frame.spacing.large));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::XL) == Catch::Approx(frame.spacing.xl));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::CardPadding) == Catch::Approx(frame.spacing.cardPadding));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::GridGap) == Catch::Approx(frame.spacing.gridGap));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::BodyPaddingX) == Catch::Approx(frame.spacing.bodyPaddingX));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::BodyPaddingY) == Catch::Approx(frame.spacing.bodyPaddingY));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::SidebarPaddingX) == Catch::Approx(frame.spacing.sidebarPaddingX));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::SidebarPaddingY) == Catch::Approx(frame.spacing.sidebarPaddingY));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::PropertyRowGap) == Catch::Approx(frame.spacing.propertyRowGap));
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::ControlGap) == Catch::Approx(frame.spacing.controlGap));
    }

    TEST_CASE("Theme token size accessors map every supported role", "[Extensions][EditorThemeTokens]") {
        const EditorThemeFrame frame;
        CHECK(SizeFor(frame, EditorThemeSizeRole::SmallControlHeight) == Catch::Approx(frame.metrics.smallControlHeight));
        CHECK(SizeFor(frame, EditorThemeSizeRole::MediumControlHeight) == Catch::Approx(frame.metrics.mediumControlHeight));
        CHECK(SizeFor(frame, EditorThemeSizeRole::LargeControlHeight) == Catch::Approx(frame.metrics.largeControlHeight));
        CHECK(SizeFor(frame, EditorThemeSizeRole::TextLineHeight) == Catch::Approx(frame.metrics.textLineHeight));
        CHECK(SizeFor(frame, EditorThemeSizeRole::RowGap) == Catch::Approx(frame.metrics.rowGap));
        CHECK(SizeFor(frame, EditorThemeSizeRole::DefaultWidth) == Catch::Approx(frame.metrics.defaultWidth));
        CHECK(SizeFor(frame, EditorThemeSizeRole::WelcomeSideWidth) == Catch::Approx(frame.metrics.welcomeSideWidth));
        CHECK(SizeFor(frame, EditorThemeSizeRole::WelcomePadding) == Catch::Approx(frame.metrics.welcomePadding));
        CHECK(SizeFor(frame, EditorThemeSizeRole::ModalWidth) == Catch::Approx(frame.metrics.modalWidth));
        CHECK(SizeFor(frame, EditorThemeSizeRole::ModalHeight) == Catch::Approx(frame.metrics.modalHeight));
        CHECK(SizeFor(frame, EditorThemeSizeRole::ModalHeaderHeight) == Catch::Approx(frame.metrics.modalHeaderHeight));
        CHECK(SizeFor(frame, EditorThemeSizeRole::ModalFooterHeight) == Catch::Approx(frame.metrics.modalFooterHeight));
        CHECK(SizeFor(frame, EditorThemeSizeRole::ModalSidebarWidth) == Catch::Approx(frame.metrics.modalSidebarWidth));
        CHECK(SizeFor(frame, EditorThemeSizeRole::SettingsWidth) == Catch::Approx(frame.metrics.settingsWidth));
        CHECK(SizeFor(frame, EditorThemeSizeRole::SettingsHeight) == Catch::Approx(frame.metrics.settingsHeight));
        CHECK(SizeFor(frame, EditorThemeSizeRole::IconSmall) == Catch::Approx(frame.metrics.iconSmall));
        CHECK(SizeFor(frame, EditorThemeSizeRole::IconMedium) == Catch::Approx(frame.metrics.iconMedium));
        CHECK(SizeFor(frame, EditorThemeSizeRole::IconLarge) == Catch::Approx(frame.metrics.iconLarge));
        CHECK(SizeFor(frame, EditorThemeSizeRole::MinimumInteractiveTarget) == Catch::Approx(frame.metrics.minimumInteractiveTarget));
    }

    TEST_CASE("Theme token radius and motion accessors map every supported role", "[Extensions][EditorThemeTokens]") {
        const EditorThemeFrame frame;
        CHECK(RadiusFor(frame, EditorThemeRadiusRole::Control) == Catch::Approx(frame.radii.control));
        CHECK(RadiusFor(frame, EditorThemeRadiusRole::Card) == Catch::Approx(frame.radii.card));
        CHECK(RadiusFor(frame, EditorThemeRadiusRole::Modal) == Catch::Approx(frame.radii.modal));
        CHECK(RadiusFor(frame, EditorThemeRadiusRole::Popup) == Catch::Approx(frame.radii.popup));
        CHECK(RadiusFor(frame, EditorThemeRadiusRole::FocusRing) == Catch::Approx(frame.radii.focusRing));

        CHECK(MotionFor(frame, EditorThemeMotionRole::Instant) == Catch::Approx(frame.motion.instantSeconds));
        CHECK(MotionFor(frame, EditorThemeMotionRole::Fast) == Catch::Approx(frame.motion.fastSeconds));
        CHECK(MotionFor(frame, EditorThemeMotionRole::Normal) == Catch::Approx(frame.motion.normalSeconds));
        CHECK(MotionFor(frame, EditorThemeMotionRole::Slow) == Catch::Approx(frame.motion.slowSeconds));
        CHECK(MotionFor(frame, EditorThemeMotionRole::Hover) == Catch::Approx(frame.motion.hoverSeconds));
        CHECK(MotionFor(frame, EditorThemeMotionRole::Press) == Catch::Approx(frame.motion.pressSeconds));
        CHECK(MotionFor(frame, EditorThemeMotionRole::Focus) == Catch::Approx(frame.motion.focusSeconds));
        CHECK(MotionFor(frame, EditorThemeMotionRole::Modal) == Catch::Approx(frame.motion.modalSeconds));
    }

    TEST_CASE("Theme token font accessors map every supported role", "[Extensions][EditorThemeTokens]") {
        const EditorThemeFrame frame;
        CHECK(FontSizeFor(frame, EditorThemeFontRole::Sans) == Catch::Approx(frame.fonts.sansBase));
        CHECK(FontSizeFor(frame, EditorThemeFontRole::SansCompact) == Catch::Approx(frame.fonts.sansCompactBase));
        CHECK(FontSizeFor(frame, EditorThemeFontRole::SansEmphasis) == Catch::Approx(frame.fonts.sansEmphasisBase));
        CHECK(FontSizeFor(frame, EditorThemeFontRole::Monospace) == Catch::Approx(frame.fonts.monospaceBase));
        CHECK(FontSizeFor(frame, EditorThemeFontRole::Icon) == Catch::Approx(frame.fonts.iconBase));
    }

    TEST_CASE("Theme token resolvers reject empty and sentinel roles", "[Extensions][EditorThemeTokens]") {
        const EditorThemeFrame frame;
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::None, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Count, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeTypographyRole(EditorThemeTypographyRole::None, frame) == EditorThemeTypographyRole::None);
        CHECK(ResolveEditorThemeTypographyRole(EditorThemeTypographyRole::Count, frame) == EditorThemeTypographyRole::None);
        CHECK(ResolveEditorThemeSpacingRole(EditorThemeSpacingRole::None, frame) == EditorThemeSpacingRole::None);
        CHECK(ResolveEditorThemeSpacingRole(EditorThemeSpacingRole::Count, frame) == EditorThemeSpacingRole::None);
        CHECK(ResolveEditorThemeSizeRole(EditorThemeSizeRole::None, frame) == EditorThemeSizeRole::None);
        CHECK(ResolveEditorThemeSizeRole(EditorThemeSizeRole::Count, frame) == EditorThemeSizeRole::None);
        CHECK(ResolveEditorThemeRadiusRole(EditorThemeRadiusRole::None, frame) == EditorThemeRadiusRole::None);
        CHECK(ResolveEditorThemeRadiusRole(EditorThemeRadiusRole::Count, frame) == EditorThemeRadiusRole::None);
        CHECK(ResolveEditorThemeInteractionRole(EditorThemeInteractionRole::None, frame) == EditorThemeInteractionRole::None);
        CHECK(ResolveEditorThemeInteractionRole(EditorThemeInteractionRole::Count, frame) == EditorThemeInteractionRole::None);
        CHECK(ResolveEditorThemeMotionRole(EditorThemeMotionRole::None, frame) == EditorThemeMotionRole::None);
        CHECK(ResolveEditorThemeMotionRole(EditorThemeMotionRole::Count, frame) == EditorThemeMotionRole::None);
        CHECK(ResolveEditorThemeFontRole(EditorThemeFontRole::None, frame) == EditorThemeFontRole::None);
        CHECK(ResolveEditorThemeFontRole(EditorThemeFontRole::Count, frame) == EditorThemeFontRole::None);
        CHECK(ResolveEditorThemeIconRole(EditorThemeIconRole::None, frame) == EditorThemeIconRole::None);
        CHECK(ResolveEditorThemeIconRole(EditorThemeIconRole::Count, frame) == EditorThemeIconRole::None);
    }

    TEST_CASE("Theme token color resolvers return none without supported roles", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame frame;
        frame.supportedTokenMask = 0U;
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::SurfaceSubtle, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::SurfaceRaised, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::SurfaceHover, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::TextDisabled, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Border, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::BorderStrong, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Focus, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Positive, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Warning, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Critical, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Accent, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::AccentHover, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::AccentActive, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::OnAccent, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::TextSecondary, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Overlay, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Surface, frame) == EditorThemeColorRole::None);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::TextPrimary, frame) == EditorThemeColorRole::None);
        CHECK(ColorFor(frame, EditorThemeColorRole::Surface).red == Catch::Approx(0.0F));
        CHECK(ColorFor(frame, EditorThemeColorRole::Count).red == Catch::Approx(0.0F));
    }

    TEST_CASE("Theme token color resolvers use deterministic fallbacks", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame frame;
        frame.supportedTokenMask =
            EditorThemeColorRoleBit(EditorThemeColorRole::Surface) | EditorThemeColorRoleBit(EditorThemeColorRole::TextPrimary);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::SurfaceSubtle, frame) == EditorThemeColorRole::Surface);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Critical, frame) == EditorThemeColorRole::TextPrimary);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::TextDisabled, frame) == EditorThemeColorRole::TextPrimary);
        CHECK(ResolveEditorThemeColorRole(EditorThemeColorRole::Overlay, frame) == EditorThemeColorRole::Surface);
    }

    TEST_CASE("Theme token typography resolvers use deterministic fallbacks", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame frame;
        frame.supportedTypographyMask = EditorThemeTypographyRoleBit(EditorThemeTypographyRole::CardTitle);
        CHECK(ResolveEditorThemeTypographyRole(EditorThemeTypographyRole::Caption, frame) == EditorThemeTypographyRole::CardTitle);
        frame.supportedTypographyMask = EditorThemeTypographyRoleBit(EditorThemeTypographyRole::Label);
        CHECK(ResolveEditorThemeTypographyRole(EditorThemeTypographyRole::Display, frame) == EditorThemeTypographyRole::Label);
        frame.supportedTypographyMask = 0U;
        CHECK(ResolveEditorThemeTypographyRole(EditorThemeTypographyRole::Display, frame) == EditorThemeTypographyRole::None);
    }

    TEST_CASE("Theme token spacing resolvers use deterministic fallbacks", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame frame;
        frame.supportedSpacingMask = EditorThemeSpacingRoleBit(EditorThemeSpacingRole::Large);
        CHECK(ResolveEditorThemeSpacingRole(EditorThemeSpacingRole::XS, frame) == EditorThemeSpacingRole::Large);
        frame.supportedSpacingMask = EditorThemeSpacingRoleBit(EditorThemeSpacingRole::Medium);
        CHECK(ResolveEditorThemeSpacingRole(EditorThemeSpacingRole::XL, frame) == EditorThemeSpacingRole::Medium);
        frame.supportedSpacingMask = 0U;
        CHECK(ResolveEditorThemeSpacingRole(EditorThemeSpacingRole::XL, frame) == EditorThemeSpacingRole::None);
    }

    TEST_CASE("Theme token size and radius resolvers use deterministic fallbacks", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame frame;
        frame.supportedSizeMask = EditorThemeSizeRoleBit(EditorThemeSizeRole::MediumControlHeight);
        CHECK(ResolveEditorThemeSizeRole(EditorThemeSizeRole::ModalWidth, frame) == EditorThemeSizeRole::MediumControlHeight);
        frame.supportedSizeMask = 0U;
        CHECK(ResolveEditorThemeSizeRole(EditorThemeSizeRole::ModalWidth, frame) == EditorThemeSizeRole::None);

        frame.supportedRadiusMask = EditorThemeRadiusRoleBit(EditorThemeRadiusRole::Control);
        CHECK(ResolveEditorThemeRadiusRole(EditorThemeRadiusRole::Modal, frame) == EditorThemeRadiusRole::Control);
        frame.supportedRadiusMask = 0U;
        CHECK(ResolveEditorThemeRadiusRole(EditorThemeRadiusRole::Modal, frame) == EditorThemeRadiusRole::None);
    }

    TEST_CASE("Theme token interaction resolver uses deterministic fallbacks", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame frame;
        frame.supportedInteractionMask = EditorThemeInteractionRoleBit(EditorThemeInteractionRole::Default);
        CHECK(ResolveEditorThemeInteractionRole(EditorThemeInteractionRole::Selected, frame) == EditorThemeInteractionRole::Default);
        frame.supportedInteractionMask = EditorThemeInteractionRoleBit(EditorThemeInteractionRole::Hover);
        CHECK(ResolveEditorThemeInteractionRole(EditorThemeInteractionRole::Hover, frame) == EditorThemeInteractionRole::Hover);
        frame.supportedInteractionMask = 0U;
        CHECK(ResolveEditorThemeInteractionRole(EditorThemeInteractionRole::Selected, frame) == EditorThemeInteractionRole::None);
    }

    TEST_CASE("Theme token motion resolver honors reduced motion and fallbacks", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame frame;
        frame.supportedMotionMask = EditorThemeMotionRoleBit(EditorThemeMotionRole::Instant);
        frame.accessibility.reduceMotion = true;
        CHECK(ResolveEditorThemeMotionRole(EditorThemeMotionRole::Modal, frame) == EditorThemeMotionRole::Instant);
        frame.supportedMotionMask = EditorThemeMotionRoleBit(EditorThemeMotionRole::Normal);
        CHECK(ResolveEditorThemeMotionRole(EditorThemeMotionRole::Modal, frame) == EditorThemeMotionRole::None);
        frame.accessibility.reduceMotion = false;
        CHECK(ResolveEditorThemeMotionRole(EditorThemeMotionRole::Modal, frame) == EditorThemeMotionRole::Normal);
        frame.supportedMotionMask = EditorThemeMotionRoleBit(EditorThemeMotionRole::Fast);
        CHECK(ResolveEditorThemeMotionRole(EditorThemeMotionRole::Modal, frame) == EditorThemeMotionRole::Fast);
        frame.supportedMotionMask = EditorThemeMotionRoleBit(EditorThemeMotionRole::Instant);
        CHECK(ResolveEditorThemeMotionRole(EditorThemeMotionRole::Modal, frame) == EditorThemeMotionRole::Instant);
        frame.supportedMotionMask = 0U;
        CHECK(ResolveEditorThemeMotionRole(EditorThemeMotionRole::Modal, frame) == EditorThemeMotionRole::None);
        CHECK(MotionFor(frame, EditorThemeMotionRole::Modal) == Catch::Approx(0.0F));
    }

    TEST_CASE("Theme token font and icon resolvers use deterministic fallbacks", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame frame;
        frame.supportedFontMask = EditorThemeFontRoleBit(EditorThemeFontRole::Sans);
        CHECK(ResolveEditorThemeFontRole(EditorThemeFontRole::Monospace, frame) == EditorThemeFontRole::Sans);
        frame.supportedFontMask = 0U;
        CHECK(ResolveEditorThemeFontRole(EditorThemeFontRole::Monospace, frame) == EditorThemeFontRole::None);

        frame.supportedIconMask = EditorThemeIconRoleBit(EditorThemeIconRole::Generic);
        CHECK(ResolveEditorThemeIconRole(EditorThemeIconRole::Generic, frame) == EditorThemeIconRole::Generic);
        CHECK(ResolveEditorThemeIconRole(EditorThemeIconRole::Delete, frame) == EditorThemeIconRole::Generic);
        frame.supportedIconMask = 0U;
        CHECK(ResolveEditorThemeIconRole(EditorThemeIconRole::Delete, frame) == EditorThemeIconRole::None);
    }

    TEST_CASE("Theme token accessors return zero for unsupported roles", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame frame;
        frame.supportedTypographyMask = 0U;
        CHECK(TypographyFor(frame, EditorThemeTypographyRole::Display) == Catch::Approx(0.0F));
        frame.supportedSpacingMask = 0U;
        CHECK(SpacingFor(frame, EditorThemeSpacingRole::XL) == Catch::Approx(0.0F));
        frame.supportedSizeMask = 0U;
        CHECK(SizeFor(frame, EditorThemeSizeRole::ModalWidth) == Catch::Approx(0.0F));
        frame.supportedRadiusMask = 0U;
        CHECK(RadiusFor(frame, EditorThemeRadiusRole::Modal) == Catch::Approx(0.0F));
        frame.supportedFontMask = 0U;
        CHECK(FontSizeFor(frame, EditorThemeFontRole::Monospace) == Catch::Approx(0.0F));
    }

    TEST_CASE("Theme token fallbacks honor accessibility and live frame changes", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame before;
        before.revision = 10;
        const EditorThemeFrame after = [&before] {
            EditorThemeFrame next = before;
            next.revision = 11;
            next.uiScale = 1.5F;
            next.changeMask =
                Changes(EditorThemeChange::Colors, EditorThemeChange::Dpi) | static_cast<std::uint32_t>(EditorThemeChange::Accessibility);
            next.colors.surface = {0.2F, 0.2F, 0.2F, 1.0F};
            next.accessibility.highContrast = true;
            next.accessibility.reduceMotion = true;
            return next;
        }();

        REQUIRE(ValidateEditorThemeFrame(before).HasValue());
        REQUIRE(ValidateEditorThemeFrame(after).HasValue());
        CHECK(after.revision > before.revision);
        CHECK(after.uiScale != before.uiScale);
        CHECK(ColorFor(after, EditorThemeColorRole::Surface).red == Catch::Approx(0.2F));
        CHECK(ResolveEditorThemeMotionRole(EditorThemeMotionRole::Modal, after) == EditorThemeMotionRole::Instant);
        CHECK(MotionFor(after, EditorThemeMotionRole::Modal) == Catch::Approx(0.0F));
        CHECK(after.accessibility.highContrast);
        CHECK(after.accessibility.reduceMotion);
    }

    TEST_CASE("Theme token validation rejects incompatible or unsafe host frames", "[Extensions][EditorThemeTokens]") {
        EditorThemeFrame frame;

        frame.schemaVersion = EditorThemeTokenSchemaVersion + 1U;
        RequireErrorCode(ValidateEditorThemeFrame(frame), "editor_theme_token_version_unsupported");

        frame = {};
        frame.colors.accent.red = std::numeric_limits<float>::infinity();
        RequireErrorCode(ValidateEditorThemeFrame(frame), "editor_theme_token_invalid");

        frame = {};
        frame.uiScale = 4.0F;
        RequireErrorCode(ValidateEditorThemeFrame(frame), "editor_theme_token_invalid");

        frame = {};
        frame.metrics.rowGap = 0.0F;
        frame.metrics.welcomeSideWidth = 0.0F;
        frame.metrics.welcomePadding = 0.0F;
        REQUIRE(ValidateEditorThemeFrame(frame).HasValue());

        frame = {};
        frame.supportedIconMask = 1ULL << 63U;
        RequireErrorCode(ValidateEditorThemeFrame(frame), "editor_theme_token_invalid");
    }

    TEST_CASE("Theme token validation rejects invalid metadata and colors", "[Extensions][EditorThemeTokens]") {
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.revision = 0U;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.uiScale = 0.25F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.changeMask = 1U << 31U;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.colors.surface.red = -0.1F;
        });
    }

    TEST_CASE("Theme token validation rejects invalid typography and spacing", "[Extensions][EditorThemeTokens]") {
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.typography.caption = 13.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.typography.body = 20.0F;
            frame.typography.cardTitle = 18.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.spacing.xs = -1.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.spacing.xs = 10.0F;
        });
    }

    TEST_CASE("Theme token validation rejects invalid metrics, radii, interaction, and motion", "[Extensions][EditorThemeTokens]") {
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.metrics.smallControlHeight = 0.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.metrics.rowGap = -1.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.metrics.smallControlHeight = 36.0F;
            frame.metrics.mediumControlHeight = 32.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.radii.control = -1.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.interaction.hoverOpacity = 2.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.interaction.focusRingWidth = -1.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.motion.modalSeconds = 61.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.motion.fastSeconds = 0.2F;
            frame.motion.normalSeconds = 0.1F;
        });
    }

    TEST_CASE("Theme token validation rejects invalid fonts, icons, and accessibility", "[Extensions][EditorThemeTokens]") {
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.fonts.body = EditorThemeFontRole::None;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.fonts.sansBase = 0.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.icons.smallSize = 0.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.icons.strokeWidth = 0.0F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.accessibility.colorVision = EditorThemeColorVisionMode::Count;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.accessibility.textContrastMultiplier = 0.5F;
        });
        RequireInvalidThemeFrame([](EditorThemeFrame &frame) {
            frame.accessibility.colorVisionSeverity = 2.0F;
        });
    }
}  // namespace Horo::Extensions::Tests
