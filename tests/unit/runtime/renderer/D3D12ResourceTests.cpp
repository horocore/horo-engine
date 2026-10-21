#include "D3D12ResourceTestSupport.h"

TEST_CASE("D3D12 resource service rejects invalid finite host bounds", "[renderer][d3d12][resources]") {
    Horo::Render::D3D12ResourceLimits limits;
    REQUIRE(limits.IsValid());
    limits.maximumResources = 0;
    REQUIRE_FALSE(limits.IsValid());
    limits = {};
    limits.maximumDescriptors = 2049;
    REQUIRE_FALSE(limits.IsValid());
    limits = {};
    limits.maximumTransfers = 1025;
    REQUIRE_FALSE(limits.IsValid());
    limits = {};
    limits.maximumTransferBytes = 0;
    REQUIRE_FALSE(limits.IsValid());
}

#if defined(_WIN32) && defined(_M_X64)
using namespace Horo;
using namespace Horo::Render;
using namespace Horo::Render::D3D12Tests;

TEST_CASE("D3D12 WARP buffer uploads and readbacks retain actual bytes and backing", "[renderer][d3d12][resources][warp]") {
    Fixture fixture;
    auto &resources = *fixture.resources;
    std::array<std::byte, 257> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>(index % 251);
    const auto buffer = fixture.Buffer({bytes.size(), CopyUsage}, bytes);
    REQUIRE(resources.Snapshot().committedBytes >= 65536);
    const auto readback = resources.Readback(buffer, 0, bytes.size());
    REQUIRE(readback.HasValue());
    resources.DestroyBuffer(buffer);
    REQUIRE(resources.Readback(buffer, 0, 1).HasError());
    REQUIRE(resources.Snapshot().retiringBytes >= 65536);
    REQUIRE(fixture.Read(readback.Value()) == std::vector<std::byte>(bytes.begin(), bytes.end()));
    REQUIRE(resources.Poll().HasValue());
    REQUIRE(resources.Snapshot().committedBytes == 0);
    REQUIRE(resources.Snapshot().resourceCount == 0);
}

TEST_CASE("D3D12 WARP texture copies preserve tight rows layers and volume slices", "[renderer][d3d12][resources][warp]") {
    Fixture fixture;
    for (const auto dimension : {RenderTextureDimension::OneD, RenderTextureDimension::TwoD, RenderTextureDimension::ThreeD}) {
        RenderTextureDescriptor descriptor;
        descriptor.dimension = dimension;
        descriptor.extent = {7, dimension == RenderTextureDimension::OneD ? 1U : 3U};
        descriptor.depth = dimension == RenderTextureDimension::ThreeD ? 2 : 1;
        descriptor.layerCount = dimension == RenderTextureDimension::ThreeD ? 1 : 2;
        descriptor.mipCount = 2;
        descriptor.usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopySource | RenderTextureUsage::CopyDestination;
        std::vector<std::byte> bytes(*RenderTextureBaseLevelByteSize(descriptor));
        for (std::size_t index = 0; index < bytes.size(); ++index)
            bytes[index] = static_cast<std::byte>(index % 239);
        const auto texture = fixture.Texture(descriptor, bytes);
        const auto transfer = fixture.resources->Readback(texture, 0, bytes.size());
        REQUIRE(transfer.HasValue());
        REQUIRE(fixture.Read(transfer.Value()) == bytes);
        fixture.resources->DestroyTexture(texture);
        REQUIRE(fixture.resources->Poll().HasValue());
    }
    REQUIRE(fixture.resources->Snapshot().committedBytes == 0);
}

TEST_CASE("D3D12 WARP validates placement and exact queue ownership before native work", "[renderer][d3d12][resources][warp]") {
    Fixture fixture;
    const RenderBufferDescriptor descriptor{1024, CopyUsage};
    const auto plan = fixture.resources->QueryBufferMemoryCost(descriptor);
    REQUIRE(plan.HasValue());
    auto placement = fixture.Placement(plan.Value());
    placement.backingBytes -= 1;
    REQUIRE(fixture.resources->CreateBuffer(descriptor, {}, placement).HasError());
    REQUIRE(fixture.resources->Snapshot().resourceCount == 0);
    placement = fixture.Placement(plan.Value());
    const auto buffer = fixture.resources->CreateBuffer(descriptor, {}, placement);
    REQUIRE(buffer.HasValue());
    REQUIRE(fixture.resources->CreateBuffer(descriptor, {}, placement).HasError());
    REQUIRE(fixture.resources->Readback(buffer.Value(), 1024, 1).HasError());
    REQUIRE(fixture.resources->Readback(buffer.Value(), 0, 0).HasError());
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    REQUIRE(fixture.resources->Readback(buffer.Value(), 0, 1, cancellation.Token()).HasError());
    REQUIRE(fixture.resources->Snapshot().transferCount == 0);
    D3D12_COMMAND_QUEUE_DESC computeDescription{};
    computeDescription.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> compute;
    REQUIRE(SUCCEEDED(fixture.device->CreateCommandQueue(&computeDescription, IID_PPV_ARGS(&compute))));
    REQUIRE(CreateD3D12Resources(fixture.device.Get(), compute.Get(), {}).HasError());
    REQUIRE(CreateD3D12Resources(nullptr, fixture.queue.Get(), {}).HasError());
}

