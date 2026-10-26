#pragma once

#include "Horo/Runtime/Render/UiPaintProgram.h"
#include "RenderFrontendTestSupport.h"
#include "RenderResourceRegistry.h"
#include "UiPaintFrontendState.h"
#include "UiSubmissionSourceFixture.h"

namespace Horo::Tests::UiPaintProgramTesting {
    using Horo::Tests::RenderFrontendTesting::CreateTrackingFrontend;
    using Horo::Tests::RenderFrontendTesting::lifecycleState;
    using Render::CookedUiPaintKernel;
    using Render::RenderFrontend;
    using Render::RenderTextureHandle;
    using Render::Test::UiAsset;
    using Render::Test::UiOwner;
    using Render::Test::UiRequire;
    using Render::Test::UiTree;
    using Render::UiPaintPreparedFrame;
    using Render::UiPaintProgram;
    using Render::UiRenderImageBinding;
    using Render::UiRenderImageTexture;
    using Render::UiRenderSubmission;
    using Runtime::Ui::NoUiRenderIndex;
    using Runtime::Ui::UiDrawCommand;
    using Runtime::Ui::UiElementTree;
    using Runtime::Ui::UiImageColorSpace;
    using Runtime::Ui::UiImageFallbackPolicy;
    using Runtime::Ui::UiImageFilter;
    using Runtime::Ui::UiImageMipmapMode;
    using Runtime::Ui::UiImagePage;
    using Runtime::Ui::UiImageResidencyState;
    using Runtime::Ui::UiImageResource;
    using Runtime::Ui::UiImageResourceHandle;
    using Runtime::Ui::UiImageResourceKind;
    using Runtime::Ui::UiImageResourceRegistry;
    using Runtime::Ui::UiImageResourceRevision;
    using Runtime::Ui::UiInteractionRevision;
    using Runtime::Ui::UiLogicalTransform;
    using Runtime::Ui::UiMask;
    using Runtime::Ui::UiMaskShape;
    using Runtime::Ui::UiOpacityGroup;
    using Runtime::Ui::UiRenderExtractor;
    using Runtime::Ui::UiRenderGeometryArena;
    using Runtime::Ui::UiRenderGeometryPlan;
    using Runtime::Ui::UiRenderSnapshotLimits;
    using Runtime::Ui::UiRenderSnapshotRevision;
    using Runtime::Ui::UiResolvedScreenCanvas;
    using Runtime::Ui::UiSolidDraw;

    /** @brief Establishes the injected backend before constructing its owning fixture member. */
    inline std::unique_ptr<RenderFrontend> PaintFrontend() {
        lifecycleState = {};
        lifecycleState.uiPaint.enabled = true;
        return CreateTrackingFrontend();
    }

    /** @brief Exercises the public program, real graph and ComposeUi admission; tracking never claims native pixels. */
    struct ProgramFixture final {
        std::unique_ptr<RenderFrontend> frontend{PaintFrontend()};
        UiElementTree tree{UiTree()};
        UiRenderSnapshotLimits limits{4, 0, 0, 0, 3, 1, 1, 3};
        UiRenderExtractor extractor{UiRequire(UiRenderExtractor::Create({{UiOwner(), 9, 1}, limits, 8}))};
        UiRenderGeometryArena arena{UiRequire(UiRenderGeometryArena::Create({{UiOwner(), 9, 1}, {32, 48, 4}, 8}))};
        UiImageResourceRegistry images{UiRequire(UiImageResourceRegistry::Create({UiOwner(), 1}))};

        static UiImageResource MaskImage() {
            const std::array pages{UiImagePage{{UiAsset(1), UiRequire(Assets::AssetTypeId::Parse("core.texture")), true},
                                               {4, 4},
                                               UiImageColorSpace::Linear,
                                               {UiImageFilter::Linear, UiImageFilter::Nearest, UiImageMipmapMode::Linear}}};
            return UiRequire(UiImageResource::Create({UiImageResourceKind::Image, UiImageFallbackPolicy::Reject, pages, {}}));
        }

