#include "Horo/Runtime/Render/NativeReconstructionViewHistory.h"
#include "RenderFrontendTestSupport.h"
#include "support/renderer/RenderBackendDrainTestSupport.h"

#include <array>
#include <utility>

namespace Horo::Tests::RenderFrontendTesting {
    namespace {
        /** @brief Marker for the portable frontend admission path; fixtures do not claim executed pixels. */
        class HistoryKernel final : public IResidentNativeReconstructionKernel {};

        /** @brief Retains the normal simulated native resource owner through drain even if a test assertion unwinds. */
        struct HistoryFrontend final {
            const std::array<RenderQueueAssignment, 1> assignments{RenderQueueAssignment{RenderQueueRole::Compute, {1}}};
            std::unique_ptr<RenderFrontend> frontend;

            HistoryFrontend() {
                lifecycleState = {};
                lifecycleState.reconstruction.enabled = true;
                lifecycleState.supportsSubmissionObservation = true;
                RenderCapabilitySnapshot capabilities{.capabilityRevision = 1};
                for (const auto feature :
                     {RenderCapability::TextureResources, RenderCapability::NativeReconstruction, RenderCapability::Compute})
                    capabilities.features.Enable(feature);
                capabilities.queues.compute = true;
                capabilities.limits.maxTextureDimension2D = 32;
                capabilities.formats.sampleCountMask = 1 << 1;
                capabilities.formats.usages.fill(RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment |
                                                 RenderTextureUsage::Storage);
                lifecycleState.syntheticCapabilities = capabilities;
                RenderBackendRegistry registry;
                RegisterTrackingBackend(registry);
                REQUIRE(registry.Seal().HasValue());
                RenderBackendConfig config;
                config.queueAssignments.assign(assignments.begin(), assignments.end());
                frontend = Take(RenderFrontend::Create(registry, RenderBackendId{"tracking"}, config));
            }

            ~HistoryFrontend() {
                lifecycleState.drainPending = false;
                Test::RequireDrainedOwner(*frontend);
            }

            template <typename T> static T Take(Result<T> result) {
                INFO((result.HasError() ? result.ErrorValue().message : "accepted"));
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }
        };

        constexpr FramebufferExtent Extent{16, 16};

        /** @brief Creates an exact persistent texture and sampled view using the tested frontend resource registry. */
        NativeReconstructionSceneImage MakeImage(RenderFrontend &frontend, RenderTextureFormat format, RenderTextureUsage usage) {
            const auto texture = HistoryFrontend::Take(frontend.CreateTexture({.extent = Extent, .format = format, .usage = usage})).handle;
            REQUIRE(frontend.ProcessResourceRequests().HasValue());
            const auto view = HistoryFrontend::Take(frontend.CreateTextureView({.texture = texture, .format = format})).handle;
            REQUIRE(frontend.ProcessResourceRequests().HasValue());
            return {texture, view};
        }

        /** @brief Freezes the selected software device/scene epoch for this one real-frame history test. */
        TemporalHistoryCompatibility Compatibility(RenderFrontend &frontend) {
            return {.view = HistoryFrontend::Take(frontend.CreateRenderViewIdentity()),
                    .provider = {1},
                    .mode = {1},
                    .renderExtent = Extent,
                    .targetExtent = Extent,
                    .providerGeneration = 1,
                    .modeGeneration = 1,
                    .surfaceGeneration = 1,
                    .rasterGeneration = 1,
                    .colorGeneration = 1,
                    .exposureGeneration = 1,
                    .inputSchemaGeneration = 1,
                    .deviceGeneration = frontend.Capabilities().support.deviceIncarnation,
                    .projectionGeneration = 1,
                    .jitterGeneration = 1,
                    .sceneOriginGeneration = 1,
                    .motionGeneration = 1,
                    .recipeGeneration = 1};
        }

