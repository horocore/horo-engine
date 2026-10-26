#include "VulkanExternalGraphTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanExternalRegistrationStorage.h"
#include "runtime/renderer/modules/vulkan/VulkanRenderBackendErrors.h"

namespace {
    using Horo::Result;
    using namespace Horo::Render;
    using namespace Horo::Render::VulkanResourceTest;
    using namespace Horo::Render::VulkanGraphTest;
    using Horo::Render::VulkanExternalTest::ExternalGraphPacket;

    struct ExternalNativeLease final : IRenderResourceRetirementLease {
        std::uint32_t claims{};
        std::uint32_t releases{};

        void Claim() noexcept override {
            ++claims;
        }

        void Release() noexcept override {
            ++releases;
        }
    };

    struct ExternalNativeSource final : Detail::IVulkanExternalTextureSource {
        VkDevice device{NativeHandle<VkDevice>(1)};
        VkImage image{NativeHandle<VkImage>(900)};
        bool current{true};
        mutable bool releaseStateProved{};
        std::uint32_t family{};
        Detail::VulkanExternalNativeViews prepared;

        Detail::VulkanExternalNativeViews PreparedNativeViews() const noexcept override {
            return prepared;
        }

        RenderTextureDescriptor descriptor{.extent = {640, 480}, .layerCount = 2, .usage = RenderTextureUsage::RenderAttachment};

        Result<void> Validate() const override {
            return current ? Result<void>::Success() : Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        }

        std::uint64_t DeviceIncarnation() const noexcept override {
            return 11;
        }

        const RenderTextureDescriptor &Descriptor() const noexcept override {
            return descriptor;
        }

        VkDevice NativeDevice() const noexcept override {
            return device;
        }

        VkImage NativeImage() const noexcept override {
            return image;
        }

        std::uint32_t NativeQueueFamily() const noexcept override {
            return family;
        }

        void RecordNativeRetirement(const RenderGraphLogicalState &state, const bool hasNativeState) const noexcept override {
            const auto required = AcquiredState();
            releaseStateProved = hasNativeState && state.layout == required.layout && state.queue == required.queue &&
                                 state.operation == required.operation && state.scope == required.scope;
        }

        RenderGraphLogicalState AcquiredState() const noexcept override {
            const bool depth = descriptor.format == RenderTextureFormat::Depth32Float;
            return {RenderGraphSynchronizationAccess::ReadWrite,
                    depth ? RenderGraphSynchronizationOperation::DepthStencilAttachment
                          : RenderGraphSynchronizationOperation::ColorAttachment,
                    RenderGraphPipelineScope::Graphics,
                    depth ? RenderGraphTextureLayout::DepthStencilAttachment : RenderGraphTextureLayout::ColorAttachment,
                    {1}};
        }

        RenderGraphLogicalState RequiredFinalState() const noexcept override {
            auto state = AcquiredState();
            state.scope = RenderGraphPipelineScope::External;
            return state;
        }
    };

    TEST_CASE("Vulkan external wrapper backing is bounded and survives the final queued weak reference", "[vulkan][external][storage]") {
        struct Value {
            int marker{42};
        };

        auto pool = std::make_shared<Detail::VulkanExternalAllocationPool>();
        std::weak_ptr<Detail::VulkanExternalAllocationPool> retainedPool = pool;
        std::weak_ptr<Value> retained;
        {
            auto value = std::allocate_shared<Value>(Detail::VulkanExternalAllocator<Value>{pool});
            retained = value;
            pool.reset();
            CHECK_FALSE(retainedPool.expired());
            CHECK(value->marker == 42);
        }
        CHECK(retained.expired());
        CHECK_FALSE(retainedPool.expired());
        retained.reset();
        CHECK(retainedPool.expired());
        pool = std::make_shared<Detail::VulkanExternalAllocationPool>();
        std::array<std::shared_ptr<Value>, Detail::VulkanExternalAllocationPool::Capacity> values;
        for (auto &value : values)
            value = std::allocate_shared<Value>(Detail::VulkanExternalAllocator<Value>{pool});
        CHECK_THROWS_AS(std::allocate_shared<Value>(Detail::VulkanExternalAllocator<Value>{pool}), std::bad_alloc);
        values.front().reset();
        CHECK_NOTHROW(values.front() = std::allocate_shared<Value>(Detail::VulkanExternalAllocator<Value>{pool}));
    }

