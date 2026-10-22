#include "Horo/Runtime/Render/RenderPresentationOutput.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    RenderOutputColor LinearEdr(RenderDisplayColorSpace space = RenderDisplayColorSpace::Srgb) {
        return {RenderTextureFormat::Rgba16Float, space, RenderDisplayTransferFunction::Linear, RenderDisplayDynamicRange::High,
                RenderOutputColorMatching::ColorSpace};
    }

    RenderOutputCapabilities Capabilities() {
        RenderOutputCapabilities result;
        result.colors[0] = {};
        result.colors[1] = {};
        result.colors[1].matching = RenderOutputColorMatching::ColorSpace;
        result.colors[2] = LinearEdr();
        result.colors[3] = LinearEdr(RenderDisplayColorSpace::DisplayP3);
        result.count = 4;
        return result;
    }

    RenderOutputDisplayFacts AttachedDisplay() {
        return {1, RenderDisplayHdrSupport::Supported, 1.0F, 4.0F, true};
    }

    bool HasCode(const Error &error, const ErrorCodeDescriptor &descriptor) {
        return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
    }
}  // namespace

TEST_CASE("Presentation preserves exact SDR and separately qualified linear EDR intent", "[renderer][presentation]") {
    const auto display = AttachedDisplay();
    for (const auto color : {RenderOutputColor{}, LinearEdr(), LinearEdr(RenderDisplayColorSpace::DisplayP3)}) {
        const auto output = NegotiatePresentationOutput({color}, Capabilities(), display, 7);
        REQUIRE(output.HasValue());
        CHECK(output.Value().requested.color == color);
        CHECK(output.Value().active == color);
        CHECK(output.Value().revision == 7);
        CHECK(output.Value().display == display);
        // Current headroom stays one before content enables EDR; potential support is not physical output proof.
        CHECK(output.Value().display.linearHeadroom == 1.0F);
        CHECK(output.Value().display.potentialLinearHeadroom == 4.0F);
    }
}

TEST_CASE("Explicit HDR never silently selects an SDR or different color encoding", "[renderer][presentation]") {
    auto display = AttachedDisplay();
    for (const auto support : {RenderDisplayHdrSupport::Unknown, RenderDisplayHdrSupport::Unsupported}) {
        display.hdr = support;
        const auto rejected = NegotiatePresentationOutput({LinearEdr()}, Capabilities(), display, 1);
        REQUIRE(rejected.HasError());
        CHECK(HasCode(rejected.ErrorValue(), RenderPresentationErrors::HdrUnavailable));
    }
    display = AttachedDisplay();
    display.potentialLinearHeadroom.reset();
    CHECK(NegotiatePresentationOutput({LinearEdr()}, Capabilities(), display, 1).HasError());
    display.potentialLinearHeadroom = 1.0F;
    CHECK(NegotiatePresentationOutput({LinearEdr()}, Capabilities(), display, 1).HasError());
    display = AttachedDisplay();
    auto hdr10 = LinearEdr();
    hdr10.colorSpace = RenderDisplayColorSpace::Rec2020;
    hdr10.transfer = RenderDisplayTransferFunction::Pq;
    REQUIRE(hdr10.IsValid());
    const auto unsupported = NegotiatePresentationOutput({hdr10}, Capabilities(), display, 1);
    REQUIRE(unsupported.HasError());
    CHECK(HasCode(unsupported.ErrorValue(), RenderPresentationErrors::OutputUnsupported));
}

TEST_CASE("Output admission rejects detached and malformed display evidence", "[renderer][presentation]") {
    auto display = AttachedDisplay();
    display.attached = false;
    const auto unavailable = NegotiatePresentationOutput({}, Capabilities(), display, 1);
    REQUIRE(unavailable.HasError());
    CHECK(HasCode(unavailable.ErrorValue(), RenderPresentationErrors::OutputUnavailable));
    for (const float bad : {0.0F, -1.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        display = AttachedDisplay();
        display.linearHeadroom = bad;
        CHECK_FALSE(display.IsValid());
        CHECK(NegotiatePresentationOutput({}, Capabilities(), display, 1).HasError());
        display = AttachedDisplay();
        display.potentialLinearHeadroom = bad;
        CHECK_FALSE(display.IsValid());
    }
    display = AttachedDisplay();
    display.linearHeadroom = 5.0F;
    CHECK_FALSE(display.IsValid());
    display = AttachedDisplay();
    display.revision = 0;
    CHECK_FALSE(display.IsValid());
    display = AttachedDisplay();
    display.hdr = static_cast<RenderDisplayHdrSupport>(255);
    CHECK_FALSE(display.IsValid());
}

TEST_CASE("Output capability sets are finite exact unique encodings", "[renderer][presentation]") {
    auto capabilities = Capabilities();
    CHECK(capabilities.IsValid());
    capabilities.count = 0;
    CHECK_FALSE(capabilities.IsValid());
    capabilities.count = 5;
    CHECK_FALSE(capabilities.IsValid());
    capabilities = Capabilities();
    capabilities.colors[1] = capabilities.colors[0];
    CHECK_FALSE(capabilities.IsValid());
    capabilities = Capabilities();
    capabilities.colors[0].format = RenderTextureFormat::Depth32Float;
    CHECK_FALSE(capabilities.IsValid());
    CHECK(NegotiatePresentationOutput({}, capabilities, AttachedDisplay(), 1).HasError());
    CHECK(NegotiatePresentationOutput({}, Capabilities(), AttachedDisplay(), 0).HasError());
}

TEST_CASE("Output encoding rejects unknown or contradictory transfer and pixel formats", "[renderer][presentation]") {
    auto color = LinearEdr();
    color.format = RenderTextureFormat::Bgra8Unorm;
    CHECK_FALSE(color.IsValid());
    color = {};
    color.transfer = RenderDisplayTransferFunction::Pq;
    CHECK_FALSE(color.IsValid());
    color = LinearEdr();
    color.transfer = RenderDisplayTransferFunction::Hlg;
    CHECK_FALSE(color.IsValid());
    color = {};
    color.colorSpace = static_cast<RenderDisplayColorSpace>(255);
    CHECK_FALSE(color.IsValid());
    color = {};
    color.dynamicRange = static_cast<RenderDisplayDynamicRange>(255);
    CHECK_FALSE(color.IsValid());
}

TEST_CASE("Unmanaged output is only the exact historical SDR marker and cannot admit HDR", "[renderer][presentation]") {
    auto color = LinearEdr();
    color.matching = RenderOutputColorMatching::Unmanaged;
    CHECK_FALSE(color.IsValid());
    CHECK(NegotiatePresentationOutput({color}, Capabilities(), AttachedDisplay(), 1).HasError());
    color = {};
    color.matching = static_cast<RenderOutputColorMatching>(255);
    CHECK_FALSE(color.IsValid());
    color = {};
    color.matching = RenderOutputColorMatching::ColorSpace;
    const auto managed = NegotiatePresentationOutput({color}, Capabilities(), AttachedDisplay(), 2);
    REQUIRE(managed.HasValue());
    CHECK(managed.Value().active.matching == RenderOutputColorMatching::ColorSpace);
    const auto legacy = NegotiatePresentationOutput({}, Capabilities(), AttachedDisplay(), 3);
    REQUIRE(legacy.HasValue());
    CHECK(legacy.Value().active.matching == RenderOutputColorMatching::Unmanaged);
    CHECK(legacy.Value().active != managed.Value().active);
}