        /** @brief Builds an exact seed batch followed by a separate read of the history color, changing its final state. */
        void RecordHistory(RenderFrameScope &frame, const HistoryFrontend &owner, const NativeReconstructionScenePlan &plan,
                           const std::shared_ptr<NativeReconstructionViewHistory> &history, NativeReconstructionSceneImage laterOutput) {
            using enum NativeReconstructionImage;
            const auto take = []<typename T>(Result<T> result) {
                return HistoryFrontend::Take(std::move(result));
            };
            auto builder = take(RenderGraphBuilder::Create({.maxPasses = 2, .maxResources = 5, .maxUsages = 6, .maxDependencies = 1}));
            const auto seed = take(builder.AddPass(RenderPassKind::Compute, RenderQueueRole::Compute));
            const auto reader = take(builder.AddPass(RenderPassKind::Compute, RenderQueueRole::Compute));
            REQUIRE(builder.AddDependency({seed, reader, RenderGraphDependencyKind::ExecutionOrder}).HasValue());

            constexpr RenderGraphLogicalState sampled{RenderGraphSynchronizationAccess::Read,
                                                      RenderGraphSynchronizationOperation::Sampled,
                                                      RenderGraphPipelineScope::Compute,
                                                      RenderGraphTextureLayout::ShaderReadOnly,
                                                      {1}};
            const auto kernel = std::make_shared<HistoryKernel>();
            RenderGraphNativeReconstruction first{.dispatch = plan.dispatch, .kernel = kernel};
            std::vector<RenderGraphImportedState> initial;
            for (const auto role : {CurrentColor, OutputColor, CurrentDepth, OutputDepth}) {
                const auto index = static_cast<std::size_t>(role);
                const auto image = plan.images[index];
                const bool output = NativeReconstructionWritesImage(role);
                const auto id = take(builder.ImportTexture(image.texture, output ? RenderGraphResourceClass::History
                                                                                 : RenderGraphResourceClass::Persistent));
                first.images[index] = {id, image.view};
                initial.push_back({id, output ? RenderGraphLogicalState{} : sampled,
                                   output ? RenderGraphInitialContents::Discarded : RenderGraphInitialContents::Preserved});
                REQUIRE(builder
                            .AddUsage({seed, id, output ? RenderGraphAccess::Write : RenderGraphAccess::Read,
                                       output ? RenderGraphUsageKind::Storage : RenderGraphUsageKind::Sampled})
                            .HasValue());
            }
            const auto source = first.images[static_cast<std::size_t>(OutputColor)];
            const auto destination = take(builder.ImportTexture(laterOutput.texture, RenderGraphResourceClass::Persistent));
            initial.push_back({destination, {}, RenderGraphInitialContents::Discarded});
            REQUIRE(builder.AddUsage({reader, source.resource, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled}).HasValue());
            REQUIRE(builder.AddUsage({reader, destination, RenderGraphAccess::Write, RenderGraphUsageKind::Storage}).HasValue());
            RenderGraphNativeReconstruction second{.dispatch = {NativeReconstructionEntry::Spatial, plan.dispatch.constants},
                                                   .kernel = kernel};
            second.images[static_cast<std::size_t>(CurrentColor)] = source;
            second.images[static_cast<std::size_t>(OutputColor)] = {destination, laterOutput.view};

            const auto graph = take(builder.Finalize());
            const auto schedule = take(CompileRenderGraph(graph));
            const auto sync = take(SynthesizeRenderGraphSynchronization(graph, schedule, owner.assignments, initial));
            const auto compiled = take(CompileRenderGraphExecution(graph, schedule, sync, owner.assignments));
            const std::array work{RenderGraphPassWorkload{seed, first}, RenderGraphPassWorkload{reader, second}};
            const std::array submissions{RenderGraphQueueSubmission{take(frame.ReserveSubmission(RenderQueueRole::Compute)), {0, 1}},
                                         RenderGraphQueueSubmission{take(frame.ReserveSubmission(RenderQueueRole::Compute)), {1, 1}}};
            const std::array histories{history};
            REQUIRE(frame.ExecuteNativeReconstructionScene(compiled, work, submissions, histories).HasValue());
        }
    }  // namespace