    TEST_CASE("Vulkan acquired image registrations borrow cold views without creating or destroying native views",
              "[vulkan][external][storage]") {
        NativeResourceScope native;
        ExternalNativeLease lease;
        ExternalNativeSource source;
        source.descriptor.usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled;
        source.prepared = {NativeHandle<VkImageView>(701), {NativeHandle<VkImageView>(702), NativeHandle<VkImageView>(703)}, 2};
        Detail::VulkanResourceRuntime runtime;
        InitializeNativeResourceRuntime(runtime);
        std::uint64_t previous{};
        for (std::size_t acquisition = 0; acquisition < 128; ++acquisition) {
            const auto imported = runtime.ImportExternalTexture(source, RenderResourceNativeLifetime{lease});
            REQUIRE(imported.HasValue());
            CHECK(imported.Value() > previous);
            previous = imported.Value();
            const RenderTextureViewDescriptor eye{.texture = {{41}, 1, 1}};
            const auto view = runtime.CreateTextureView(eye, imported.Value());
            REQUIRE(view.HasValue());
            auto right = eye;
            right.baseLayer = 1;
            REQUIRE(runtime.CreateTextureView(right, imported.Value()).HasValue());
            auto outside = eye;
            outside.baseLayer = 2;
            CHECK(runtime.CreateTextureView(outside, imported.Value()).HasError());
            runtime.DestroyTexture(imported.Value());
            CHECK(runtime.ExternalTextureSource(imported.Value()) == nullptr);
        }
        CHECK(lease.claims == 128);
        CHECK(lease.releases == 128);
        CHECK(native.state.createViewCount == 0);
        CHECK(native.state.destroyViewCount == 0);
        CHECK(native.state.destroyImageCount == 0);
    }

    TEST_CASE("Vulkan external image retirement destroys only Horo views and releases the canonical lifetime",
              "[unit][runtime][renderer][vulkan][external]") {
        NativeResourceScope native;
        ExternalNativeLease lease;
        ExternalNativeSource source;
        Detail::VulkanResourceRuntime runtime;
        InitializeNativeResourceRuntime(runtime);
        const auto imported = runtime.ImportExternalTexture(source, RenderResourceNativeLifetime{lease});
        REQUIRE(imported.HasValue());
        CHECK(native.state.createImageCount == 0);
        CHECK(native.state.allocateCount == 0);
        CHECK(native.state.createViewCount == 1);
        CHECK(lease.claims == 1);
        CHECK(lease.releases == 0);
        CHECK(runtime.ExternalTextureSource(imported.Value()) == &source);
        runtime.DestroyTexture(imported.Value());
        CHECK(native.state.destroyImageCount == 0);
        CHECK(native.state.freeCount == 0);
        CHECK(native.state.destroyViewCount == 1);
        CHECK(lease.releases == 1);
        CHECK(source.releaseStateProved);
        CHECK(runtime.ExternalTextureSource(imported.Value()) == nullptr);
        runtime.DestroyTexture(imported.Value());
        CHECK(lease.releases == 1);
    }

