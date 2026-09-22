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
        frame.supportedIconMask = 1ULL << 63U;
        RequireErrorCode(ValidateEditorThemeFrame(frame), "editor_theme_token_invalid");
    }
}  // namespace Horo::Extensions::Tests
