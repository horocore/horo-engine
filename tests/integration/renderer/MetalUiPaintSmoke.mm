#include "Horo/Runtime/Render/UiPaintFrameUpload.h"
#include "MetalUiPaintFixture.h"
#include "RenderMemoryTestSupport.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"
#include "runtime/renderer/modules/metal/MetalResourceRuntime.h"

#import <Metal/Metal.h>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace Horo::Render::Detail {
    namespace {
        constexpr FramebufferExtent Extent{64, 64};
        constexpr std::size_t RowBytes = 256;
        using Color = std::array<float, 4>;

        /** @brief Owns only test preparation/readback storage; production commands and lifetime tracking remain unchanged. */
        class PaintSmoke final {
        public:
            MetalResourceRuntime runtime;
            __strong id<MTLDevice> device{MTLCreateSystemDefaultDevice()};
            __strong id<MTLCommandQueue> queue{nil};
            std::shared_ptr<IResidentUiPaintKernel> kernel;
            std::vector<RenderGraphResourceInstance> resources;
            std::vector<UiPaintVertex> vertices;
            std::vector<std::uint32_t> indices;
            std::vector<UiPaintParameters> parameters;
            std::array<std::uint64_t, 3> buffers{};
            std::uint64_t readback{};
            std::uint64_t revision{};

            explicit PaintSmoke(const std::filesystem::path &root) {
                REQUIRE(device != nil);
                queue = [device newCommandQueue];
                REQUIRE(queue != nil);
                runtime.Initialize((__bridge void *)device, (__bridge void *)queue);
                const auto cooked = Tests::LoadMetalUiPaintFixture(root, RenderTextureFormat::Rgba8Unorm);
                auto resident = runtime.RealizeUiPaintKernel(cooked);
                REQUIRE(resident.HasValue());
                kernel = std::move(resident).Value();
                constexpr std::array usages{RenderBufferUsage::Vertex, RenderBufferUsage::Index, RenderBufferUsage::Uniform};
                for (std::size_t index = 0; index < buffers.size(); ++index)
                    buffers[index] = Buffer(16384, usages[index], index + 1);
                readback = Buffer(RowBytes * Extent.height, RenderBufferUsage::CopyDestination, 4);
            }

            ~PaintSmoke() {
                runtime.Shutdown();
            }

            /** @brief Creates one actual graph buffer in a separate tracked placement. */
            std::uint64_t Buffer(std::size_t bytes, RenderBufferUsage usage, std::uint64_t slot) {
                const RenderBufferDescriptor descriptor{.byteSize = bytes, .usage = usage, .access = RenderBufferAccess::HostVisible};
                const auto cost = runtime.QueryBufferMemoryCost(descriptor);
                REQUIRE(cost.HasValue());
                const auto created = runtime.CreateBuffer(descriptor, {}, TestSupport::PlacementFor(cost.Value(), slot, slot));
                REQUIRE(created.HasValue());
                resources.push_back({{{1}, static_cast<std::uint32_t>(slot)}, created.Value()});
                return created.Value();
            }

            /** @brief Creates an actual single-sample target with readback and sampling roles declared before recording. */
            RenderGraphResourceId Texture(RenderTextureFormat format, std::uint64_t slot) {
                const bool stencil = format == RenderTextureFormat::Depth32FloatStencil8;
                const RenderTextureDescriptor descriptor{.extent = Extent,
                                                         .format = format,
                                                         .usage = stencil
                                                                      ? RenderTextureUsage::RenderAttachment
                                                                      : RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled |
                                                                            RenderTextureUsage::CopySource};
                const auto cost = runtime.QueryTextureMemoryCost(descriptor);
                REQUIRE(cost.HasValue());
                const auto created = runtime.CreateTexture(descriptor, {}, TestSupport::PlacementFor(cost.Value(), slot, slot));
                REQUIRE(created.HasValue());
                const RenderGraphResourceId id{{1}, static_cast<std::uint32_t>(slot)};
                resources.push_back({id, created.Value()});
                return id;
            }

            /** @brief Appends exact transformed quad geometry; UV remains local for the native rounded-mask shader. */
            UiPaintDraw Quad(std::array<std::array<float, 2>, 4> corners, Color color) {
                const auto firstVertex = static_cast<std::uint32_t>(vertices.size());
                const auto firstIndex = static_cast<std::uint32_t>(indices.size());
                constexpr std::array<std::array<float, 2>, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
                for (std::size_t index = 0; index < corners.size(); ++index)
                    vertices.push_back({corners[index], uv[index], color});
                for (const auto index : {0U, 1U, 2U, 0U, 2U, 3U})
                    indices.push_back(firstVertex + index);
                return {.firstIndex = firstIndex, .indexCount = 6, .scissor = {0, 0, Extent.width, Extent.height}};
            }

            /** @brief Builds one bounded native pass sharing the exact reusable vertex/index/uniform publication. */
            RenderGraphUiPaint Pass(RenderGraphResourceId target, std::span<const UiPaintDraw> draws, RenderGraphResourceId stencil = {},
                                    std::span<const RenderGraphResourceId> sampled = {}) {
                const auto first = static_cast<std::uint32_t>(parameters.size());
                for (const auto &draw : draws)
                    parameters.push_back({.extent = {64, 64},
                                          .opacity = draw.opacity,
                                          .sampling = static_cast<std::uint32_t>(draw.sampling),
                                          .flags = draw.coverage == NoUiPaintTexture ? 0U : 1U,
                                          .maskExtent = draw.maskExtent,
                                          .cornerRadii = draw.cornerRadii});
                return {.color = target,
                        .stencil = stencil,
                        .vertices = {{1}, 1},
                        .indices = {{1}, 2},
                        .parameters = {{1}, 3},
                        .sampledResources = sampled,
                        .extent = Extent,
                        .draws = draws,
                        .kernel = kernel,
                        .clearColor = true,
                        .clearStencil = stencil.IsValid(),
                        .firstParameter = first};
            }

            /** @brief Publishes all three native residents once, then encodes each pass using the same exact revision. */
            void Encode(id<MTLCommandBuffer> commands, std::span<RenderGraphUiPaint> passes) {
                ++revision;
                REQUIRE(runtime.UpdateUiPaintFrame({buffers, {revision, vertices, indices, parameters, {}}}).HasValue());
                for (auto &pass : passes) {
                    pass.vertexCount = static_cast<std::uint32_t>(vertices.size());
                    pass.indexCount = static_cast<std::uint32_t>(indices.size());
                    pass.frameRevision = revision;
                    REQUIRE(runtime.ExecuteGraphWorkload((__bridge void *)commands, RenderGraphWorkload{pass}, resources).HasValue());
                }
            }

            /** @brief Copies native pixels and waits only under the finite test readback deadline. */
            void Read(id<MTLCommandBuffer> commands, RenderGraphResourceId target) {
                const auto found = std::ranges::find_if(resources, [&](const auto &item) {
                    return item.resource == target;
                });
                REQUIRE(found != resources.end());
                const auto *texture = reinterpret_cast<MetalTextureInstance *>(static_cast<std::uintptr_t>(found->instance));
                const auto *destination = reinterpret_cast<MetalBufferInstance *>(static_cast<std::uintptr_t>(readback));
                id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                REQUIRE(blit != nil);
                [blit copyFromTexture:texture->texture
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(0, 0, 0)
                                  sourceSize:MTLSizeMake(64, 64, 1)
                                    toBuffer:destination->buffer
                           destinationOffset:0
                      destinationBytesPerRow:RowBytes
                    destinationBytesPerImage:RowBytes * Extent.height];
                [blit endEncoding];
                [commands commit];
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                REQUIRE(commands.status == MTLCommandBufferStatusCompleted);
                runtime.DrainGraphRetirements();
            }

            /** @brief Reads only actual completed native output bytes, with no CPU image synthesis. */
            std::array<std::uint8_t, 4> Pixel(std::size_t x, std::size_t y) const {
                REQUIRE(x < Extent.width);
                REQUIRE(y < Extent.height);
                const auto *buffer = reinterpret_cast<MetalBufferInstance *>(static_cast<std::uintptr_t>(readback));
                const auto *bytes = static_cast<const std::uint8_t *>(buffer->buffer.contents) + y * RowBytes + x * 4;
                return {bytes[0], bytes[1], bytes[2], bytes[3]};
            }
        };

        constexpr std::array<std::array<float, 2>, 4> Full{{{0, 0}, {64, 0}, {64, 64}, {0, 64}}};

        /** @brief Requires installed-tool qualification inputs rather than substituting source compilation at runtime. */
        std::filesystem::path FixtureRoot() {
            const auto *root = std::getenv("HORO_UI_PAINT_METAL_FIXTURE");
            if (!root)
                SKIP("Actual same-run locked HLSL/MSL and Apple offline Metal fixture is unavailable.");
            return std::filesystem::path{root};
        }
    }  // namespace

    TEST_CASE("Cooked native Metal UI rounded masks clip transformed pixels and balanced stencil",
              "[integration][renderer][metal][gpu][ui-paint]") {
        PaintSmoke fixture{FixtureRoot()};
        const auto target = fixture.Texture(RenderTextureFormat::Rgba8Unorm, 5);
        const auto stencil = fixture.Texture(RenderTextureFormat::Depth32FloatStencil8, 6);
        auto push = fixture.Quad({{{8, 8}, {56, 8}, {56, 56}, {8, 56}}}, {1, 1, 1, 1});
        push.operation = UiPaintOperation::PushStencil;
        auto rounded = fixture.Quad({{{32, 4}, {60, 32}, {32, 60}, {4, 32}}}, {1, 0, 0, 1});
        rounded.sampling = UiPaintSampling::RoundedRectangle;
        rounded.stencilDepth = 1;
        rounded.maskExtent = {40, 40};
        rounded.cornerRadii = {12, 12, 12, 12};
        auto pop = push;
        pop.operation = UiPaintOperation::PopStencil;
        pop.stencilDepth = 1;
        const std::array draws{push, rounded, pop};
        std::array passes{fixture.Pass(target, draws, stencil)};
        auto commands = [fixture.queue commandBuffer];
        fixture.Encode(commands, passes);
        fixture.Read(commands, target);
        CHECK((fixture.Pixel(32, 32) == std::array<std::uint8_t, 4>{255, 0, 0, 255}));
        CHECK(fixture.Pixel(1, 32)[3] == 0);
        CHECK(fixture.Pixel(32, 5)[3] == 0);
        CHECK(fixture.Pixel(8, 8)[3] == 0);
        CHECK(fixture.Pixel(32, 16)[3] == 255);
        bool fractionalEdge{};
        for (std::size_t y = 8; y < 56; ++y)
            for (std::size_t x = 8; x < 56; ++x) {
                const auto alpha = fixture.Pixel(x, y)[3];
                fractionalEdge = fractionalEdge || (alpha > 0 && alpha < 255);
            }
        CHECK(fractionalEdge);
    }

    TEST_CASE("Cooked native Metal UI isolated opacity and independent coverage multiply once",
              "[integration][renderer][metal][gpu][ui-paint]") {
        PaintSmoke fixture{FixtureRoot()};
        const auto group = fixture.Texture(RenderTextureFormat::Rgba8Unorm, 5);
        const auto coverage = fixture.Texture(RenderTextureFormat::Rgba8Unorm, 6);
        const auto target = fixture.Texture(RenderTextureFormat::Rgba8Unorm, 7);
        const std::array groupDraws{fixture.Quad(Full, {1, 0, 0, 0.5F}), fixture.Quad(Full, {0, 0, 1, 0.5F})};
        const std::array coverageDraws{fixture.Quad({{{0, 0}, {32, 0}, {32, 64}, {0, 64}}}, {1, 1, 1, 0.5F})};
        auto composite = fixture.Quad(Full, {1, 1, 1, 1});
        composite.sampling = UiPaintSampling::Premultiplied;
        composite.sampled = 0;
        composite.coverage = 1;
        composite.opacity = 0.5F;
        const std::array compositeDraws{composite};
        const std::array sampled{group, coverage};
        std::array passes{fixture.Pass(group, groupDraws), fixture.Pass(coverage, coverageDraws),
                          fixture.Pass(target, compositeDraws, {}, sampled)};
        auto commands = [fixture.queue commandBuffer];
        fixture.Encode(commands, passes);
        fixture.Read(commands, target);
        const auto inside = fixture.Pixel(16, 32);
        CHECK(inside[0] >= 15);
        CHECK(inside[0] <= 17);
        CHECK(inside[2] >= 31);
        CHECK(inside[2] <= 33);
        CHECK(inside[3] >= 47);
        CHECK(inside[3] <= 49);
        CHECK(fixture.Pixel(48, 32)[3] == 0);
    }

    TEST_CASE("Cooked native Metal UI nested groups intersect fractional ancestor masks without sibling leakage",
              "[integration][renderer][metal][gpu][ui-paint]") {
        PaintSmoke fixture{FixtureRoot()};
        const auto inner = fixture.Texture(RenderTextureFormat::Rgba8Unorm, 5);
        const auto outer = fixture.Texture(RenderTextureFormat::Rgba8Unorm, 6);
        const auto ancestor = fixture.Texture(RenderTextureFormat::Rgba8Unorm, 7);
        const auto child = fixture.Texture(RenderTextureFormat::Rgba8Unorm, 8);
        const auto target = fixture.Texture(RenderTextureFormat::Rgba8Unorm, 9);
        const std::array innerDraws{fixture.Quad(Full, {1, 0, 0, 0.5F}), fixture.Quad(Full, {0, 0, 1, 0.5F})};
        auto outerDraw = fixture.Quad(Full, {1, 1, 1, 1});
        outerDraw.sampling = UiPaintSampling::Premultiplied;
        outerDraw.sampled = 0;
        outerDraw.opacity = 0.5F;
        const std::array outerDraws{outerDraw};
        const std::array innerTable{inner};
        const std::array ancestorDraws{fixture.Quad({{{0, 0}, {48, 0}, {48, 64}, {0, 64}}}, {1, 1, 1, 0.5F})};
        auto childDraw = fixture.Quad({{{0, 0}, {64, 0}, {64, 48}, {0, 48}}}, {1, 1, 1, 0.5F});
        childDraw.coverage = 0;
        const std::array childDraws{childDraw};
        const std::array ancestorTable{ancestor};
        auto composite = outerDraw;
        composite.coverage = 1;
        const std::array compositeTable{outer, child};
        auto sibling = fixture.Quad({{{52, 52}, {64, 52}, {64, 64}, {52, 64}}}, {0, 1, 0, 1});
        const std::array finalDraws{composite, sibling};
        std::array passes{fixture.Pass(inner, innerDraws), fixture.Pass(outer, outerDraws, {}, innerTable),
                          fixture.Pass(ancestor, ancestorDraws), fixture.Pass(child, childDraws, {}, ancestorTable),
                          fixture.Pass(target, finalDraws, {}, compositeTable)};
        auto commands = [fixture.queue commandBuffer];
        fixture.Encode(commands, passes);
        fixture.Read(commands, target);
        const auto center = fixture.Pixel(24, 24);
        CHECK(center[0] >= 3);
        CHECK(center[0] <= 5);
        CHECK(center[2] >= 7);
        CHECK(center[2] <= 9);
        CHECK(center[3] >= 11);
        CHECK(center[3] <= 13);
        CHECK(fixture.Pixel(56, 24)[3] == 0);
        CHECK(fixture.Pixel(24, 56)[3] == 0);
        CHECK((fixture.Pixel(56, 56) == std::array<std::uint8_t, 4>{0, 255, 0, 255}));
    }

    TEST_CASE("Cooked native Metal UI replacement publication waits for exact completion and rejects the old frame",
              "[integration][renderer][metal][gpu][ui-paint]") {
        PaintSmoke fixture{FixtureRoot()};
        const auto target = fixture.Texture(RenderTextureFormat::Rgba8Unorm, 5);
        const std::array firstDraws{fixture.Quad(Full, {1, 0, 0, 1})};
        std::array first{fixture.Pass(target, firstDraws)};
        auto firstCommands = [fixture.queue commandBuffer];
        fixture.Encode(firstCommands, first);
        auto pending = fixture.runtime.UpdateUiPaintFrame(
            {fixture.buffers, {fixture.revision + 1, fixture.vertices, fixture.indices, fixture.parameters, {}}});
        REQUIRE(pending.HasError());
        CHECK(pending.ErrorValue().code.Value() == UiPaintFrameErrors::Pending.code.Value());
        fixture.Read(firstCommands, target);
        CHECK((fixture.Pixel(32, 32) == std::array<std::uint8_t, 4>{255, 0, 0, 255}));
        fixture.vertices.clear();
        fixture.indices.clear();
        fixture.parameters.clear();
        const std::array secondDraws{fixture.Quad(Full, {0, 1, 0, 1})};
        std::array second{fixture.Pass(target, secondDraws)};
        auto secondCommands = [fixture.queue commandBuffer];
        fixture.Encode(secondCommands, second);
        REQUIRE(fixture.runtime.ValidateGraphWorkload(RenderGraphWorkload{first.front()}, fixture.resources).HasError());
        fixture.Read(secondCommands, target);
        CHECK((fixture.Pixel(32, 32) == std::array<std::uint8_t, 4>{0, 255, 0, 255}));
        fixture.runtime.Shutdown();
        REQUIRE(fixture.runtime.ValidateGraphWorkload(RenderGraphWorkload{second.front()}, fixture.resources).HasError());
    }
}  // namespace Horo::Render::Detail
