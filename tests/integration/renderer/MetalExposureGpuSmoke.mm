#include "ExposureGpuCook.h"
#include "Horo/Runtime/Render/ExposureGpuWorkload.h"
#include "RenderMemoryTestSupport.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"
#include "runtime/renderer/modules/metal/MetalResourceRuntime.h"

#import <Metal/Metal.h>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <thread>
#include <vector>

namespace Horo::Render::Detail {
    namespace {
        using Pixel = std::array<std::uint16_t, 4>;

        /** @brief Preserves typed native/cook diagnostics at every fixture admission. */
        template <typename T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "success"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Loads bounded exact offline artifact bytes without synthesizing their identities. */
        std::vector<std::uint8_t> Read(const std::filesystem::path &path) {
            std::ifstream stream{path, std::ios::binary | std::ios::ate};
            REQUIRE(stream.good());
            const auto size = stream.tellg();
            REQUIRE(size > 0);
            REQUIRE(size <= 16 * 1024 * 1024);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            stream.seekg(0);
            stream.read(reinterpret_cast<char *>(bytes.data()), size);
            REQUIRE(stream.good());
            return bytes;
        }

        /** @brief Decodes actual three-stage compiler records through the canonical reflection boundary. */
        CookedExposureGpuKernel LoadKernel() {
            const char *directory = std::getenv("HORO_EXPOSURE_GPU_PACKAGE");
            if (directory == nullptr)
                SKIP("Supply the genuine offline three-stage HORO_EXPOSURE_GPU_PACKAGE for native GPU qualification.");
            CookedExposureGpuKernel kernel;
            const auto prefixes = Editor::Offline::ExposureGpuCookPrefixes();
            const auto target = Editor::Offline::ExposureGpuMetalCookTarget();
            for (std::size_t index = 0; index < kernel.stages.size(); ++index) {
                auto &stage = kernel.stages[index];
                stage.target = target;
                stage.manifest = MakeExposureGpuShaderManifest(static_cast<ExposureGpuStage>(index), target);
                auto payload = Read(std::filesystem::path{directory} / (std::string{prefixes[index]} + ".horoshdr"));
                const auto metadata = Read(std::filesystem::path{directory} / (std::string{prefixes[index]} + ".reflection.json"));
                auto reflected = Take(DecodeReflectedShaderArtifact(stage.manifest, target, payload, metadata));
                stage.artifact.backend = target.backend;
                stage.artifact.payloadFormat = target.payloadFormat;
                stage.artifact.artifactKey = reflected.compiledArtifactKey;
                stage.artifact.payload = std::move(payload);
                stage.reflection = {reflected.reflection.backend,        reflected.reflection.interfaceSchemaVersion,
                                    reflected.reflection.bindings,       reflected.reflection.parameters,
                                    reflected.reflection.stageInterface, reflected.reflection.targetBindings,
                                    reflected.reflection.sourceMap};
                stage.artifact.reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(reflected));
            }
            REQUIRE(ValidateCookedExposureGpuKernel(kernel, ShaderTargetBackend::Metal).HasValue());
            return kernel;
        }

        /** @brief Uses exact half-float source bits; powers, NaNs and adjacent boundary values need no conversion approximation. */
        Pixel Grey(const std::uint16_t half) {
            return {half, half, half, 0x3c00};
        }

