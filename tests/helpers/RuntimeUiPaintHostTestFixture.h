#pragma once

#include "RuntimeUiCapabilityTestSupport.h"
#include "RuntimeUiPaintHost.h"
#include "unit/runtime/renderer/RenderFrontendTestSupport.h"
#include "unit/runtime/renderer/UiSubmissionSourceFixture.h"
#include "unit/runtime/ui/UiAnimationOwnerIntegrationFixture.h"

namespace Horo::Tests::UiPaintHostTesting {
    using namespace Application::Internal;
    using namespace Runtime::Ui;
    using UiAccess::Take;
    namespace Source = Runtime::Ui::ReloadTests;
    inline constexpr UiLogicalPoint NestedEffectScroll{0, 30};
    inline constexpr UiScalar NestedEffectChildOffset = 40;

    /** @brief Uses the real selected frontend contract while retaining explicit CPU-only tracking classification. */
    inline std::unique_ptr<Render::RenderFrontend> Frontend(const bool paintAvailable = true) {
        RenderFrontendTesting::lifecycleState = {};
        RenderFrontendTesting::lifecycleState.uiPaint.enabled = paintAvailable;
        return RenderFrontendTesting::CreateTrackingFrontend();
    }

    /** @brief Actual ordered RuntimeHost renderer consumer; it owns the same frame for scene, UI and native present. */
    class Renderer final : public Runtime::RuntimeLifecycleParticipant {
    public:
        Renderer(Render::RenderFrontend &frontend, RuntimeUiPaintPort port, const Render::FramebufferExtent &output)
            : frontend_(frontend), port_(std::move(port)), output_(output) {}

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            frame_.reset();
        }

        Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override {
            if (phase == Runtime::RuntimePhase::RenderExecution) {
                auto begun = frontend_.BeginFrame({.frameNumber = context.frameNumber, .outputExtent = output_});
                if (begun.HasError())
                    return Result<void>::Failure(begun.ErrorValue());
                frame_.emplace(std::move(begun).Value());
                const auto scene = frame_->Execute({});
                return scene.HasError() ? scene : port_.ComposeUi(*frame_, context.frameNumber);
            }
            if (phase == Runtime::RuntimePhase::Presentation && frame_) {
                const auto presented = frame_->Present();
                const auto receipt = port_.Presented(context.frameNumber, presented.HasValue());
                frame_.reset();
                return presented.HasError() ? presented : receipt;
            }
            return Result<void>::Success();
        }

