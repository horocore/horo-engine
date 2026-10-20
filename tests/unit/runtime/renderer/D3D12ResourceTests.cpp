#include "runtime/renderer/modules/d3d12/D3D12Initialization.h"
#include "runtime/renderer/modules/d3d12/D3D12NativeResources.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

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
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using Microsoft::WRL::ComPtr;

    // WARP is explicitly selected by tests. This bypasses production host admission only
    // to test native resource ownership on CI; it is not a renderer fallback or qualification of startup.
    class Fixture {
    public:
        explicit Fixture(const D3D12ResourceLimits &limits = {}) {
            ComPtr<IDXGIFactory4> factory;
            REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
            ComPtr<IDXGIAdapter> warp;
            REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
            REQUIRE(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))));
            D3D12_COMMAND_QUEUE_DESC description{};
            description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            REQUIRE(SUCCEEDED(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue))));
            auto created = CreateD3D12Resources(device.Get(), queue.Get(), limits);
            REQUIRE(created.HasValue());
            resources = std::move(created).Value();
        }

        ~Fixture() {
            if (!resources)
                return;
            resources->StopAdmission();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (std::chrono::steady_clock::now() < deadline) {
                const auto drain = resources->PollDrain();
                if (drain.HasError() || drain.Value())
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }

        RenderMemoryPlacement Placement(const RenderMemoryCostPlan &plan) {
            return {.pool = {{1}, nextPool++},
                    .scope = {1, 1},
                    .attempt = {nextPool},
                    .memoryClass = plan.memoryClass,
                    .compatibility = plan.compatibility,
                    .provenance = plan.provenance,
                    .budgetRevision = 1,
                    .offsetBytes = 0,
                    .payloadBytes = plan.payloadBytes,
                    .requiredBytes = plan.requiredBytes,
                    .backingBytes = plan.requiredBytes,
                    .allocationClass = plan.allocationClass};
        }

        std::uint64_t Buffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> bytes = {}) {
            const auto plan = resources->QueryBufferMemoryCost(descriptor);
            REQUIRE(plan.HasValue());
            const auto created = resources->CreateBuffer(descriptor, bytes, Placement(plan.Value()));
            REQUIRE(created.HasValue());
            return created.Value();
        }

        std::uint64_t Texture(const RenderTextureDescriptor &descriptor, std::span<const std::byte> bytes = {}) {
            const auto plan = resources->QueryTextureMemoryCost(descriptor);
            REQUIRE(plan.HasValue());
            const auto created = resources->CreateTexture(descriptor, bytes, Placement(plan.Value()));
            REQUIRE(created.HasValue());
            return created.Value();
        }

        std::vector<std::byte> Read(D3D12TransferId id) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (std::chrono::steady_clock::now() < deadline) {
                auto result = resources->TakeReadback(id);
                if (result.HasValue())
                    return std::move(result).Value();
                REQUIRE(result.ErrorValue().code.Value() == "render.d3d12.transfer_pending");
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("Bounded WARP readback timed out");
            return {};
        }

        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        std::unique_ptr<ID3D12Resources> resources;
        std::uint64_t nextPool{1};
    };

    constexpr RenderBufferUsage CopyUsage = RenderBufferUsage::CopySource | RenderBufferUsage::CopyDestination;
}  // namespace

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

TEST_CASE("D3D12 WARP transfer capacity cancellation and shutdown preserve fence ownership", "[renderer][d3d12][resources][warp]") {
    D3D12ResourceLimits limits;
    limits.maximumTransfers = 1;
    limits.maximumTransferBytes = 65536;
    Fixture fixture{limits};
    const auto buffer = fixture.Buffer({65536, CopyUsage});
    const auto first = fixture.resources->Readback(buffer, 0, 16);
    REQUIRE(first.HasValue());
    REQUIRE(fixture.resources->Readback(buffer, 0, 16).HasError());
    REQUIRE(fixture.resources->Cancel(first.Value()).HasValue());
    REQUIRE(fixture.resources->TakeReadback(first.Value()).HasError());
    fixture.resources->DestroyBuffer(buffer);
    fixture.resources->StopAdmission();
    REQUIRE(fixture.resources->QueryBufferMemoryCost({1, CopyUsage}).HasError());
    REQUIRE_FALSE(fixture.resources->Snapshot().accepting);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    bool drained = false;
    while (std::chrono::steady_clock::now() < deadline) {
        auto poll = fixture.resources->PollDrain();
        REQUIRE(poll.HasValue());
        if (poll.Value()) {
            drained = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(drained);
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

TEST_CASE("D3D12 WARP device removal remains a typed failure instead of false completion", "[renderer][d3d12][resources][warp]") {
    Fixture fixture;
    const auto buffer = fixture.Buffer({256, CopyUsage});
    REQUIRE(fixture.resources->Readback(buffer, 0, 256).HasValue());
    ComPtr<ID3D12Device5> removable;
    REQUIRE(SUCCEEDED(fixture.device.As(&removable)));
    removable->RemoveDevice();
    const auto result = fixture.resources->Poll();
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "render.d3d12.device_lost");
    REQUIRE(fixture.resources->QueryBufferMemoryCost({256, CopyUsage}).HasError());
}
#endif