    TEST_CASE("Vulkan views over borrowed images retain Horo ownership during explicit and cascading retirement",
              "[unit][runtime][renderer][vulkan][external]") {
        NativeResourceScope native;
        ExternalNativeLease imageLease;
        ExternalNativeLease viewLease;
        ExternalNativeSource source;
        Detail::VulkanResourceRuntime runtime;
        InitializeNativeResourceRuntime(runtime);
        const auto imported = runtime.ImportExternalTexture(source, RenderResourceNativeLifetime{imageLease});
        REQUIRE(imported.HasValue());
        const RenderTextureViewDescriptor descriptor{.texture = {{41}, 1, 1},
                                                     .layerCount = 2,
                                                     .dimension = RenderTextureViewDimension::TwoDArray};
        const auto explicitView = runtime.CreateTextureView(descriptor, imported.Value(), RenderResourceNativeLifetime{viewLease});
        const auto cascadingView = runtime.CreateTextureView(descriptor, imported.Value());
        REQUIRE(explicitView.HasValue());
        REQUIRE(cascadingView.HasValue());
        CHECK(native.state.createViewCount == 3);
        runtime.DestroyTextureView(explicitView.Value());
        CHECK(native.state.destroyViewCount == 1);
        CHECK(viewLease.releases == 1);
        CHECK(imageLease.releases == 0);
        CHECK(native.state.destroyImageCount == 0);
        runtime.DestroyTexture(imported.Value());
        CHECK(native.state.destroyViewCount == 3);
        CHECK(imageLease.releases == 1);
        CHECK(source.releaseStateProved);
        CHECK(native.state.destroyImageCount == 0);
        CHECK(native.state.freeCount == 0);
        runtime.ShutdownResources();
        CHECK(native.state.destroyViewCount == 3);
        CHECK(viewLease.releases == 1);
        CHECK(imageLease.releases == 1);
    }