TEST_CASE("D3D12 WARP descriptor reuse waits for dependency retirement", "[renderer][d3d12][resources][warp]") {
    D3D12ResourceLimits limits;
    limits.maximumDescriptors = 1;
    Fixture fixture{limits};
    RenderTextureDescriptor texture;
    texture.extent = {8, 8};
    texture.usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
    const auto storage = fixture.Texture(texture);
    RenderTextureViewDescriptor view;
    view.texture = {{1}, 1, 1};
    const auto first = fixture.resources->CreateTextureView(view, storage);
    REQUIRE(first.HasValue());
    RenderTargetDescriptor target;
    target.colorAttachment = {{1}, 2, 1};
    target.extent = {8, 8};
    const auto framebuffer = fixture.resources->CreateRenderTarget(target, first.Value(), 0);
    REQUIRE(framebuffer.HasValue());
    fixture.resources->DestroyTextureView(first.Value());
    REQUIRE(fixture.resources->CreateTextureView(view, storage).HasError());
    fixture.resources->DestroyRenderTarget(framebuffer.Value());
    REQUIRE(fixture.resources->Poll().HasValue());
    const auto second = fixture.resources->CreateTextureView(view, storage);
    REQUIRE(second.HasValue());
    REQUIRE(second.Value() != first.Value());
    fixture.resources->DestroyTexture(storage);
    REQUIRE(fixture.resources->Snapshot().retiringBytes > 0);
    fixture.resources->DestroyTextureView(second.Value());
    REQUIRE(fixture.resources->Poll().HasValue());
    REQUIRE(fixture.resources->Snapshot().descriptorCount == 0);
    REQUIRE(fixture.resources->Snapshot().committedBytes == 0);
}

TEST_CASE("D3D12 WARP resource calls enforce owner thread and unsupported formats", "[renderer][d3d12][resources][warp]") {
    Fixture fixture;
    bool rejected = false;
    std::thread worker{[&] {
        const auto result = fixture.resources->QueryBufferMemoryCost({1024, CopyUsage});
        rejected = result.HasError() && result.ErrorValue().code.Value() == "render.d3d12.wrong_thread";
    }};
    worker.join();
    REQUIRE(rejected);
    RenderTextureDescriptor depth;
    depth.extent = {4, 4};
    depth.format = RenderTextureFormat::Depth32Float;
    depth.usage = RenderTextureUsage::Storage;
    REQUIRE(fixture.resources->QueryTextureMemoryCost(depth).HasError());
    REQUIRE(fixture.resources->QueryBufferMemoryCost({256, CopyUsage, RenderBufferAccess::HostVisible}).HasError());
    REQUIRE(fixture.resources->QueryBufferMemoryCost({255, RenderBufferUsage::Uniform}).HasError());
    const std::array initial{std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}};
    const auto immutable = fixture.Buffer({4, RenderBufferUsage::CopySource}, initial);
    REQUIRE(fixture.resources->Upload(immutable, initial, 0).HasError());
    const auto preserved = fixture.resources->Readback(immutable, 0, 4);
    REQUIRE(preserved.HasValue());
    REQUIRE(fixture.Read(preserved.Value()) == std::vector<std::byte>(initial.begin(), initial.end()));
    RenderSamplerDescriptor sampler;
    sampler.maximumAnisotropy = 32;
    REQUIRE(fixture.resources->QuerySamplerMemoryCost(sampler).HasError());
}

TEST_CASE("D3D12 WARP sampler storage and sampled-depth descriptors own their native slots", "[renderer][d3d12][resources][warp]") {
    Fixture fixture;
    RenderSamplerDescriptor sampler;
    sampler.maximumAnisotropy = 4;
    sampler.compareEnabled = true;
    const auto plan = fixture.resources->QuerySamplerMemoryCost(sampler);
    REQUIRE(plan.HasValue());
    const auto instance = fixture.resources->CreateSampler(sampler, fixture.Placement(plan.Value()));
    REQUIRE(instance.HasValue());
    const auto storageBuffer = fixture.Buffer({256, RenderBufferUsage::Storage | RenderBufferUsage::Uniform});
    REQUIRE(fixture.resources->Snapshot().descriptorCount == 3);
    RenderTextureDescriptor depth;
    depth.extent = {4, 4};
    depth.format = RenderTextureFormat::Depth32Float;
    depth.usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
    const auto texture = fixture.Texture(depth);
    RenderTextureViewDescriptor view;
    view.texture = {{1}, 1, 1};
    view.format = depth.format;
    view.aspect = RenderTextureAspect::Depth;
    const auto depthView = fixture.resources->CreateTextureView(view, texture);
    REQUIRE(depthView.HasValue());
    REQUIRE(fixture.resources->Snapshot().descriptorCount == 5);
    fixture.resources->DestroySampler(instance.Value());
    fixture.resources->DestroyBuffer(storageBuffer);
    fixture.resources->DestroyTextureView(depthView.Value());
    fixture.resources->DestroyTexture(texture);
    REQUIRE(fixture.resources->Poll().HasValue());
    REQUIRE(fixture.resources->Snapshot().descriptorCount == 0);
    REQUIRE(fixture.resources->Snapshot().committedBytes == 0);
    const auto residency = fixture.resources->QueryResidency();
    if (residency.HasError())
        REQUIRE(residency.ErrorValue().code.Value() == "render.d3d12.residency_unavailable");
}