        UiImageResourceHandle image{
            UiRequire(images.Publish(MaskImage(), UiRequire(UiImageResourceRevision::Create(3)), UiImageResidencyState::Resident))};
        std::optional<UiRenderImageTexture> texture;
        UiResolvedScreenCanvas projection{.logicalExtent = {40960, 30720}, .pixelExtent = {640, 480}, .pixelsPerDip = {1, 1}};
        std::array<UiDrawCommand, 4> commands;
        std::array<UiLogicalTransform, 1> transforms{};
        std::array<UiMask, 2> masks;
        std::array<UiOpacityGroup, 2> groups;
        std::uint64_t revision{1};
        std::uint64_t frameNumber{1};

        ProgramFixture() {
            std::array<std::byte, 64> alpha;
            alpha.fill(std::byte{128});
            texture = UiRequire(frontend->CreateUiMaskTexture(images, image, 0, alpha));
            const auto root = UiRequire(tree.Root()).handle;
            for (auto &command : commands)
                command = {root, {{0, 0}, {640, 640}}, 0, NoUiRenderIndex, 0, 1, UiSolidDraw{{1, 0, 0, 1}}, 0};
            masks = {UiMask{{{0, 0}, {640, 640}}, 0, 0}, UiMask{{{0, 0}, {320, 640}}, 0, 0, UiMaskShape::AlphaTexture, 0}};
            groups = {UiOpacityGroup{root, {{0, 0}, {640, 640}}, 0, 4, NoUiRenderIndex, 0.5F, 0, NoUiRenderIndex, 0},
                      UiOpacityGroup{root, {{0, 0}, {640, 640}}, 1, 2, 0, 0.25F, 0, NoUiRenderIndex, 1}};
            commands[1].group = commands[2].group = 1;
            commands[1].mask = commands[2].mask = 1;
        }

        UiRenderGeometryPlan Extract() {
            const auto descriptor =
                Runtime::Ui::Test::SnapshotDescriptor(tree, UiRequire(UiInteractionRevision::Create(8)),
                                                      UiRequire(UiRenderSnapshotRevision::Create(revision++)), {UiOwner(), 9, 1}, limits);
            const std::array resources{texture->Source()};
            const auto snapshot = UiRequire(
                extractor
                    .Extract(tree, descriptor,
                             {.commands = commands, .masks = masks, .transforms = transforms, .resources = resources, .groups = groups}));
            return UiRequire(arena.Build(snapshot, projection));
        }

        UiRenderGeometryPlan ExtractInto(UiRenderGeometryArena &target) {
            const auto geometry = Extract();
            return UiRequire(target.Build(geometry.SourceSnapshot(), projection));
        }

        std::unique_ptr<UiPaintProgram> Create(const UiRenderGeometryPlan &geometry, std::span<const RenderTextureHandle> extra = {}) {
            std::vector handles{texture->Creation().handle};
            handles.insert(handles.end(), extra.begin(), extra.end());
            auto result = UiRequire(UiPaintProgram::Create(*frontend, {projection, {512, 768, 128}, 1, 32}, geometry.SourceSnapshot(),
                                                           handles, CookedUiPaintKernel{}));
            REQUIRE(frontend->ProcessResourceRequests().HasValue());
            return result;
        }

        UiRenderSubmission Submission(UiPaintProgram &program, const UiRenderGeometryPlan &geometry,
                                      std::array<UiRenderImageBinding, 1> &bindings) {
            bindings = {
                UiRenderImageBinding{&images, image, 0, UiRequire(program.TextureResource(0, texture->Creation().handle)), &*texture}};
            return {&geometry, bindings, {}, {}};
        }

        void Encode(UiPaintProgram &program, const UiRenderGeometryPlan &geometry, const UiPaintPreparedFrame &prepared) {
            auto frame = UiRequire(frontend->BeginFrame({.frameNumber = frameNumber++, .outputExtent = {640, 480}}));
            REQUIRE(frame.Execute({}).HasValue());
            std::array<UiRenderImageBinding, 1> bindings;
            REQUIRE(program.Submit(frame, prepared, Submission(program, geometry, bindings)).HasValue());
            REQUIRE(frame.Present().HasValue());
        }
    };
}  // namespace Horo::Tests::UiPaintProgramTesting
