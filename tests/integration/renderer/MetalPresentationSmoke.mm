#include "Horo/Runtime/Render/RenderFrontend.h"
#include "editor/renderer/metal/SdlMetalPresentationPort.h"
#include "runtime/renderer/modules/metal/MetalPresentationOutput.h"

#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Editor;
    using Clock = std::chrono::steady_clock;

    struct PresentationFixture final {
        SDL_Window *window{};
        std::unique_ptr<SdlMetalPresentationPort> port;
        MetalEditorGraphicsBridge bridge;
        std::unique_ptr<RenderFrontend> frontend;

        PresentationFixture() {
            REQUIRE(SDL_Init(SDL_INIT_VIDEO));
            window = SDL_CreateWindow("Horo Metal presentation qualification", 128, 96, SDL_WINDOW_METAL);
            REQUIRE(window != nullptr);
            port = std::make_unique<SdlMetalPresentationPort>(*window);
        }

        ~PresentationFixture() {
            if (frontend) {
                static_cast<void>(frontend->BeginShutdown());
                const auto deadline = Clock::now() + std::chrono::seconds{5};
                for (;;) {
                    const auto result = frontend->PollDrain();
                    if (result.HasValue() && result.Value() == RenderBackendDrainProgress::Drained)
                        break;
                    if (Clock::now() >= deadline)
                        std::terminate();
                    SDL_PumpEvents();
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                frontend.reset();
            }
            port.reset();
            SDL_DestroyWindow(window);
            SDL_Quit();
        }

        void CloseFrontend() {
            REQUIRE(frontend->BeginShutdown().HasValue());
            const auto deadline = Clock::now() + std::chrono::seconds{5};
            do {
                const auto drained = frontend->PollDrain();
                REQUIRE(drained.HasValue());
                if (drained.Value() == RenderBackendDrainProgress::Drained) {
                    frontend.reset();
                    return;
                }
                // Pending retains the complete native surface and its window parent.
                CHECK(port->Layer() != nullptr);
                CHECK(window != nullptr);
                SDL_PumpEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            } while (Clock::now() < deadline);
            FAIL("The original native generation did not drain; its owners are retained");
        }

        void Initialize() {
            REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0"));
            REQUIRE_FALSE(SDL_GetHintBoolean(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, true));
            RenderBackendRegistry registry;
            REQUIRE(RegisterMetalRenderBackend(registry, *port, bridge).HasValue());
            REQUIRE(registry.Seal().HasValue());
            auto created = RenderFrontend::Create(registry, RenderBackendId{"metal"}, {.requirePresentation = true});
            INFO((created.HasError() ? created.ErrorValue().message : "Metal presentation admitted"));
            REQUIRE(created.HasValue());
            frontend = std::move(created).Value();
        }

        void AwaitReady(FramebufferExtent extent) {
            const auto deadline = Clock::now() + std::chrono::seconds{5};
            do {
                SDL_PumpEvents();
                const auto ready = frontend->PollPresentationAvailability(extent);
                if (ready.HasValue() && ready.Value())
                    return;
                if (ready.HasError())
                    REQUIRE(ready.ErrorValue().code.Value() == RenderPresentationErrors::DrawableUnavailable.code.Value());
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            } while (Clock::now() < deadline);
            FAIL("The actual Metal drawable did not become ready within the qualification budget");
        }

        void Present(std::uint64_t frameNumber, FramebufferExtent extent, RenderOutputColor color = {}) {
            AwaitReady(extent);
            auto begun = frontend->BeginFrame({frameNumber, extent});
            REQUIRE(begun.HasValue());
            auto frame = std::move(begun).Value();
            const std::array passes{RenderPassDescriptor{.id = RenderPassId{1},
                                                         .kind = RenderPassKind::Graphics,
                                                         .primaryOutput = PrimaryOutputAttachment{.clearColor = {2.0F, 0.25F, 0.5F, 1.0F},
                                                                                                  .outputColor = color}}};
            REQUIRE(frame.Execute(passes).HasValue());
            REQUIRE(frame.Present().HasValue());
        }
    };
}  // namespace

TEST_CASE("SDL Metal host policy precedes view creation and publishes actual revisioned display facts",
          "[integration][metal][gpu][presentation]") {
    PresentationFixture fixture;
    REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "1"));
    const auto rejected = fixture.port->CreateSurface();
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == "render.metal.external_resize_policy");
    CHECK(fixture.port->Layer() == nullptr);
    REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0"));
    REQUIRE(fixture.port->CreateSurface().HasValue());
    const auto facts = fixture.port->OutputDisplayFacts();
    REQUIRE(facts.HasValue());
    CHECK(facts.Value().IsValid());
    for (unsigned frame = 0; frame != 32; ++frame)
        CHECK(fixture.port->OutputDisplayFacts().Value() == facts.Value());
    std::optional<Error> foreignFailure;
    std::thread foreign{[&] {
        const auto queried = fixture.port->OutputDisplayFacts();
        if (queried.HasError())
            foreignFailure = queried.ErrorValue();
    }};
    foreign.join();
    REQUIRE(foreignFailure.has_value());
    CHECK(foreignFailure->code.Value() == RenderPresentationErrors::ThreadAffinityViolation.code.Value());
    fixture.port->DestroySurface();
    CHECK(fixture.port->Layer() == nullptr);
    CHECK(fixture.port->OutputDisplayFacts().HasError());
}