TEST_CASE("D3D12 WARP host-visible initial bytes and finite resource capacity are explicit", "[renderer][d3d12][resources][warp]") {
    D3D12ResourceLimits limits;
    limits.maximumResources = 1;
    Fixture fixture{limits};
    const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const auto buffer = fixture.Buffer({bytes.size(), RenderBufferUsage::CopySource, RenderBufferAccess::HostVisible}, bytes);
    const auto overflowPlan = fixture.resources->QueryBufferMemoryCost({4, CopyUsage});
    REQUIRE(overflowPlan.HasValue());
    REQUIRE(fixture.resources->CreateBuffer({4, CopyUsage}, {}, fixture.Placement(overflowPlan.Value())).HasError());
    const auto read = fixture.resources->Readback(buffer, 0, bytes.size());
    REQUIRE(read.HasValue());
    REQUIRE(fixture.Read(read.Value()) == std::vector<std::byte>(bytes.begin(), bytes.end()));
    fixture.resources->DestroyBuffer(buffer);
    REQUIRE(fixture.resources->Poll().HasValue());
    REQUIRE(fixture.Buffer({4, CopyUsage}) != buffer);
}

TEST_CASE("D3D12 native color views preserve linear sRGB families and reject unsupported storage interpretations",
          "[renderer][d3d12][resources][warp]") {
    Fixture fixture;
    RenderTextureDescriptor descriptor;
    descriptor.extent = {3, 2};
    descriptor.usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopySource;
    const std::vector bytes(24, std::byte{127});
    const auto texture = fixture.Texture(descriptor, bytes);
    RenderTextureViewDescriptor srgb;
    srgb.texture = {{1}, 1, 1};
    srgb.format = RenderTextureFormat::Rgba8UnormSrgb;
    const auto view = fixture.resources->CreateTextureView(srgb, texture);
    REQUIRE(view.HasValue());
    const auto readback = fixture.resources->Readback(texture, 0, bytes.size());
    REQUIRE(readback.HasValue());
    REQUIRE(fixture.Read(readback.Value()) == bytes);
    fixture.resources->DestroyTextureView(view.Value());
    fixture.resources->DestroyTexture(texture);
    descriptor.usage = RenderTextureUsage::Sampled | RenderTextureUsage::Storage;
    const auto writable = fixture.Texture(descriptor);
    const auto unsupported = fixture.resources->CreateTextureView(srgb, writable);
    REQUIRE(unsupported.HasError());
    REQUIRE(unsupported.ErrorValue().code.Value() == "render.d3d12.view_unsupported");
}

TEST_CASE("D3D12 native descriptor creation is checked by the installed debug layer", "[renderer][d3d12][resources][warp][diagnostics]") {
    Fixture fixture;
    if (!fixture.debugEnabled)
        SKIP("D3D12 diagnostic layers are unavailable; native byte/lifetime tests run independently.");
    ComPtr<ID3D12InfoQueue> diagnostics;
    REQUIRE(SUCCEEDED(fixture.device.As(&diagnostics)));
    diagnostics->ClearStoredMessages();
    RenderTextureDescriptor descriptor;
    descriptor.extent = {4, 4};
    descriptor.layerCount = 2;
    descriptor.usage = RenderTextureUsage::Sampled | RenderTextureUsage::Storage | RenderTextureUsage::RenderAttachment;
    const auto texture = fixture.Texture(descriptor);
    RenderTextureViewDescriptor view;
    view.texture = {{1}, 1, 1};
    view.dimension = RenderTextureViewDimension::TwoDArray;
    view.layerCount = 2;
    REQUIRE(fixture.resources->CreateTextureView(view, texture).HasValue());
    for (UINT64 index = 0; index < diagnostics->GetNumStoredMessages(); ++index) {
        SIZE_T size = 0;
        REQUIRE(SUCCEEDED(diagnostics->GetMessage(index, nullptr, &size)));
        REQUIRE(size <= 1024U * 1024U);
        std::vector<std::byte> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        REQUIRE(SUCCEEDED(diagnostics->GetMessage(index, message, &size)));
        INFO(message->pDescription);
        REQUIRE(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR);
        REQUIRE(message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION);
    }
}

#endif