    TEST_CASE("Vulkan external image admission rejects foreign stale duplicate and failed view imports",
              "[unit][runtime][renderer][vulkan][external]") {
        NativeResourceScope native;
        ExternalNativeLease lease;
        ExternalNativeSource source;
        Detail::VulkanResourceRuntime runtime;
        InitializeNativeResourceRuntime(runtime);
        SECTION("foreign device") {
            source.device = NativeHandle<VkDevice>(2);
        }
        SECTION("stale acquisition") {
            source.current = false;
        }
        SECTION("invalid native image") {
            source.image = VK_NULL_HANDLE;
        }
        SECTION("unsupported sample count") {
            source.descriptor.sampleCount = 4;
        }
        SECTION("native view failure") {
            native.state.createViewResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        SECTION("duplicate native allocation") {
            ExternalNativeLease firstLease;
            const auto first = runtime.ImportExternalTexture(source, RenderResourceNativeLifetime{firstLease});
            REQUIRE(first.HasValue());
            CHECK(runtime.ImportExternalTexture(source, RenderResourceNativeLifetime{lease}).HasError());
            runtime.DestroyTexture(first.Value());
            CHECK(firstLease.releases == 1);
            CHECK(native.state.destroyImageCount == 0);
            return;
        }
        CHECK(runtime.ImportExternalTexture(source, RenderResourceNativeLifetime{lease}).HasError());
        CHECK(lease.claims == 1);
        CHECK(lease.releases == 1);
        CHECK(native.state.destroyImageCount == 0);
        CHECK(native.state.freeCount == 0);
    }

    ExternalGraphPacket ExternalGraph(const ExternalNativeSource &source, const std::uint64_t instance, const bool includeFinal = true) {
        return VulkanExternalTest::ExternalGraph({{41}, 1, 1}, source.descriptor, source.RequiredFinalState(), instance, includeFinal);
    }

    TEST_CASE("Vulkan actual external color and depth graph encodes release after rendering and retires after GPU completion",
              "[unit][runtime][renderer][vulkan][external][graph]") {
        ExternalNativeLease nativeLease;
        ExternalNativeSource source;
        source.family = 3;
        SECTION("color array") {}
        SECTION("depth array") {
            source.descriptor.format = RenderTextureFormat::Depth32Float;
        }
        NativeGraphFixture fixture;
        const auto imported = fixture.resources.ImportExternalTexture(source, RenderResourceNativeLifetime{nativeLease});
        REQUIRE(imported.HasValue());
        const VkImageAspectFlags expectedAspect =
            source.descriptor.format == RenderTextureFormat::Depth32Float ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        CHECK(fixture.resourceState.state.lastViewAspectMask == expectedAspect);
        auto packet = ExternalGraph(source, imported.Value());
        RenderGraphLeaseTransfer transfer;
        REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
        const auto execution = fixture.graph.ExecuteGraph(
            {{1}, packet.graph, packet.workloads, packet.instances, &fixture.lease, false, &transfer, packet.submissions});
        INFO((execution.HasError() ? execution.ErrorValue().message : "external native graph admitted"));
        REQUIRE(execution.HasValue());
        REQUIRE(fixture.encoded.imageBarriers.size() == 2);
        CHECK(fixture.encoded.layers == 2);
        CHECK(fixture.encoded.depthAttachment == (source.descriptor.format == RenderTextureFormat::Depth32Float));
        REQUIRE(fixture.encoded.operations.size() == 4);
        CHECK(fixture.encoded.operations[0] == "barrier");
        CHECK(fixture.encoded.operations[1] == "begin-rendering");
        CHECK(fixture.encoded.operations[2] == "end-rendering");
        CHECK(fixture.encoded.operations[3] == "barrier");
        CHECK(fixture.encoded.imageBarriers.back().image == source.image);
        CHECK(fixture.encoded.imageBarriers.back().subresourceRange.layerCount == 2);
        REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
        fixture.resources.DestroyTexture(imported.Value());
        CHECK(nativeLease.releases == 0);
        CHECK_FALSE(source.releaseStateProved);
        const RenderCpuWait poll{.completion = {{1}, 1}, .expectedDeviceIncarnation = 11};
        const auto pending = fixture.graph.ObserveSubmission(poll);
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        CHECK(fixture.resourceState.state.destroyViewCount == 0);
        fixture.commandState.state.Complete();
        const auto complete = fixture.graph.ObserveSubmission(poll);
        REQUIRE(complete.HasValue());
        CHECK(complete.Value());
        CHECK(nativeLease.releases == 1);
        CHECK(source.releaseStateProved);
        CHECK(fixture.resourceState.state.destroyViewCount == 1);
        CHECK(fixture.resourceState.state.destroyImageCount == 0);
        CHECK(fixture.commandState.state.waitCalls == 0);
    }

    TEST_CASE("Vulkan external release rejects mismatched native queue and revoked acquisition before command recording",
              "[unit][runtime][renderer][vulkan][external][graph]") {
        ExternalNativeLease nativeLease;
        ExternalNativeSource source;
        source.family = 3;
        NativeGraphFixture fixture;
        const auto imported = fixture.resources.ImportExternalTexture(source, RenderResourceNativeLifetime{nativeLease});
        REQUIRE(imported.HasValue());
        bool includeFinal = true;
        SECTION("missing release obligation") {
            includeFinal = false;
        }
        auto packet = ExternalGraph(source, imported.Value(), includeFinal);
        SECTION("native family differs") {
            source.family = 7;
        }
        SECTION("acquisition revoked") {
            source.current = false;
        }
        RenderGraphLeaseTransfer transfer;
        REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
        const auto rejected = fixture.graph.ExecuteGraph(
            {{1}, packet.graph, packet.workloads, packet.instances, &fixture.lease, false, &transfer, packet.submissions});
        CHECK(rejected.HasError());
        CHECK(fixture.commandState.state.beginCalls == 0);
        CHECK(fixture.commandState.state.accepted.empty());
        CHECK(fixture.encoded.operations.empty());
        fixture.graph.AbortFrame({1});
        fixture.resources.DestroyTexture(imported.Value());
    }

}  // namespace
