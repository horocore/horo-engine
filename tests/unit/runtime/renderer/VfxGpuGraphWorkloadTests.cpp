#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderGraphTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    // This lease exercises portable admission only; it supplies no native shader or execution proof.
    class MetadataOnlyKernel final : public IResidentVfxGpuKernel {};

    class MetadataOnlySimulation final : public IResidentVfxGpuSimulation {
    public:
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept override {
            return 0;  // Portable admission fixture has no native owner or GPU inventory.
        }
    };

    VfxGpuSimulationDescriptor Descriptor() {
        auto layout = BuildGpuParticleLayout(4, 0, {4096, 4096});
        REQUIRE(layout.HasValue());
        VfxGpuSimulationDescriptor descriptor;
        descriptor.identity = {7, {7, 1, 1}, {7, 2, 1}, {7, 3, 1}};
        descriptor.initialParticleCount = 4;
        descriptor.layout = layout.Value();
        descriptor.random = {11, descriptor.identity.activation, descriptor.identity.emitter};
        descriptor.maximumPendingSteps = 2;
        descriptor.maximumRetainedBytes = 4096;
        descriptor.capabilityRevision = descriptor.policyRevision = 1;
        descriptor.initializationDigest.bytes[0] = 1;
        descriptor.arithmetic.maximumSteps = 8;
        descriptor.arithmetic.maximumDeltaSeconds = 1;
        descriptor.arithmetic.maximumPositionMagnitude = 4;
        descriptor.arithmetic.maximumVelocityMagnitude = 2;
        descriptor.arithmetic.maximumAccelerationMagnitude = 1;
        descriptor.arithmetic.maximumRotationMagnitude = 4;
        descriptor.arithmetic.maximumSizeMagnitude = 2;
        return descriptor;
    }

    CompiledRenderGraphExecution Compile(RenderGraphBuilder &builder, const std::span<const RenderGraphImportedState> initial) {
        auto graph = Test::RequireGraph(builder);
        auto schedule = Test::RequireSchedule(graph);
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        auto synchronized = SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial);
        REQUIRE(synchronized.HasValue());
        auto compiled = CompileRenderGraphExecution(graph, schedule, synchronized.Value(), queues);
        REQUIRE(compiled.HasValue());
        return std::move(compiled).Value();
    }

    struct ComputeFixture {
        std::optional<CompiledRenderGraphExecution> execution;
        std::array<RenderGraphPassWorkload, 1> workloads;
        std::array<RenderGraphResourceInstance, 4> instances;

        explicit ComputeFixture(const RenderPassKind kind = RenderPassKind::Compute) {
            auto builder = Test::RequireBuilder({.maxPasses = 1, .maxResources = 4, .maxUsages = 4, .maxDependencies = 1});
            const auto pass = Test::RequirePass(builder, kind, RenderQueueRole::Graphics);
            std::array<RenderGraphResourceId, 4> ids;
            std::array<RenderGraphImportedState, 4> initial;
            for (std::size_t index = 0; index < ids.size(); ++index) {
                ids[index] = Test::RequireResource(
                    builder.ImportBuffer(Test::BufferHandle(static_cast<std::uint32_t>(index + 1)), RenderGraphResourceClass::Persistent));
                const auto access = index == 0 || index == 2 ? RenderGraphAccess::Read : RenderGraphAccess::Write;
                Test::RequireUsage(builder, {pass, ids[index], access, RenderGraphUsageKind::Storage});
                initial[index] = {ids[index],
                                  {RenderGraphSynchronizationAccess::ReadWrite,
                                   RenderGraphSynchronizationOperation::Storage,
                                   RenderGraphPipelineScope::Compute,
                                   RenderGraphTextureLayout::NotApplicable,
                                   {1}}};
                instances[index] = {ids[index], index + 1};
            }
            execution = Compile(builder, initial);
            auto descriptor = Descriptor();
            const GpuVfxSimulationWork work{descriptor.identity, {{{17}, 0, 1}, 13, 0, 1, 2}, 0.25F, false};
            workloads[0] = {pass,
                            RenderGraphVfxGpuCompute{ids[0], ids[1], ids[2], ids[3], descriptor, work,
                                                     std::make_shared<MetadataOnlyKernel>(), std::make_shared<MetadataOnlySimulation>()}};
        }

        RenderGraphVfxGpuCompute &Work() {
            return std::get<RenderGraphVfxGpuCompute>(workloads[0].workload);
        }

        Result<void> Validate() const {
            return ValidateRenderGraphExecutionRequest({{1}, *execution, workloads, instances});
        }
    };

    struct DrawFixture {
        std::optional<CompiledRenderGraphExecution> execution;
        std::array<RenderGraphPassWorkload, 1> workloads;
        std::array<RenderGraphResourceInstance, 3> instances;

        explicit DrawFixture(const bool depth = false) {
            auto builder = Test::RequireBuilder({.maxPasses = 1, .maxResources = 3, .maxUsages = 3, .maxDependencies = 1});
            const auto pass = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            const auto particles = Test::RequireResource(builder.ImportBuffer(Test::BufferHandle(), RenderGraphResourceClass::Persistent));
            const auto color = Test::RequireResource(builder.ImportTexture(Test::TextureHandle(), RenderGraphResourceClass::Persistent));
            Test::RequireUsage(builder, {pass, particles, RenderGraphAccess::Read, RenderGraphUsageKind::Storage});
            Test::RequireUsage(builder, {pass, color, RenderGraphAccess::ReadWrite, RenderGraphUsageKind::ColorAttachment});
            std::array<RenderGraphImportedState, 3> initial{{{particles,
                                                              {RenderGraphSynchronizationAccess::Read,
                                                               RenderGraphSynchronizationOperation::Storage,
                                                               RenderGraphPipelineScope::Graphics,
                                                               RenderGraphTextureLayout::NotApplicable,
                                                               {1}}},
                                                             {color,
                                                              {RenderGraphSynchronizationAccess::ReadWrite,
                                                               RenderGraphSynchronizationOperation::ColorAttachment,
                                                               RenderGraphPipelineScope::Graphics,
                                                               RenderGraphTextureLayout::ColorAttachment,
                                                               {1}}},
                                                             {}}};
            RenderGraphResourceId depthId;
            if (depth) {
                depthId = Test::RequireResource(builder.ImportTexture(Test::TextureHandle(2), RenderGraphResourceClass::Persistent));
                Test::RequireUsage(builder, {pass, depthId, RenderGraphAccess::Read, RenderGraphUsageKind::DepthStencilAttachment});
                initial[2] = {depthId,
                              {RenderGraphSynchronizationAccess::Read,
                               RenderGraphSynchronizationOperation::DepthStencilAttachment,
                               RenderGraphPipelineScope::Graphics,
                               RenderGraphTextureLayout::DepthStencilAttachment,
                               {1}}};
            }
            execution = Compile(builder, std::span{initial}.first(depth ? 3 : 2));
            instances = {{{particles, 1}, {color, 1}, {depthId, 2}}};
            RenderGraphVfxGpuDraw draw;
            draw.particles = particles;
            draw.color = color;
            draw.depth = depthId;
            draw.simulation = {{17}, 0, 1};
            draw.deviceIncarnation = 13;
            draw.generation = 1;
            draw.descriptor = Descriptor();
            draw.view.extent = {64, 64};
            draw.colorOperations.loadOperation = AttachmentLoadOperation::Load;
            draw.colorOperations.storeOperation = AttachmentStoreOperation::Store;
            draw.kernel = std::make_shared<MetadataOnlyKernel>();
            draw.simulationLease = std::make_shared<MetadataOnlySimulation>();
            workloads[0] = {pass, std::move(draw)};
        }

        RenderGraphVfxGpuDraw &Work() {
            return std::get<RenderGraphVfxGpuDraw>(workloads[0].workload);
        }

        Result<void> Validate() const {
            return ValidateRenderGraphExecutionRequest(
                {{1}, *execution, workloads, std::span{instances}.first(execution->Resources().size())});
        }
    };
}  // namespace