        /** @brief Contains completion polling to the explicit native test deadline, never a normal render path. */
        void Completed(id<MTLCommandBuffer> commands) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            REQUIRE(commands.status == MTLCommandBufferStatusCompleted);
        }

        /** @brief Resolves actual recorded commands even when a fixture assertion unwinds before submission. */
        struct RecordedCommand final {
            MetalResourceRuntime &runtime;
            __strong id<MTLCommandBuffer> commands;
            bool resolved{};

            /** @brief Retains the actual native recording and its sole resource owner. */
            RecordedCommand(MetalResourceRuntime &runtime, id<MTLCommandBuffer> commands) : runtime(runtime), commands(commands) {}

            /** @brief Releases unsent native-use evidence without pretending GPU completion. */
            ~RecordedCommand() {
                if (!resolved)
                    runtime.FinishGraphCommands((__bridge void *)commands, false);
            }

            /** @brief Marks acceptance only after actual native queue submission. */
            void Commit() {
                [commands commit];
                resolved = true;
                runtime.FinishGraphCommands((__bridge void *)commands, true);
            }

            /** @brief Explicitly abandons an unsubmitted recording before lifecycle assertions. */
            void Abandon() {
                runtime.FinishGraphCommands((__bridge void *)commands, false);
                resolved = true;
            }
        };

        /** @brief Owns only test readback copies of actual completed GPU output. */
        struct Readback final {
            ExposureGpuState state;
            std::array<std::uint32_t, ExposureHistogramBins> histogram{};
        };

        /** @brief Exercises the private concrete native owner; public frontend graph-import admission is a separate qualification. */
        struct NativeFixture final {
            __strong id<MTLDevice> device{MTLCreateSystemDefaultDevice()};
            __strong id<MTLCommandQueue> queue;
            MetalResourceRuntime runtime;
            std::shared_ptr<IResidentExposureGpuKernel> kernel;
            std::uint64_t incarnation{};
            std::array<RenderGraphResourceInstance, 4> resources{};
            std::array<RenderBufferHandle, 2> handles{{{{1}, 1, 1}, {{1}, 2, 1}}};
            FramebufferExtent extent;
            std::uint64_t texture{};
            std::uint64_t nextPlacement{1};
            std::uint32_t nextHandle{3};

            /** @brief Initializes actual device/queue identity and finite native storage without a presentation window. */
            explicit NativeFixture(const CookedExposureGpuKernel &cooked) {
                REQUIRE(device != nil);
                queue = [device newCommandQueue];
                REQUIRE(queue != nil);
                incarnation = Take(AcquireRenderDeviceIncarnation());
                runtime.Initialize((__bridge void *)device, (__bridge void *)queue, incarnation);
                kernel = Take(runtime.RealizeExposureGpuKernel(cooked));
                resources[0].resource = {{1}, 1};
                for (std::size_t index = 1; index < resources.size(); ++index) {
                    const auto size = index == 1 ? ExposureHistogramBins * sizeof(std::uint32_t) : sizeof(ExposureGpuState);
                    const RenderBufferDescriptor descriptor{.byteSize = size,
                                                            .usage = RenderBufferUsage::Storage | RenderBufferUsage::CopySource |
                                                                     RenderBufferUsage::CopyDestination,
                                                            .access = RenderBufferAccess::DeviceLocal};
                    const auto cost = Take(runtime.QueryBufferMemoryCost(descriptor));
                    const auto placement = nextPlacement++;
                    resources[index] = {{{1}, static_cast<std::uint32_t>(index + 1)},
                                        Take(runtime.CreateBuffer(descriptor, {}, TestSupport::PlacementFor(cost, placement, placement)))};
                }
            }

            /** @brief Allocates real fresh native state storage instead of overwriting accepted history on reset. */
            std::uint64_t Buffer(std::size_t bytes) {
                const RenderBufferDescriptor descriptor{.byteSize = bytes,
                                                        .usage = RenderBufferUsage::Storage | RenderBufferUsage::CopySource |
                                                                 RenderBufferUsage::CopyDestination,
                                                        .access = RenderBufferAccess::DeviceLocal};
                const auto cost = Take(runtime.QueryBufferMemoryCost(descriptor));
                const auto placement = nextPlacement++;
                return Take(runtime.CreateBuffer(descriptor, {}, TestSupport::PlacementFor(cost, placement, placement)));
            }

            /** @brief Starts an explicit reset with genuinely fresh seed/output buffers; old accepted storage stays intact. */
            void FreshStates() {
                resources[2].instance = Buffer(sizeof(ExposureGpuState));
                resources[3].instance = Buffer(sizeof(ExposureGpuState));
                handles[0] = {{1}, nextHandle++, 1};
                handles[1] = {{1}, nextHandle++, 1};
            }

            /** @brief Releases native resources only after every submitted test command has reached actual completion. */
            ~NativeFixture() {
                runtime.Shutdown();
            }

            /** @brief Allocates a distinct real scene texture without retiring another view's tracked source. */
            std::uint64_t Texture(std::span<const Pixel> pixels, FramebufferExtent size) {
                REQUIRE(pixels.size() == static_cast<std::size_t>(size.width) * size.height);
                const RenderTextureDescriptor descriptor{.extent = size,
                                                         .format = RenderTextureFormat::Rgba16Float,
                                                         .usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination |
                                                                  RenderTextureUsage::RenderAttachment};
                const auto cost = Take(runtime.QueryTextureMemoryCost(descriptor));
                const auto placement = nextPlacement++;
                return Take(
                    runtime.CreateTexture(descriptor, std::as_bytes(pixels), TestSupport::PlacementFor(cost, placement, placement)));
            }

            /** @brief Uploads caller-owned synthetic scene samples through the actual native texture resource path. */
            void Source(std::span<const Pixel> pixels, FramebufferExtent size) {
                const auto created = Texture(pixels, size);
                if (texture != 0)
                    runtime.DestroyTexture(texture);
                texture = created;
                extent = size;
                resources[0].instance = texture;
            }

            /** @brief Builds closed logical metadata; native identity is still checked against real resources and submission receipts. */
            RenderGraphExposureGpu Work(const ExposureSettings &settings, std::uint64_t submission = 1, std::uint64_t generation = 1,
                                        const ExposureState *previous = nullptr, std::uint64_t settingsGeneration = 1) const {
                const auto dispatch = Take(PrepareExposureDispatch(settings, extent, {0, 0, extent.width, extent.height}, 1.0F));
                ExposureState produced{.mode = ExposureMode::AutomaticHistogram,
                                       .view = {394},
                                       .generation = generation,
                                       .settingsGeneration = settingsGeneration,
                                       .deviceGeneration = incarnation,
                                       .sourceSubmission = submission,
                                       .consumeSubmission = submission + 1,
                                       .buffer = handles[1]};
                return {resources[0].resource,
                        resources[1].resource,
                        resources[2].resource,
                        resources[3].resource,
                        extent,
                        {dispatch,
                         {static_cast<std::uint32_t>(generation), static_cast<std::uint32_t>(generation >> 32U),
                          static_cast<std::uint32_t>(submission), static_cast<std::uint32_t>(submission >> 32U)}},
                        previous ? *previous : ExposureState{},
                        produced,
                        previous == nullptr,
                        kernel};
            }

            /** @brief Exchanges actual buffers and their logical imported metadata for next-submission consumption. */
            void Swap() {
                std::swap(resources[2].instance, resources[3].instance);
                std::swap(handles[0], handles[1]);
            }

            /** @brief Appends real device-to-shared blits in the same queue command after all three exposure stages. */
            id<MTLBuffer> Copy(id<MTLCommandBuffer> commands) const {
                id<MTLBuffer> readback = [device newBufferWithLength:sizeof(Readback) options:MTLResourceStorageModeShared];
                REQUIRE(readback != nil);
                const auto *histogram = reinterpret_cast<const MetalBufferInstance *>(static_cast<std::uintptr_t>(resources[1].instance));
                const auto *output = reinterpret_cast<const MetalBufferInstance *>(static_cast<std::uintptr_t>(resources[3].instance));
                id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                REQUIRE(blit != nil);
                [blit copyFromBuffer:output->buffer sourceOffset:0 toBuffer:readback destinationOffset:0 size:sizeof(ExposureGpuState)];
                [blit copyFromBuffer:histogram->buffer
                         sourceOffset:0
                             toBuffer:readback
                    destinationOffset:sizeof(ExposureGpuState)
                                 size:ExposureHistogramBins * sizeof(std::uint32_t)];
                [blit endEncoding];
                return readback;
            }

            /** @brief Test-only GPU blit corrupts actual prior bytes after completion while retaining the genuine native journal receipt.
             */
            void InvalidPrevious(ExposureGpuState previous) const {
                previous.reason = static_cast<std::uint32_t>(ExposureSampleReason::Invalid);
                id<MTLBuffer> staging = [device newBufferWithBytes:&previous length:sizeof(previous) options:MTLResourceStorageModeShared];
                REQUIRE(staging != nil);
                const auto *target = reinterpret_cast<const MetalBufferInstance *>(static_cast<std::uintptr_t>(resources[2].instance));
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                REQUIRE(commands != nil);
                id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                REQUIRE(blit != nil);
                [blit copyFromBuffer:staging sourceOffset:0 toBuffer:target->buffer destinationOffset:0 size:sizeof(previous)];
                [blit endEncoding];
                [commands commit];
                Completed(commands);
            }

            /** @brief Publishes journal acceptance only after actual native commit and reads only after terminal completion. */
            Readback Submit(const RenderGraphExposureGpu &work) {
                REQUIRE(runtime.ValidateExposureGpu(work, resources).HasValue());
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                REQUIRE(commands != nil);
                RecordedCommand recording{runtime, commands};
                REQUIRE(runtime.ExecuteExposureGpu((__bridge void *)commands, work, resources).HasValue());
                id<MTLBuffer> readback = Copy(commands);
                recording.Commit();
                Completed(commands);
                Readback result;
                std::memcpy(&result, readback.contents, sizeof(result));
                return result;
            }
        };

        /** @brief Checks state arithmetic and exact shader-written metadata independently of host diagnostic fields. */
        void CheckState(const Readback &read, float ev, std::uint32_t samples, ExposureSampleReason reason) {
            CHECK(read.state.exposureEv == Catch::Approx(ev).margin(0.002F));
            CHECK(read.state.exposureScale == Catch::Approx(std::exp2(ev)).epsilon(0.003F));
            CHECK(std::isfinite(read.state.exposureScale));
            CHECK(read.state.exposureScale > 0.0F);
            CHECK(read.state.validSamples == samples);
            CHECK(read.state.reason == static_cast<std::uint32_t>(reason));
            CHECK(std::accumulate(read.histogram.begin(), read.histogram.end(), std::uint32_t{}) == samples);
        }

        /** @brief Requires a typed native provenance rejection without accepting an alternate CPU path. */
        void Rejected(Result<void> result) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == ExposureGpuErrors::InvalidInput.domain.Value());
            CHECK(result.ErrorValue().code.Value() == ExposureGpuErrors::InvalidInput.code.Value());
        }
    }  // namespace

    TEST_CASE("Offline-cooked Metal exposure meters gray and consumes actual N GPU state in N plus one", "[metal][gpu][exposure]") {
        const auto cooked = LoadKernel();
        NativeFixture frame{cooked};
        std::array<Pixel, 128> pixels;
        pixels.fill(Grey(0x31c3));  // 0.1800537109375, the nearest float16 gray.
        frame.Source(pixels, {16, 8});
        auto work = frame.Work({.mode = ExposureMode::AutomaticHistogram});
        auto gray = frame.Submit(work);
        CHECK(std::abs(gray.state.exposureEv) < 0.063F);
        CHECK(gray.state.validSamples == pixels.size());
        CHECK(gray.histogram[108] == pixels.size());
        NativeFixture adapted{cooked};
        pixels.fill(Grey(0x2400));  // 1/64.
        adapted.Source(pixels, {16, 8});
        constexpr std::uint64_t generation = 0x100000001ULL;
        constexpr std::uint64_t firstSubmission = 0xffffffffULL;
        auto first = adapted.Work({.mode = ExposureMode::AutomaticHistogram}, firstSubmission, generation);
        CheckState(adapted.Submit(first), 3.0F, 128, ExposureSampleReason::Valid);
        adapted.Swap();
        pixels.fill(Grey(0x4400));  // 4.0.
        adapted.Source(pixels, {16, 8});
        const auto bright =
            adapted.Submit(adapted.Work({.mode = ExposureMode::AutomaticHistogram}, firstSubmission + 1, generation + 1, &first.produced));
        CheckState(bright, 2.0F, 128, ExposureSampleReason::Valid);
        CHECK(bright.state.sourceSubmissionLo == 0);
        CHECK(bright.state.sourceSubmissionHi == 1);
        CHECK(bright.state.generationLo == 2);
        CHECK(bright.state.generationHi == 1);
    }

    TEST_CASE("Metal exposure bright scene uses negative EV and bounded darkening", "[metal][gpu][exposure]") {
        NativeFixture frame{LoadKernel()};
        const std::array<Pixel, 1> pixels{Grey(0x4400)};
        frame.Source(pixels, {1, 1});
        CheckState(frame.Submit(frame.Work({.mode = ExposureMode::AutomaticHistogram})), -1.0F, 1, ExposureSampleReason::Valid);
    }

    TEST_CASE("Metal exposure counts finite boundary samples and excludes nonfinite RGBA", "[metal][gpu][exposure]") {
        NativeFixture frame{LoadKernel()};
        const std::array<Pixel, 11> pixels{Grey(0),
                                           Grey(0xbc00),
                                           Grey(0x0100),
                                           Grey(0x3bff),
                                           Grey(0x3c01),
                                           Grey(0x7bff),
                                           Grey(0x7e00),
                                           Pixel{0, 0, 0, 0x7e00},
                                           Pixel{0x3c00, 0, 0, 0x3c00},
                                           Pixel{0, 0x3c00, 0, 0x3c00},
                                           Pixel{0, 0, 0x3c00, 0x3c00}};
        frame.Source(pixels, {11, 1});
        const auto result = frame.Submit(frame.Work({.mode = ExposureMode::AutomaticHistogram}));
        CHECK(result.state.validSamples == 9);
        CHECK(result.histogram[0] == 3);
        CHECK(result.histogram[127] == 1);
        CHECK(result.histogram[128] == 1);
        CHECK(result.histogram[255] == 1);
        // Actual colored AP1 primaries distinguish canonical luminance from Rec.709 or RGB averaging.
        CHECK(result.histogram[112] == 1);
        CHECK(result.histogram[123] == 1);
        CHECK(result.histogram[94] == 1);
    }

    TEST_CASE("Metal exposure percentile prefix uses fractional weights and positive EV increases exposure", "[metal][gpu][exposure]") {
        NativeFixture frame{LoadKernel()};
        std::array<Pixel, 16> pixels;
        constexpr std::array<std::uint16_t, 4> values{0x31c3, 0x35c3, 0x39c3, 0x3dc3};
        for (std::size_t index = 0; index < pixels.size(); ++index)
            pixels[index] = Grey(values[index / 4]);
        frame.Source(pixels, {16, 1});
        ExposureSettings settings{.mode = ExposureMode::AutomaticHistogram};
        settings.lowPercentile = 0.125F;
        settings.highPercentile = 0.625F;
        settings.compensationEv = 2.0F;
        settings.brightenEvPerSecond = 64;
        settings.darkenEvPerSecond = 64;
        const auto result = frame.Submit(frame.Work(settings));
        // Weights2,4,2 at bins108,116,124: weighted meanlog=-1.4375.
        const auto expected = std::log2(0.18F) + 1.4375F + 2.0F;
        CheckState(result, expected, 16, ExposureSampleReason::Valid);
        CHECK(result.state.exposureScale > 1.0F);
    }

    TEST_CASE("Metal exposure empty input obeys initial and genuine previous recovery policies", "[metal][gpu][exposure]") {
        NativeFixture frame{LoadKernel()};
        std::array<Pixel, 8> pixels;
        pixels.fill(Grey(0x2400));
        frame.Source(pixels, {8, 1});
        ExposureSettings settings{.mode = ExposureMode::AutomaticHistogram, .reset = ExposureResetPolicy::RetainLastValid};
        auto work = frame.Work(settings);
        CheckState(frame.Submit(work), 3.0F, 8, ExposureSampleReason::Valid);
        frame.Swap();
        pixels.fill(Grey(0x7e00));
        frame.Source(pixels, {8, 1});
        CheckState(frame.Submit(frame.Work(settings, 2, 2, &work.produced)), 3.0F, 0, ExposureSampleReason::Empty);
        frame.FreshStates();
        settings.reset = ExposureResetPolicy::InitialValue;
        settings.initialEv = 1;
        settings.compensationEv = 0.5F;
        CheckState(frame.Submit(frame.Work(settings, 3, 3, nullptr, 2)), 1.5F, 0, ExposureSampleReason::Empty);
    }

    TEST_CASE("Metal exposure rejects uncommitted abandoned and reset predecessor evidence", "[metal][gpu][exposure][lifecycle]") {
        NativeFixture frame{LoadKernel()};
        const std::array<Pixel, 1> pixels{Grey(0x31c3)};
        frame.Source(pixels, {1, 1});
        const auto first = frame.Work({.mode = ExposureMode::AutomaticHistogram});
        id<MTLCommandBuffer> commands = [frame.queue commandBuffer];
        RecordedCommand recording{frame.runtime, commands};
        REQUIRE(frame.runtime.ExecuteExposureGpu((__bridge void *)commands, first, frame.resources).HasValue());
        frame.Swap();
        auto next = frame.Work({.mode = ExposureMode::AutomaticHistogram}, 2, 2, &first.produced);
        auto uncommitted = frame.runtime.ValidateExposureGpu(next, frame.resources);
        auto sameRecording = frame.runtime.ExecuteGraphWorkload((__bridge void *)commands, RenderGraphWorkload{next}, frame.resources);
        recording.Abandon();
        commands = nil;
        Rejected(std::move(uncommitted));
        Rejected(std::move(sameRecording));
        Rejected(frame.runtime.ValidateExposureGpu(next, frame.resources));
        frame.Swap();
        frame.Submit(first);
        const auto old = first.produced;
        const auto oldBuffer = frame.resources[3].instance;
        frame.FreshStates();
        frame.Submit(frame.Work({.mode = ExposureMode::AutomaticHistogram}, 2, 2));
        frame.resources[2].instance = oldBuffer;
        frame.resources[3].instance = frame.Buffer(sizeof(ExposureGpuState));
        Rejected(frame.runtime.ValidateExposureGpu(frame.Work({.mode = ExposureMode::AutomaticHistogram}, 2, 3, &old), frame.resources));
    }

    TEST_CASE("Metal scene attachment write feeds exposure in the same actual native recording", "[metal][gpu][exposure][graph]") {
        NativeFixture frame{LoadKernel()};
        const std::array<Pixel, 1> pixels{Grey(0x4400)};
        frame.Source(pixels, {1, 1});
        const auto work = frame.Work({.mode = ExposureMode::AutomaticHistogram});
        id<MTLCommandBuffer> commands = [frame.queue commandBuffer];
        REQUIRE(commands != nil);
        RecordedCommand recording{frame.runtime, commands};
        const RenderGraphWorkload clear =
            RenderGraphColorAttachment{frame.resources[0].resource, {.clearColor = {0.18F, 0.18F, 0.18F, 1.0F}}};
        REQUIRE(frame.runtime.ExecuteGraphWorkload((__bridge void *)commands, clear, frame.resources).HasValue());
        // The generic dispatcher must preserve the authentic current-command context through exposure validation.
        REQUIRE(frame.runtime.ExecuteGraphWorkload((__bridge void *)commands, RenderGraphWorkload{work}, frame.resources).HasValue());
        id<MTLBuffer> readback = frame.Copy(commands);
        recording.Commit();
        Completed(commands);
        Readback result;
        std::memcpy(&result, readback.contents, sizeof(result));
        CheckState(result, std::log2(0.18F) + 2.4375F, 1, ExposureSampleReason::Valid);
        CHECK(result.histogram[108] == 1);
    }

    TEST_CASE("Metal exposure rejects a scene write owned by a different unsubmitted command", "[metal][gpu][exposure][graph]") {
        NativeFixture frame{LoadKernel()};
        const std::array<Pixel, 1> pixels{Grey(0x2400)};
        frame.Source(pixels, {1, 1});
        const auto work = frame.Work({.mode = ExposureMode::AutomaticHistogram});
        id<MTLCommandBuffer> source = [frame.queue commandBuffer];
        id<MTLCommandBuffer> other = [frame.queue commandBuffer];
        REQUIRE(source != nil);
        REQUIRE(other != nil);
        RecordedCommand sourceRecording{frame.runtime, source}, otherRecording{frame.runtime, other};
        const RenderGraphWorkload clear =
            RenderGraphColorAttachment{frame.resources[0].resource, {.clearColor = {0.18F, 0.18F, 0.18F, 1.0F}}};
        REQUIRE(frame.runtime.ExecuteGraphWorkload((__bridge void *)source, clear, frame.resources).HasValue());
        auto denied = frame.runtime.ExecuteGraphWorkload((__bridge void *)other, RenderGraphWorkload{work}, frame.resources);
        otherRecording.Abandon();
        sourceRecording.Abandon();
        Rejected(std::move(denied));
        // Discarding the unsent clear preserves the actual originally uploaded dark pixels.
        CheckState(frame.Submit(work), 3.0F, 1, ExposureSampleReason::Valid);
    }

    TEST_CASE("Metal exposure abandonment preserves accepted predecessor across independent view pruning",
              "[metal][gpu][exposure][lifecycle]") {
        NativeFixture frame{LoadKernel()};
        const std::array<Pixel, 1> pixels{Grey(0x31c3)};
        frame.Source(pixels, {1, 1});
        const ExposureSettings settings{.mode = ExposureMode::AutomaticHistogram};
        const auto accepted = frame.Work(settings);
        frame.Submit(accepted);
        frame.Swap();
        const auto candidate = frame.Work(settings, 2, 2, &accepted.produced);
        id<MTLCommandBuffer> commands = [frame.queue commandBuffer];
        REQUIRE(commands != nil);
        RecordedCommand pending{frame.runtime, commands};
        REQUIRE(frame.runtime.ExecuteExposureGpu((__bridge void *)commands, candidate, frame.resources).HasValue());
        const auto retainedResources = frame.resources;
        const auto retainedHandles = frame.handles;
        // Every independent-view resource is real and distinct; B's unsent scene texture must not block the prune path.
        frame.resources[0].instance = frame.Texture(pixels, {1, 1});
        frame.resources[1].instance = frame.Buffer(ExposureHistogramBins * sizeof(std::uint32_t));
        frame.FreshStates();
        // Empty creation issued no GPU upload, regardless of whether the unrelated texture upload has already completed.
        for (std::size_t index = 1; index < frame.resources.size(); ++index) {
            CAPTURE(index);
            const auto *created =
                reinterpret_cast<const MetalBufferInstance *>(static_cast<std::uintptr_t>(frame.resources[index].instance));
            CHECK(created->use.last == nil);
        }
        auto independent = frame.Work(settings);
        independent.produced.view = {395};
        REQUIRE(frame.runtime.ValidateExposureGpu(independent, frame.resources).HasValue());
        pending.Abandon();
        frame.resources = retainedResources;
        frame.handles = retainedHandles;
        const auto retried = frame.Work(settings, 2, 3, &accepted.produced);
        CheckState(frame.Submit(retried), std::log2(0.18F) + 2.4375F, 1, ExposureSampleReason::Valid);
    }

    TEST_CASE("Completed Metal exposure commands require genuine owner submission acceptance before next use",
              "[metal][gpu][exposure][lifecycle]") {
        NativeFixture frame{LoadKernel()};
        const std::array<Pixel, 1> pixels{Grey(0x31c3)};
        frame.Source(pixels, {1, 1});
        const auto first = frame.Work({.mode = ExposureMode::AutomaticHistogram});
        id<MTLCommandBuffer> commands = [frame.queue commandBuffer];
        REQUIRE(commands != nil);
        REQUIRE(frame.runtime.ExecuteExposureGpu((__bridge void *)commands, first, frame.resources).HasValue());
        [commands commit];
        Completed(commands);
        frame.Swap();
        const auto next = frame.Work({.mode = ExposureMode::AutomaticHistogram}, 2, 2, &first.produced);
        auto unaccepted = frame.runtime.ValidateExposureGpu(next, frame.resources);
        // The delayed owning callback observes an actual completed command; it never invents native completion.
        frame.runtime.FinishGraphCommands((__bridge void *)commands, true);
        Rejected(std::move(unaccepted));
        REQUIRE(frame.runtime.ValidateExposureGpu(next, frame.resources).HasValue());
    }

    TEST_CASE("Metal exposure propagates actual invalid GPU predecessor reason until explicit reset", "[metal][gpu][exposure][lifecycle]") {
        NativeFixture frame{LoadKernel()};
        std::array<Pixel, 8> pixels;
        pixels.fill(Grey(0x31c3));
        frame.Source(pixels, {8, 1});
        const ExposureSettings settings{.mode = ExposureMode::AutomaticHistogram};
        auto work = frame.Work(settings);
        const auto first = frame.Submit(work);
        frame.Swap();
        frame.InvalidPrevious(first.state);
        work = frame.Work(settings, 2, 2, &work.produced);
        const auto invalid = frame.Submit(work);
        CHECK(invalid.state.reason == static_cast<std::uint32_t>(ExposureSampleReason::Invalid));
        CHECK(invalid.state.validSamples == 0);
        CHECK(invalid.state.exposureEv == 0.0F);
        CHECK(std::accumulate(invalid.histogram.begin(), invalid.histogram.end(), std::uint32_t{}) == 8);
        frame.Swap();
        work = frame.Work(settings, 3, 3, &work.produced);
        const auto propagated = frame.Submit(work);
        CHECK(propagated.state.reason == static_cast<std::uint32_t>(ExposureSampleReason::Invalid));
        CHECK(propagated.state.validSamples == 0);
        frame.FreshStates();
        CheckState(frame.Submit(frame.Work(settings, 4, 4)), std::log2(0.18F) + 2.4375F, 8, ExposureSampleReason::Valid);
    }

    TEST_CASE("Metal exposure denies foreign kernels and stale native runtime resources", "[metal][gpu][exposure][lifecycle]") {
        const auto cooked = LoadKernel();
        NativeFixture first{cooked}, second{cooked};
        const std::array<Pixel, 1> pixels{Grey(0x31c3)};
        first.Source(pixels, {1, 1});
        second.Source(pixels, {1, 1});
        auto foreign = second.Work({.mode = ExposureMode::AutomaticHistogram});
        foreign.kernel = first.kernel;
        Rejected(second.runtime.ValidateExposureGpu(foreign, second.resources));
        auto stale = first.Work({.mode = ExposureMode::AutomaticHistogram});
        first.runtime.Shutdown();
        Rejected(first.runtime.ValidateExposureGpu(stale, first.resources));
    }
}  // namespace Horo::Render::Detail
