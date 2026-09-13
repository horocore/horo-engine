#include "runtime/renderer/modules/metal/MetalResourcePolicy.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Render::Detail {
    namespace {  // NOSONAR(cpp:S1000) File-local Metal policy fixtures intentionally have internal linkage.

        [[nodiscard]] RenderBufferDescriptor Buffer(const RenderBufferAccess access) {
            return {.byteSize = 64, .usage = RenderBufferUsage::Vertex | RenderBufferUsage::CopyDestination, .access = access};
        }

        [[nodiscard]] RenderTextureDescriptor Texture() {
            return {.dimension = RenderTextureDimension::TwoD,
                    .extent = {4, 4},
                    .format = RenderTextureFormat::Rgba8Unorm,
                    .mipCount = 1,
                    .layerCount = 1,
                    .sampleCount = 1,
                    .usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination,
                    .depth = 1};
        }

        [[nodiscard]] RenderMemoryPlacement Placement(const RenderMemoryCostPlan &plan, const std::size_t offset,
                                                      const std::size_t backing) {
            return {.pool = {.renderer = RenderResourceOwnerId{9}, .value = 7},
                    .scope = {.owner = 3, .incarnation = 2},
                    .attempt = ResourceOperationId{4},
                    .memoryClass = plan.memoryClass,
                    .compatibility = plan.compatibility,
                    .provenance = plan.provenance,
                    .budgetRevision = 5,
                    .offsetBytes = offset,
                    .payloadBytes = plan.payloadBytes,
                    .requiredBytes = plan.requiredBytes,
                    .backingBytes = backing,
                    .allocationClass = plan.allocationClass};
        }

        [[nodiscard]] Result<RenderMemoryCostPlan> DeviceLocalPlan() {
            return PlanMetalBufferMemory(Buffer(RenderBufferAccess::DeviceLocal), {.size = 64, .alignment = 16});
        }

        [[nodiscard]] RenderMeshDescriptor Mesh() {
            RenderMeshDescriptor descriptor{};
            descriptor.vertexBuffer = {{1}, 1, 1};
            descriptor.indexBuffer = {{1}, 2, 1};
            descriptor.vertexStride = sizeof(MeshVertex);
            descriptor.vertexCount = 3;
            descriptor.indexCount = 3;
            descriptor.localBounds = {{-1, -1, -1}, {1, 1, 1}};
            return descriptor;
        }
    }  // namespace

    TEST_CASE("Metal resource policy keeps host-visible and device-local storage distinct") {
        CHECK(MetalBufferStorage(Buffer(RenderBufferAccess::HostVisible)) == MetalResourceStorage::Shared);
        CHECK(MetalBufferStorage(Buffer(RenderBufferAccess::DeviceLocal)) == MetalResourceStorage::Private);
        CHECK(MetalTextureStorage(Texture()) == MetalResourceStorage::Private);
    }

    TEST_CASE("Metal resource policy produces exact suballocated requirements") {
        const auto shared = PlanMetalBufferMemory(Buffer(RenderBufferAccess::HostVisible), {.size = 80, .alignment = 16});
        const auto privateBuffer = PlanMetalBufferMemory(Buffer(RenderBufferAccess::DeviceLocal), {.size = 80, .alignment = 16});
        const auto texture = PlanMetalTextureMemory(Texture(), {.size = 256, .alignment = 64});

        REQUIRE(shared.HasValue());
        REQUIRE(privateBuffer.HasValue());
        REQUIRE(texture.HasValue());
        CHECK(shared.Value().allocationClass == RenderMemoryAllocationClass::Suballocated);
        CHECK(shared.Value().compatibility != privateBuffer.Value().compatibility);
        CHECK(privateBuffer.Value().compatibility != texture.Value().compatibility);
        CHECK(texture.Value().payloadBytes == 64);
    }

    TEST_CASE("Metal resource policy rejects malformed descriptors and native requirements") {
        CHECK(PlanMetalBufferMemory({}, {.size = 64, .alignment = 16}).HasError());
        CHECK(PlanMetalBufferMemory(Buffer(RenderBufferAccess::DeviceLocal), {.size = 32, .alignment = 16}).HasError());
        CHECK(PlanMetalBufferMemory(Buffer(RenderBufferAccess::DeviceLocal), {.size = 64, .alignment = 3}).HasError());
        CHECK(PlanMetalTextureMemory({}, {.size = 64, .alignment = 16}).HasError());
        RenderTextureDescriptor unsupportedDimension = Texture();
        unsupportedDimension.dimension = RenderTextureDimension::ThreeD;
        unsupportedDimension.extent = {4, 4};
        unsupportedDimension.depth = 4;
        CHECK(PlanMetalTextureMemory(unsupportedDimension, {.size = 256, .alignment = 64}).HasError());
    }

    TEST_CASE("Metal texture upload policy calculates aligned rows images and layers") {
        RenderTextureDescriptor texture = Texture();
        texture.extent = {17, 3};
        texture.layerCount = 2;
        const auto tightLayout = PlanMetalTextureUpload(texture, 4);
        const auto paddedLayout = PlanMetalTextureUpload(texture, 256);

        REQUIRE(tightLayout.HasValue());
        CHECK(tightLayout.Value().tightRowBytes == 68);
        CHECK(tightLayout.Value().tightImageBytes == 204);
        CHECK(tightLayout.Value().paddedRowBytes == 68);
        CHECK(tightLayout.Value().paddedImageBytes == 204);
        CHECK(tightLayout.Value().stagingBytes == 408);
        REQUIRE(paddedLayout.HasValue());
        CHECK(paddedLayout.Value().paddedRowBytes == 256);
        CHECK(paddedLayout.Value().paddedImageBytes == 768);
        CHECK(paddedLayout.Value().stagingBytes == 1'536);
    }

    TEST_CASE("Metal texture upload policy rejects invalid alignment and overflow") {
        CHECK(PlanMetalTextureUpload(Texture(), 0).HasError());
        CHECK(PlanMetalTextureUpload(Texture(), 3).HasError());
        RenderTextureDescriptor multisampled = Texture();
        multisampled.sampleCount = 4;
        CHECK(PlanMetalTextureUpload(multisampled, 256).HasError());
        constexpr std::size_t excessiveAlignment = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1U);
        CHECK(PlanMetalTextureUpload(Texture(), excessiveAlignment).HasError());
    }

    TEST_CASE("Metal mesh binding policy requires valid typed vertex and index usages") {
        const RenderBufferUsage vertex = RenderBufferUsage::Vertex | RenderBufferUsage::CopyDestination;
        const RenderBufferUsage index = RenderBufferUsage::Index | RenderBufferUsage::CopyDestination;
        CHECK(ValidateMetalMeshBindings(Mesh(), vertex, index).HasValue());
        CHECK(ValidateMetalMeshBindings(Mesh(), RenderBufferUsage::Index, index).HasError());
        CHECK(ValidateMetalMeshBindings(Mesh(), vertex, RenderBufferUsage::Vertex).HasError());
        RenderMeshDescriptor invalid = Mesh();
        invalid.indexCount = 2;
        CHECK(ValidateMetalMeshBindings(invalid, vertex, index).HasError());
    }

    TEST_CASE("Metal texture aspect policy covers color depth and stencil formats") {
        CHECK(MetalTextureAspectMatches(RenderTextureFormat::Rgba8Unorm, RenderTextureAspect::Color));
        CHECK_FALSE(MetalTextureAspectMatches(RenderTextureFormat::Rgba8Unorm, RenderTextureAspect::Depth));
        CHECK(MetalTextureAspectMatches(RenderTextureFormat::Depth16Unorm, RenderTextureAspect::Depth));
        CHECK_FALSE(MetalTextureAspectMatches(RenderTextureFormat::Depth16Unorm, RenderTextureAspect::Stencil));
        CHECK(MetalTextureAspectMatches(RenderTextureFormat::Depth24Stencil8, RenderTextureAspect::Depth));
        CHECK(MetalTextureAspectMatches(RenderTextureFormat::Depth24Stencil8, RenderTextureAspect::Stencil));
        CHECK(MetalTextureAspectMatches(RenderTextureFormat::Depth24Stencil8, RenderTextureAspect::DepthStencil));
        CHECK_FALSE(MetalTextureAspectMatches(RenderTextureFormat::Depth24Stencil8, RenderTextureAspect::Color));
        CHECK(MetalTextureAspectMatches(RenderTextureFormat::Depth32FloatStencil8, RenderTextureAspect::DepthStencil));
    }

    TEST_CASE("Metal resource policy accepts aligned nonzero heap offsets") {
        const auto plan = DeviceLocalPlan();
        REQUIRE(plan.HasValue());
        CHECK(ValidateMetalResourcePlacement(plan.Value(), Placement(plan.Value(), 16, 128)).HasValue());
        CHECK(ValidateMetalResourcePlacement(plan.Value(), Placement(plan.Value(), 8, 128)).HasError());
        CHECK(ValidateMetalResourcePlacement(plan.Value(), Placement(plan.Value(), 80, 128)).HasError());
    }

    TEST_CASE("Metal heap lifecycle validates reuse and reports final retirement") {
        const auto plan = DeviceLocalPlan();
        REQUIRE(plan.HasValue());
        const RenderMemoryPlacement placement = Placement(plan.Value(), 16, 128);
        MetalHeapState heap{.storage = MetalResourceStorage::Private, .compatibility = plan.Value().compatibility, .backingBytes = 128};

        CHECK(ValidateMetalHeapReuse(heap, placement, MetalResourceStorage::Private).HasValue());
        CHECK(ValidateMetalHeapReuse(heap, placement, MetalResourceStorage::Shared).HasError());
        RenderMemoryPlacement incompatible = placement;
        incompatible.compatibility = RenderMemoryCompatibilityId{99};
        CHECK(ValidateMetalHeapReuse(heap, incompatible, MetalResourceStorage::Private).HasError());
        REQUIRE(RetainMetalHeapResource(heap).HasValue());
        REQUIRE(RetainMetalHeapResource(heap).HasValue());
        CHECK_FALSE(ReleaseMetalHeapResource(heap));
        CHECK(ReleaseMetalHeapResource(heap));
        CHECK_FALSE(ReleaseMetalHeapResource(heap));
    }
}  // namespace Horo::Render::Detail