TEST_CASE("GPU compute graph admits exact immutable inputs and distinct candidate storage", "[renderer][vfx][gpu-graph]") {
    ComputeFixture fixture;
    CHECK(fixture.Validate().HasValue());
}

TEST_CASE("GPU compute graph rejects malformed step metadata before native admission", "[renderer][vfx][gpu-graph]") {
    ComputeFixture fixture;
    REQUIRE(fixture.Validate().HasValue());
    SECTION("missing kernel") {
        fixture.Work().kernel.reset();
    }
    SECTION("missing native simulation") {
        fixture.Work().simulationLease.reset();
    }
    SECTION("stale logical identity") {
        ++fixture.Work().work.identity.emitter.generation;
    }
    SECTION("unissued device") {
        fixture.Work().work.step.deviceIncarnation = 0;
    }
    SECTION("skipped candidate generation") {
        ++fixture.Work().work.step.targetGeneration;
    }
    SECTION("invalid delta") {
        fixture.Work().work.deltaSeconds = std::numeric_limits<float>::infinity();
    }
    SECTION("expired step allowance") {
        fixture.Work().work.step.tick = fixture.Work().descriptor.arithmetic.maximumSteps;
    }
    SECTION("undeclared observation") {
        fixture.Work().work.observe = true;
    }
    CHECK(fixture.Validate().HasError());
}