    TEST_CASE("Reconstruction history admits an accepted scene tail independently of presentation",
              "[renderer][reconstruction][submission]") {
        HistoryFrontend owner;
        auto &frontend = *owner.frontend;
        constexpr auto sampled = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
        constexpr auto storage = RenderTextureUsage::Sampled | RenderTextureUsage::Storage;
        const auto color = MakeImage(frontend, RenderTextureFormat::Rgba16Float, sampled);
        const auto depth = MakeImage(frontend, RenderTextureFormat::R32Float, sampled);
        const auto motion = MakeImage(frontend, RenderTextureFormat::Rg16Float, sampled);
        const auto later = MakeImage(frontend, RenderTextureFormat::Rgba32Float, storage);
        const auto pair = [&] {
            return NativeReconstructionHistoryImages{MakeImage(frontend, RenderTextureFormat::Rgba32Float, storage),
                                                     MakeImage(frontend, RenderTextureFormat::R32Float, storage)};
        };
        const NativeReconstructionViewHistory::ImagePairs pairs{pair(), pair()};
        const auto epoch = Compatibility(frontend);
        const auto history = HistoryFrontend::Take(NativeReconstructionViewHistory::Create(frontend, epoch, pairs));
        const auto input = [&](NativeReconstructionSceneImage image, RenderTextureFormat format) {
            return ReconstructionTextureInput{image.texture, {.extent = Extent, .format = format, .usage = sampled}, epoch.view, 1};
        };
        const ReconstructionFrameRequest request{.compatibility = epoch,
                                                 .resourceOwner = color.texture.owner,
                                                 .timing = {.frameId = 1},
                                                 .color = input(color, RenderTextureFormat::Rgba16Float),
                                                 .positiveLinearDepth = input(depth, RenderTextureFormat::R32Float),
                                                 .motion = input(motion, RenderTextureFormat::Rg16Float),
                                                 .output = {.descriptor = {.extent = Extent,
                                                                           .format = RenderTextureFormat::Rgba32Float,
                                                                           .usage = storage},
                                                            .writeUsage = RenderGraphUsageKind::Storage}};
        const auto plan = HistoryFrontend::Take(history->Stage(frontend, request, {color.view, depth.view, motion.view}, {}));
        auto frame = HistoryFrontend::Take(frontend.BeginFrame({.frameNumber = 1, .outputExtent = Extent}));
        RecordHistory(frame, owner, plan, history, later);
        REQUIRE(lifecycleState.reconstruction.recorded.size() == 2);
        lifecycleState.failPresentation = true;
        lifecycleState.drainPending = true;

        SECTION("The complete scene graph survives a failed window tail") {
            lifecycleState.reconstruction.acceptedPrefix = 2;
            REQUIRE(frame.Present().HasError());
            CHECK(lifecycleState.reconstruction.queried == lifecycleState.reconstruction.recorded.back());
            CHECK_FALSE(HistoryFrontend::Take(history->Poll(frontend, epoch)));
            lifecycleState.drainPending = false;
            CHECK_FALSE(HistoryFrontend::Take(history->Poll(frontend, epoch)));
            auto *retained = std::exchange(lifecycleState.graphLease, nullptr);
            REQUIRE(retained);
            retained->Release();
            REQUIRE(HistoryFrontend::Take(history->Poll(frontend, epoch)));
            const auto published = history->Published(epoch);
            REQUIRE(published);
            CHECK(published->frameId == 1);
            CHECK(published->states.color.layout == RenderGraphTextureLayout::ShaderReadOnly);
        }

        SECTION("An accepted dispatch cannot publish final states from a failed later batch") {
            lifecycleState.reconstruction.acceptedPrefix = 1;
            REQUIRE(frame.Present().HasError());
            CHECK(lifecycleState.reconstruction.queried == lifecycleState.reconstruction.recorded.back());
            lifecycleState.drainPending = false;
            auto *retained = std::exchange(lifecycleState.graphLease, nullptr);
            REQUIRE(retained);
            retained->Release();
            CHECK_FALSE(HistoryFrontend::Take(history->Poll(frontend, epoch)));
            CHECK_FALSE(history->Published(epoch));
            CHECK(history->Stage(frontend, request, {color.view, depth.view, motion.view}, {}).HasError());
        }
    }
}  // namespace Horo::Tests::RenderFrontendTesting
