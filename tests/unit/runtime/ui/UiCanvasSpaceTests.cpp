#include "Horo/Runtime/Ui/UiCanvasSpace.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        using Test::Stable;

        UiCanvasDescriptor Canvas(const UiRenderMode renderMode = UiRenderMode::ScreenSpaceOverlay,
                                  const UiScaleMode scaleMode = UiScaleMode::ScaleWithScreenSize) {
            return {.id = Stable<UiCanvasId>(1),
                    .rootElement = Stable<UiElementId>(2),
                    .renderMode = renderMode,
                    .referenceResolution = {1920, 1080},
                    .scaleMode = scaleMode};
        }

        void RequireFailureCode(const auto &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("Runtime UI canvas descriptors expose every backend-neutral render mode", "[runtime_ui][canvas]") {
            REQUIRE(Canvas(UiRenderMode::ScreenSpaceOverlay).IsValid());
            REQUIRE(Canvas(UiRenderMode::ScreenSpaceCamera).IsValid());
            REQUIRE(Canvas(UiRenderMode::WorldSpace).IsValid());

            auto malformed = Canvas();
            malformed.renderMode = static_cast<UiRenderMode>(255);
            REQUIRE_FALSE(malformed.IsValid());
            malformed = Canvas();
            malformed.scaleMode = static_cast<UiScaleMode>(255);
            REQUIRE_FALSE(malformed.IsValid());
            malformed = Canvas();
            malformed.referenceResolution.width = 0;
            REQUIRE_FALSE(malformed.IsValid());
            malformed = Canvas();
            malformed.referenceResolution.height = MaximumUiCanvasReferenceDip + 1U;
            REQUIRE_FALSE(malformed.IsValid());
        }

        TEST_CASE("Scale-with-screen-size resolves a uniform reduced scale and full logical viewport", "[runtime_ui][canvas]") {
            const auto referenceAspect = ResolveUiScreenCanvas(Canvas(), {1280, 720});
            REQUIRE(referenceAspect.HasValue());
            REQUIRE(referenceAspect.Value().pixelsPerDip == UiCanvasDeviceScale{2, 3});
            REQUIRE(referenceAspect.Value().logicalExtent == UiCanvasLogicalExtent{1920 * 64, 1080 * 64});

            const auto ultrawide = ResolveUiScreenCanvas(Canvas(UiRenderMode::ScreenSpaceCamera), {2560, 1080});
            REQUIRE(ultrawide.HasValue());
            REQUIRE(ultrawide.Value().pixelsPerDip == UiCanvasDeviceScale{1, 1});
            REQUIRE(ultrawide.Value().logicalExtent == UiCanvasLogicalExtent{2560 * 64, 1080 * 64});
        }

        TEST_CASE("Constant pixel and caller-resolved physical scales remain distinct", "[runtime_ui][canvas]") {
            const auto pixels =
                ResolveUiScreenCanvas(Canvas(UiRenderMode::ScreenSpaceOverlay, UiScaleMode::ConstantPixelSize), {300, 150}, {9, 4});
            REQUIRE(pixels.HasValue());
            REQUIRE(pixels.Value().pixelsPerDip == UiCanvasDeviceScale{1, 1});
            REQUIRE(pixels.Value().logicalExtent == UiCanvasLogicalExtent{300 * 64, 150 * 64});

            const auto scaled = ResolveUiScreenCanvas(Canvas(), {1280, 720}, {9, 4});
            REQUIRE(scaled.HasValue());
            REQUIRE(scaled.Value().pixelsPerDip == UiCanvasDeviceScale{2, 3});

            const auto physical =
                ResolveUiScreenCanvas(Canvas(UiRenderMode::ScreenSpaceOverlay, UiScaleMode::ConstantPhysicalSize), {300, 150}, {6, 4});
            REQUIRE(physical.HasValue());
            REQUIRE(physical.Value().pixelsPerDip == UiCanvasDeviceScale{3, 2});
            REQUIRE(physical.Value().logicalExtent == UiCanvasLogicalExtent{200 * 64, 100 * 64});
        }

        TEST_CASE("Canvas logical conversion uses checked ties-to-even rounding", "[runtime_ui][canvas]") {
            const auto resolved =
                ResolveUiScreenCanvas(Canvas(UiRenderMode::ScreenSpaceOverlay, UiScaleMode::ConstantPhysicalSize), {1, 3}, {128, 1});
            REQUIRE(resolved.HasValue());
            REQUIRE(resolved.Value().logicalExtent == UiCanvasLogicalExtent{0, 2});
        }

        TEST_CASE("Viewport evidence resolves safe content, DPI, UI scale, font scale and pixel snapping",
                  "[runtime_ui][canvas][presentation]") {
            auto canvas = Canvas();
            canvas.presentation = {.safeArea = UiSafeAreaMode::Inset,
                                   .uiScale = {5, 4},
                                   .fontScale = {3, 2},
                                   .pixelSnap = UiPixelSnapMode::Edges};
            const auto resolved = ResolveUiScreenCanvasWithEvidence(canvas, {{1920, 1080}, {144, 96}, {100, 20, 60, 40}});
            REQUIRE(resolved.HasValue());
            REQUIRE(resolved.Value().ContentPixelRect() == UiCanvasPixelRect{100, 20, 1760, 1020});
            REQUIRE(resolved.Value().safeAreaInsets == UiCanvasPixelInsets{100, 20, 60, 40});
            REQUIRE(resolved.Value().dpiScale == UiCanvasDeviceScale{3, 2});
            REQUIRE(resolved.Value().pixelsPerDip == UiCanvasDeviceScale{5, 4});
            REQUIRE(resolved.Value().logicalExtent == UiCanvasLogicalExtent{1408 * 64, 816 * 64});
            REQUIRE(resolved.Value().uiScale == UiCanvasScaleFactor{5, 4});
            REQUIRE(resolved.Value().fontScale == UiCanvasScaleFactor{3, 2});
            REQUIRE(resolved.Value().pixelSnap == UiPixelSnapMode::Edges);
            REQUIRE(resolved.Value().IsValid());

            const auto resized = ResolveUiScreenCanvasWithEvidence(canvas, {{2560, 1440}, {192, 128}, {0, 0, 0, 0}});
            REQUIRE(resized.HasValue());
            REQUIRE(resized.Value().ContentPixelRect() == UiCanvasPixelRect{0, 0, 2560, 1440});
            REQUIRE(resized.Value().logicalExtent == UiCanvasLogicalExtent{1536 * 64, 864 * 64});
        }

        TEST_CASE("Presentation evidence rejects malformed DPI and safe-area changes transactionally", "[runtime_ui][canvas][failure]") {
            auto canvas = Canvas();
            canvas.presentation.safeArea = UiSafeAreaMode::Inset;
            const auto tooMuchInset = ResolveUiScreenCanvasWithEvidence(canvas, {{100, 100}, {}, {50, 0, 50, 0}});
            RequireFailureCode(tooMuchInset, UiErrors::CanvasSpaceInvalid);

            const auto malformedDpi = ResolveUiScreenCanvasWithEvidence(canvas, {{100, 100}, {2, 0}, {0, 0, 0, 0}});
            RequireFailureCode(malformedDpi, UiErrors::CanvasSpaceInvalid);

            auto malformedPolicy = canvas;
            malformedPolicy.presentation.fontScale.numerator = 0;
            const auto invalidPolicy = ResolveUiScreenCanvasWithEvidence(malformedPolicy, {{100, 100}, {}, {}});
            RequireFailureCode(invalidPolicy, UiErrors::CanvasSpaceInvalid);

            auto malformedSnap = canvas;
            malformedSnap.presentation.pixelSnap = static_cast<UiPixelSnapMode>(255);
            const auto invalidSnap = ResolveUiScreenCanvasWithEvidence(malformedSnap, {{100, 100}, {}, {}});
            RequireFailureCode(invalidSnap, UiErrors::CanvasSpaceInvalid);
        }

        TEST_CASE("World canvases retain logical space without fabricating device projection", "[runtime_ui][canvas]") {
            const auto world = ResolveUiWorldCanvas(Canvas(UiRenderMode::WorldSpace));
            REQUIRE(world.HasValue());
            REQUIRE(world.Value() == UiCanvasLogicalExtent{1920 * 64, 1080 * 64});
            RequireFailureCode(ResolveUiScreenCanvas(Canvas(UiRenderMode::WorldSpace), {1920, 1080}), UiErrors::CanvasSpaceModeMismatch);
            RequireFailureCode(ResolveUiWorldCanvas(Canvas()), UiErrors::CanvasSpaceModeMismatch);

            auto safeWorld = Canvas(UiRenderMode::WorldSpace);
            safeWorld.presentation.safeArea = UiSafeAreaMode::Inset;
            RequireFailureCode(ResolveUiWorldCanvas(safeWorld), UiErrors::CanvasSpaceModeMismatch);
            safeWorld.presentation.safeArea = UiSafeAreaMode::Ignore;
            safeWorld.presentation.pixelSnap = UiPixelSnapMode::Edges;
            RequireFailureCode(ResolveUiWorldCanvas(safeWorld), UiErrors::CanvasSpaceModeMismatch);
        }

        TEST_CASE("Canvas resolution rejects malformed evidence and representational overflow", "[runtime_ui][canvas]") {
            RequireFailureCode(ResolveUiScreenCanvas(Canvas(), {}), UiErrors::CanvasSpaceInvalid);
            RequireFailureCode(ResolveUiScreenCanvas(Canvas(UiRenderMode::ScreenSpaceOverlay, UiScaleMode::ConstantPhysicalSize), {1, 1},
                                                     {}),
                               UiErrors::CanvasSpaceInvalid);
            RequireFailureCode(ResolveUiScreenCanvas(Canvas(UiRenderMode::ScreenSpaceOverlay, UiScaleMode::ConstantPixelSize),
                                                     {std::numeric_limits<std::uint32_t>::max(), 1}),
                               UiErrors::CanvasSpaceOverflow);
            RequireFailureCode(ResolveUiScreenCanvas(Canvas(UiRenderMode::ScreenSpaceOverlay, UiScaleMode::ConstantPhysicalSize),
                                                     {std::numeric_limits<std::uint32_t>::max(), 1},
                                                     {1, std::numeric_limits<std::uint32_t>::max()}),
                               UiErrors::CanvasSpaceOverflow);

            auto maximumWorld = Canvas(UiRenderMode::WorldSpace);
            maximumWorld.referenceResolution = {MaximumUiCanvasReferenceDip, MaximumUiCanvasReferenceDip};
            const auto resolved = ResolveUiWorldCanvas(maximumWorld);
            REQUIRE(resolved.HasValue());
            REQUIRE(resolved.Value().width == static_cast<std::int32_t>(MaximumUiCanvasReferenceDip * 64U));
        }

        TEST_CASE("Canvas space errors expose stable actionable descriptors", "[runtime_ui][canvas][errors]") {
            const ErrorCodeDescriptor *descriptors[]{&UiErrors::CanvasSpaceInvalid, &UiErrors::CanvasSpaceModeMismatch,
                                                     &UiErrors::CanvasSpaceOverflow};
            for (const ErrorCodeDescriptor *descriptor : descriptors) {
                REQUIRE(descriptor->domain.Value() == "horo.runtime_ui");
                REQUIRE_FALSE(descriptor->summary.empty());
                REQUIRE_FALSE(descriptor->remediationHint.empty());
            }
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