TEST_CASE("GPU compute graph rejects logical and physical imported-buffer aliases", "[renderer][vfx][gpu-graph]") {
    ComputeFixture fixture;
    REQUIRE(fixture.Validate().HasValue());
    SECTION("logical source and destination") {
        fixture.Work().destination = fixture.Work().source;
    }
    SECTION("physical source and destination") {
        fixture.instances[1].instance = fixture.instances[0].instance;
    }
    SECTION("physical force and observation") {
        fixture.instances[3].instance = fixture.instances[2].instance;
    }
    SECTION("read write exchange") {
        std::swap(fixture.Work().source, fixture.Work().destination);
    }
    SECTION("unresolved storage") {
        fixture.instances[3].instance = 0;
    }
    SECTION("foreign graph owner") {
        fixture.Work().source.owner = {};
    }
    CHECK(fixture.Validate().HasError());
}

TEST_CASE("GPU compute graph bounds malformed usage ranges before slicing", "[renderer][vfx][gpu-graph]") {
    ComputeFixture fixture;
    auto pass = fixture.execution->Passes()[0];
    pass.usages.offset = std::numeric_limits<std::size_t>::max();
    CHECK(ValidateVfxGpuComputeGraphWorkload({{1}, *fixture.execution, fixture.workloads, fixture.instances}, pass, fixture.Work())
              .HasError());
    ComputeFixture graphics(RenderPassKind::Graphics);
    CHECK(graphics.Validate().HasError());
}

TEST_CASE("GPU draw graph consumes storage directly with retained color and optional read-only depth", "[renderer][vfx][gpu-graph]") {
    DrawFixture withoutDepth;
    CHECK(withoutDepth.Validate().HasValue());
    DrawFixture withDepth(true);
    CHECK(withDepth.Validate().HasValue());
}

TEST_CASE("GPU draw graph rejects invalid generation views and discarded attachment contents", "[renderer][vfx][gpu-graph]") {
    DrawFixture fixture;
    REQUIRE(fixture.Validate().HasValue());
    SECTION("unissued simulation") {
        fixture.Work().simulation.owner = {};
    }
    SECTION("unissued device") {
        fixture.Work().deviceIncarnation = 0;
    }
    SECTION("missing generation") {
        fixture.Work().generation = 0;
    }
    SECTION("unbounded generation") {
        fixture.Work().generation = std::numeric_limits<std::uint64_t>::max();
    }
    SECTION("nonfinite view") {
        fixture.Work().view.originDelta.x = std::numeric_limits<float>::infinity();
    }
    SECTION("empty extent") {
        fixture.Work().view.extent.width = 0;
    }
    SECTION("missing native preparation") {
        fixture.Work().kernel.reset();
    }
    SECTION("missing native simulation") {
        fixture.Work().simulationLease.reset();
    }
    SECTION("discarding output") {
        fixture.Work().colorOperations.storeOperation = AttachmentStoreOperation::DontCare;
    }
    SECTION("clear without declaring a write-only use") {
        fixture.Work().colorOperations.loadOperation = AttachmentLoadOperation::Clear;
    }
    CHECK(fixture.Validate().HasError());
}

TEST_CASE("GPU draw graph rejects aliased or forged depth resources", "[renderer][vfx][gpu-graph]") {
    DrawFixture fixture(true);
    REQUIRE(fixture.Validate().HasValue());
    SECTION("logical color depth alias") {
        fixture.Work().depth = fixture.Work().color;
    }
    SECTION("physical color depth alias") {
        fixture.instances[2].instance = fixture.instances[1].instance;
    }
    SECTION("buffer used as depth") {
        fixture.Work().depth = fixture.Work().particles;
    }
    SECTION("unresolved depth") {
        fixture.instances[2].instance = 0;
    }
    CHECK(fixture.Validate().HasError());
}

TEST_CASE("Retained GPU work cannot admit a new compute or draw after unit cancellation", "[renderer][vfx][gpu-graph]") {
    CancellationSource lifetime;
    ComputeFixture compute;
    DrawFixture draw;
    compute.Work().work.lifetimeCancellation = lifetime.Token();
    draw.Work().lifetimeCancellation = lifetime.Token();
    REQUIRE(compute.Validate().HasValue());
    REQUIRE(draw.Validate().HasValue());
    lifetime.RequestCancellation();
    CHECK(compute.Validate().HasError());
    CHECK(draw.Validate().HasError());
}