    private:
        Render::RenderFrontend &frontend_;
        RuntimeUiPaintPort port_;
        const Render::FramebufferExtent &output_;
        std::optional<Render::RenderFrameScope> frame_;
    };

    /** @brief Real cooked/provider-loaded source, real canonical service and production App adapter, not a replacement paint owner. */
    struct Fixture final {
        std::unique_ptr<Render::RenderFrontend> frontend{Frontend()};
        UiAccess::Fixture source{false, false, true};
        DeterministicClock clock;
        Render::FramebufferExtent output{100, 100};
        UiCanvasDescriptor authoredCanvas;
        std::unique_ptr<Runtime::RuntimeHost> host{Take(Runtime::RuntimeHost::Create(clock))};
        RuntimeUiPaintPort port;
        std::shared_ptr<UiImageResourceRegistry> images{
            std::make_shared<UiImageResourceRegistry>(Take(UiImageResourceRegistry::Create({Source::Owner(), 2})))};
        UiImageResourceHandle image{
            Take(images->Publish(Render::Test::UiImage(), Source::Revision<UiImageResourceRevision>(1), UiImageResidencyState::Resident))};
        std::optional<Render::UiRenderImageTexture> texture;
        UiRenderViewId view{Source::Owner(), 10, 1};

        RuntimeUiPaintDeclaration Declaration(std::uint64_t revision = 1) const {
            RuntimeUiPaintDeclaration result{Source::Stable<UiDocumentId>(1), Source::Revision<UiDocumentRevision>(revision),
                                             Source::Stable<UiCanvasId>(2)};
            result.resources.push_back(texture->Source());
            result.components.push_back({Source::Stable<UiElementId>(11), UiImageDraw{0, {1, 1, 1, 1}}});
            result.masks.push_back({Source::Stable<UiElementId>(10), UiMaskShape::Rectangle});
            result.isolated.push_back(Source::Stable<UiElementId>(10));
            return result;
        }

        RuntimeUiPaintResources Assets() const {
            RuntimeUiPaintResources result;
            result.images.push_back({images, *texture});
            return result;
        }

        explicit Fixture(const bool paintAvailable = true, const UiLogicalPoint initialScroll = {})
            : frontend(Frontend(paintAvailable)), source(false, false, true, initialScroll) {
            std::array<std::byte, 64> pixels;
            pixels.fill(std::byte{128});
            texture.emplace(Take(frontend->CreateUiImageTexture(*images, image, 0, pixels)));
            REQUIRE(frontend->ProcessResourceRequests().HasValue());
            const auto admitted = Take(source.service.Acquire(Source::Instance()));
            authoredCanvas = admitted.Get()->Instance().Canvases().front();
            auto resources = Runtime::Ui::AnimationTests::Resources(admitted.Get()->Canvases().front());
            if (initialScroll != UiLogicalPoint{})
                for (auto &element : resources.definition.elements)
                    if (element.element == Source::Stable<UiElementId>(11)) {
                        element.layout.positioning = UiLayoutPositioning::Absolute;
                        element.layout.offsets.top = NestedEffectChildOffset;
                    }
            auto composition = Take(RuntimeUiPaintHost::Compose(*frontend, host->DispatchSource(), std::move(source.service)));
            port = composition.port;
            const UiRenderExtractorDescriptor extraction{view, {8, 2, 16, 8, 4, 8, 4, 4}, 8};
            const UiRenderGeometryArenaDescriptor geometry{view, {128, 192, 16}, 8};
            const UiResolvedScreenCanvas canvas{.logicalExtent = {6400, 6400}, .pixelExtent = {100, 100}, .pixelsPerDip = {1, 1}};
            const auto attached = port.AttachSource({Source::Instance(),
                                                     source.slots,
                                                     std::move(resources.registry),
                                                     std::move(resources.styles),
                                                     std::move(resources.definition),
                                                     {},
                                                     Runtime::Ui::AnimationTests::Config(),
                                                     Declaration(),
                                                     Assets(),
                                                     extraction,
                                                     geometry,
                                                     {canvas, {256, 384, 64}, 1, 32},
                                                     {}});
            if (paintAvailable) {
                REQUIRE(attached.HasValue());
            } else {
                REQUIRE(attached.HasError());
                CHECK(attached.ErrorValue().domain.Value() == Render::UiPaintFrameErrors::Unavailable.domain.Value());
                CHECK(attached.ErrorValue().code.Value() == Render::UiPaintFrameErrors::Unavailable.code.Value());
            }
            REQUIRE(host->AddParticipant(std::move(composition.participant)).HasValue());
            REQUIRE(host->AddParticipant(std::make_unique<Renderer>(*frontend, port, output)).HasValue());
            REQUIRE(host->Startup().HasValue());
        }

        ~Fixture() {
            if (host)
                host->Shutdown();
        }

        void Frame() {
            clock.Advance(Duration::FromMilliseconds(16));
            const auto frame = host->RunFrame();
            INFO((frame.HasError() ? frame.ErrorValue().message : ""));
            REQUIRE(frame.HasValue());
            REQUIRE(host->State() == Runtime::RuntimeLifecycleState::Running);
        }

        void Stop() {
            host->Shutdown();
            REQUIRE(Take(port.CollectRetired()));
        }
    };
}  // namespace Horo::Tests::UiPaintHostTesting