TEST_CASE("Native Metal realizes SDR, validates real drawables and coalesces latest resize before reuse",
          "[integration][metal][gpu][presentation]") {
    PresentationFixture fixture;
    fixture.Initialize();
    const auto output = fixture.frontend->PresentationOutput();
    REQUIRE(output.HasValue());
    CHECK(output.Value().active == RenderOutputColor{});
    CAMetalLayer *layer = (__bridge CAMetalLayer *)fixture.port->Layer();
    CHECK(Detail::MetalPresentationOutputMatches(layer, output.Value()));
    fixture.Present(1, {128, 96});
    static_cast<void>(fixture.frontend->PollPresentationAvailability({64, 64}));
    fixture.Present(2, {96, 48});
    CHECK(layer.drawableSize.width == 96);
    CHECK(layer.drawableSize.height == 48);
    const auto before = fixture.frontend->PresentationOutput();
    REQUIRE(before.HasValue());
    auto unsupported = before.Value().requested;
    unsupported.color.colorSpace = RenderDisplayColorSpace::Rec2020;
    const auto rejected = fixture.frontend->ConfigurePresentationOutput(unsupported);
    REQUIRE(rejected.HasError());
    CHECK(fixture.frontend->PresentationOutput().Value().active == before.Value().active);
    CHECK(Detail::MetalPresentationOutputMatches(layer, before.Value()));
    fixture.CloseFrontend();
    CHECK(fixture.port->Layer() == nullptr);
    CHECK(fixture.bridge.Device() == nullptr);
    fixture.Initialize();
    const auto reopened = fixture.frontend->PresentationOutput();
    REQUIRE(reopened.HasValue());
    CHECK(reopened.Value().display.revision > before.Value().display.revision);
    fixture.Present(1, {128, 96});
}

TEST_CASE("Metal explicit sRGB matching round-trips back to the unchanged unmanaged SDR policy",
          "[integration][metal][gpu][presentation]") {
    PresentationFixture fixture;
    fixture.Initialize();
    CAMetalLayer *layer = (__bridge CAMetalLayer *)fixture.port->Layer();
    CHECK(layer.colorspace == nullptr);
    auto matched = RenderOutputColor{};
    matched.matching = RenderOutputColorMatching::ColorSpace;
    const auto configured = fixture.frontend->ConfigurePresentationOutput({matched});
    REQUIRE(configured.HasValue());
    CHECK(configured.Value().active.matching == RenderOutputColorMatching::ColorSpace);
    CHECK(layer.colorspace != nullptr);
    CHECK(Detail::MetalPresentationOutputMatches(layer, configured.Value()));
    const auto legacy = fixture.frontend->ConfigurePresentationOutput({});
    REQUIRE(legacy.HasValue());
    CHECK(legacy.Value().active == RenderOutputColor{});
    CHECK(layer.colorspace == nullptr);
    CHECK(layer.EDRMetadata == nil);
    CHECK(Detail::MetalPresentationOutputMatches(layer, legacy.Value()));
    fixture.Present(1, {128, 96});
}

TEST_CASE("Metal linear EDR qualification uses actual host potential and native layer readback",
          "[integration][metal][gpu][presentation][linear-edr]") {
    PresentationFixture fixture;
    fixture.Initialize();
    const auto facts = fixture.port->OutputDisplayFacts();
    REQUIRE(facts.HasValue());
    if (facts.Value().hdr != RenderDisplayHdrSupport::Supported || !facts.Value().potentialLinearHeadroom ||
        *facts.Value().potentialLinearHeadroom <= 1.0F)
        SKIP("This actual host display does not publish linear EDR capability; no HDR output is claimed");
    const RenderOutputColor linearEdr{RenderTextureFormat::Rgba16Float, RenderDisplayColorSpace::DisplayP3,
                                      RenderDisplayTransferFunction::Linear, RenderDisplayDynamicRange::High,
                                      RenderOutputColorMatching::ColorSpace};
    const auto output = fixture.frontend->ConfigurePresentationOutput({linearEdr});
    REQUIRE(output.HasValue());
    CHECK(output.Value().active == linearEdr);
    CAMetalLayer *layer = (__bridge CAMetalLayer *)fixture.port->Layer();
    CHECK(layer.pixelFormat == MTLPixelFormatRGBA16Float);
    CHECK(layer.wantsExtendedDynamicRangeContent);
    CHECK(Detail::MetalPresentationOutputMatches(layer, output.Value()));
    fixture.Present(1, {128, 96}, linearEdr);
    const auto current = fixture.frontend->PresentationOutput();
    REQUIRE(current.HasValue());
    if (current.Value().display.linearHeadroom)
        INFO("Actual current linear headroom: " << *current.Value().display.linearHeadroom);
    else
        INFO("Actual current linear headroom is unavailable");
    INFO("Physical extended-range qualification requires actual current headroom > 1; layer realization alone does not prove it");
}
