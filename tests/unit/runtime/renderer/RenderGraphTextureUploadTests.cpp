#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderGraphTestUtils.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    /** @brief Owns a real compiled portable transfer packet; opaque fixture identities establish no native execution evidence. */
    struct UploadFixture final {
        std::optional<CompiledRenderGraphExecution> graph;
        std::array<RenderGraphPassWorkload, 1> workloads;
        std::array<RenderGraphResourceInstance, 2> instances;
        RenderBufferDescriptor source{16, RenderBufferUsage::CopySource, RenderBufferAccess::HostVisible};
        RenderTextureDescriptor destination{.extent = {2, 2},
                                            .format = RenderTextureFormat::Rgba8Unorm,
                                            .usage = RenderTextureUsage::CopyDestination | RenderTextureUsage::Sampled};

        UploadFixture() {
            auto builder = Test::RequireBuilder({.maxPasses = 1, .maxResources = 2, .maxUsages = 2, .maxDependencies = 1});
            const auto pass = Test::RequirePass(builder, RenderPassKind::Copy, RenderQueueRole::Graphics);
            const auto buffer = Test::RequireResource(builder.ImportBuffer(Test::BufferHandle(1), RenderGraphResourceClass::Persistent));
            const auto texture = Test::RequireResource(builder.ImportTexture(Test::TextureHandle(2), RenderGraphResourceClass::Persistent));
            Test::RequireUsage(builder, {pass, buffer, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource});
            Test::RequireUsage(builder, {pass, texture, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination});
            const auto authored = Test::RequireGraph(builder);
            const auto schedule = Test::RequireSchedule(authored);
            const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
            const std::array initial{RenderGraphImportedState{buffer,
                                                              {RenderGraphSynchronizationAccess::Read,
                                                               RenderGraphSynchronizationOperation::CopySource,
                                                               RenderGraphPipelineScope::Transfer,
                                                               RenderGraphTextureLayout::NotApplicable,
                                                               {1}}},
                                     RenderGraphImportedState{texture, {}, RenderGraphInitialContents::Discarded}};
            const auto synchronization = SynthesizeRenderGraphSynchronization(authored, schedule, queues, initial);
            REQUIRE(synchronization.HasValue());
            auto compiled = CompileRenderGraphExecution(authored, schedule, synchronization.Value(), queues);
            REQUIRE(compiled.HasValue());
            graph = std::move(compiled).Value();
            workloads[0] = {pass, RenderGraphTextureUpload{buffer, texture, {2, 2}, 16}};
            instances = {RenderGraphResourceInstance{buffer, 11}, RenderGraphResourceInstance{texture, 12}};
        }

        RenderGraphTextureUpload &Upload() {
            return std::get<RenderGraphTextureUpload>(workloads[0].workload);
        }

        Result<void> ValidateGraph() const {
            return ValidateRenderGraphExecutionRequest({{1}, *graph, workloads, instances});
        }

        Result<void> ValidateDescriptors() {
            return ValidateTextureUploadDescriptors(Upload(), source, destination);
        }
    };
}  // namespace

TEST_CASE("Whole RGBA8 upload has exact portable transfer uses and descriptor bounds", "[renderer][texture-upload]") {
    UploadFixture fixture;
    REQUIRE(fixture.ValidateGraph().HasValue());
    REQUIRE(fixture.ValidateDescriptors().HasValue());
    REQUIRE(fixture.graph->Passes().size() == 1);
    CHECK(fixture.graph->Passes()[0].kind == RenderPassKind::Copy);
    const auto destination = fixture.Upload().destination;
    const auto transition = std::ranges::find(fixture.graph->Transitions(), destination, &RenderGraphTransition::resource);
    REQUIRE(transition != fixture.graph->Transitions().end());
    CHECK(transition->newState.layout == RenderGraphTextureLayout::CopyDestination);
    CHECK(transition->newState.access == RenderGraphSynchronizationAccess::Write);
}

TEST_CASE("Texture upload rejects source owner kind count and extent mismatches", "[renderer][texture-upload]") {
    UploadFixture fixture;
    SECTION("foreign source owner") {
        fixture.Upload().source.owner = {};
    }
    SECTION("texture relabeled as source buffer") {
        fixture.Upload().source = fixture.Upload().destination;
    }
    SECTION("buffer relabeled as destination image") {
        fixture.Upload().destination = fixture.Upload().source;
    }
    SECTION("zero byte payload") {
        fixture.Upload().byteCount = 0;
    }
    SECTION("wrong exact byte payload") {
        fixture.Upload().byteCount = 15;
    }
    SECTION("zero width") {
        fixture.Upload().extent.width = 0;
    }
    SECTION("overflowing whole image product") {
        fixture.Upload().extent = {std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max()};
    }
    CHECK(fixture.ValidateGraph().HasError());
}

TEST_CASE("Texture upload source must be the exact registered host-visible transfer buffer", "[renderer][texture-upload]") {
    UploadFixture fixture;
    SECTION("device local source") {
        fixture.source.access = RenderBufferAccess::DeviceLocal;
    }
    SECTION("undeclared transfer usage") {
        fixture.source.usage = RenderBufferUsage::Uniform;
    }
    SECTION("short source") {
        fixture.source.byteSize = 15;
    }
    SECTION("padded source is not a whole exact upload") {
        fixture.source.byteSize = 20;
    }
    CHECK(fixture.ValidateDescriptors().HasError());
}

TEST_CASE("Texture upload admits only the declared whole RGBA8 base level", "[renderer][texture-upload]") {
    UploadFixture fixture;
    SECTION("foreign format") {
        fixture.destination.format = RenderTextureFormat::Rgba16Float;
    }
    SECTION("different extent") {
        fixture.destination.extent.width = 4;
    }
    SECTION("missing copy destination") {
        fixture.destination.usage = RenderTextureUsage::Sampled;
    }
    SECTION("multiple layers") {
        fixture.destination.layerCount = 2;
    }
    SECTION("multiple mips") {
        fixture.destination.mipCount = 2;
    }
    SECTION("multisampled") {
        fixture.destination.sampleCount = 4;
    }
    SECTION("volume texture") {
        fixture.destination.dimension = RenderTextureDimension::ThreeD;
        fixture.destination.depth = 2;
    }
    CHECK(fixture.ValidateDescriptors().HasError());
}

TEST_CASE("Texture upload does not authorize a graphics pass or an invalid usage range", "[renderer][texture-upload]") {
    UploadFixture fixture;
    auto pass = fixture.graph->Passes()[0];
    SECTION("wrong pass domain") {
        pass.kind = RenderPassKind::Graphics;
    }
    SECTION("invalid borrowed usage range") {
        pass.usages.count = std::numeric_limits<std::size_t>::max();
    }
    const RenderGraphExecutionRequest request{{1}, *fixture.graph, fixture.workloads, fixture.instances};
    CHECK(ValidateTextureUploadGraphWorkload(request, pass, fixture.Upload()).HasError());
    CHECK(fixture.ValidateGraph().HasValue());
}
