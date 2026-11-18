#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderGraphTestUtils.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using Entry = NativeReconstructionEntry;
    using Image = NativeReconstructionImage;

    /** @brief Portable compiled graph evidence; native command ownership is exercised by the Vulkan fixture. */
    class KernelFixture final : public IResidentNativeReconstructionKernel {};

    struct ReconstructionGraph final {
        std::optional<CompiledRenderGraphExecution> compiled;
        std::array<RenderGraphPassWorkload, 1> workloads;
        std::array<RenderGraphResourceInstance, RenderGraphNativeReconstruction::ImageCount> instances;
        std::size_t instanceCount{};

        explicit ReconstructionGraph(Entry entry) {
            constexpr auto images = RenderGraphNativeReconstruction::ImageCount;
            auto builder = Test::RequireBuilder({.maxPasses = 1, .maxResources = images, .maxUsages = images, .maxDependencies = 1});
            const auto pass = Test::RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute);
            constexpr RenderQueueId queue{1};
            std::array initial = std::array<RenderGraphImportedState, images>{};
            RenderGraphNativeReconstruction work;
            work.dispatch = {.entry = entry,
                             .constants = {.extents = {12, 8, 17, 9},
                                           .sample = {0.0F, 0.0F, 1.0F, entry == Entry::TemporalHistory ? 0.75F : 0.0F}}};
            work.kernel = std::make_shared<KernelFixture>();
            for (std::size_t slot = 0; slot < images; ++slot) {
                const auto role = static_cast<Image>(slot);
                if (!NativeReconstructionUsesImage(entry, role))
                    continue;
                const auto localSlot = static_cast<std::uint32_t>(slot + 1);
                const auto texture =
                    Test::RequireResource(builder.ImportTexture(Test::TextureHandle(localSlot), RenderGraphResourceClass::Persistent));
                work.images[slot] = {texture, {{42}, localSlot, 1}};
                const bool writes = NativeReconstructionWritesImage(role);
                const auto access = writes ? RenderGraphAccess::Write : RenderGraphAccess::Read;
                const auto usage = writes ? RenderGraphUsageKind::Storage : RenderGraphUsageKind::Sampled;
                Test::RequireUsage(builder, {pass, texture, access, usage});
                if (writes) {
                    initial[instanceCount] = {texture, {}, RenderGraphInitialContents::Discarded};
                } else {
                    initial[instanceCount] = {texture,
                                              {RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::Sampled,
                                               RenderGraphPipelineScope::Compute, RenderGraphTextureLayout::ShaderReadOnly, queue}};
                }
                instances[instanceCount++] = {texture, localSlot};
            }
            const auto graph = Test::RequireGraph(builder);
            const auto schedule = Test::RequireSchedule(graph);
            constexpr std::array queues{RenderQueueAssignment{RenderQueueRole::Compute, queue}};
            const auto sync = SynthesizeRenderGraphSynchronization(graph, schedule, queues, std::span{initial}.first(instanceCount));
            REQUIRE(sync.HasValue());
            auto result = CompileRenderGraphExecution(graph, schedule, sync.Value(), queues);
            REQUIRE(result.HasValue());
            compiled = std::move(result).Value();
            workloads.front() = {pass, std::move(work)};
        }

        RenderGraphNativeReconstruction &Work() {
            return std::get<RenderGraphNativeReconstruction>(workloads.front().workload);
        }

        Result<void> Validate() const {
            return ValidateRenderGraphExecutionRequest({{1}, *compiled, workloads, std::span{instances}.first(instanceCount)});
        }
    };
}  // namespace

TEST_CASE("Reconstruction graph tracks exact spatial, seed and history image roles", "[renderer][reconstruction][graph]") {
    for (const auto entry : {Entry::Spatial, Entry::TemporalSeed, Entry::TemporalHistory}) {
        CAPTURE(static_cast<unsigned>(entry));
        ReconstructionGraph graph{entry};
        REQUIRE(graph.Validate().HasValue());
        const auto &work = graph.Work();
        const auto image = work.images[static_cast<std::size_t>(Image::OutputColor)].resource;
        const auto transition = std::ranges::find(graph.compiled->Transitions(), image, &RenderGraphTransition::resource);
        REQUIRE(transition != graph.compiled->Transitions().end());
        CHECK(transition->newState.scope == RenderGraphPipelineScope::Compute);
        CHECK(transition->newState.layout == RenderGraphTextureLayout::General);
        CHECK(transition->newState.access == RenderGraphSynchronizationAccess::Write);
        CHECK(work.images[static_cast<std::size_t>(Image::PreviousColor)].resource.IsValid() == (entry == Entry::TemporalHistory));
    }
}

TEST_CASE("Reconstruction denies contradictory and unused graph image bindings", "[renderer][reconstruction][graph]") {
    ReconstructionGraph graph{Entry::TemporalSeed};
    auto &work = graph.Work();
    SECTION("An omitted previous descriptor cannot carry a hidden graph use") {
        work.images[static_cast<std::size_t>(Image::PreviousColor)] = work.images[static_cast<std::size_t>(Image::CurrentColor)];
    }
    SECTION("A declared shader read and write cannot reference a common image") {
        work.images[static_cast<std::size_t>(Image::OutputColor)] = work.images[static_cast<std::size_t>(Image::CurrentColor)];
    }
    SECTION("Every declared role has an exact resident view token") {
        work.images[static_cast<std::size_t>(Image::OutputDepth)].view = {};
    }
    SECTION("An unrepresentable source cannot produce unbounded sample addresses") {
        work.dispatch.constants.extents[0] = std::numeric_limits<std::uint32_t>::max();
    }
    SECTION("History blending cannot be injected into the seed pass") {
        work.dispatch.constants.sample[3] = 0.5F;
    }
    CHECK(graph.Validate().HasError());
}

TEST_CASE("Reconstruction bindings require the whole resolved HDR and depth formats", "[renderer][reconstruction][graph]") {
    const NativeReconstructionDispatchPlan dispatch{.entry = Entry::TemporalSeed, .constants = {.extents = {12, 8, 17, 9}}};
    RenderTextureDescriptor color{.extent = {17, 9},
                                  .format = RenderTextureFormat::Rgba32Float,
                                  .usage = RenderTextureUsage::Storage | RenderTextureUsage::Sampled};
    CHECK(MatchesNativeReconstructionImage(dispatch, Image::OutputColor, color));
    CHECK_FALSE(MatchesNativeReconstructionImage(dispatch, Image::CurrentColor, color));
    CHECK_FALSE(MatchesNativeReconstructionImage(dispatch, Image::PreviousColor, color));
    color.format = RenderTextureFormat::R32Float;
    CHECK(MatchesNativeReconstructionImage(dispatch, Image::OutputDepth, color));
    color.layerCount = 2;
    CHECK_FALSE(MatchesNativeReconstructionImage(dispatch, Image::OutputDepth, color));
}
