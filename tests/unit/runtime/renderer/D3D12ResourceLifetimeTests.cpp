#include "D3D12ResourceTestSupport.h"

#if defined(_WIN32) && defined(_M_X64)
using namespace Horo;
using namespace Horo::Render;
using namespace Horo::Render::D3D12Tests;

TEST_CASE("D3D12 WARP transfer capacity cancellation and shutdown preserve fence ownership", "[renderer][d3d12][resources][warp]") {
    D3D12ResourceLimits limits;
    limits.maximumTransfers = 1;
    limits.maximumTransferBytes = 65536;
    Lease lease;
    Fixture fixture{limits};
    const auto buffer = fixture.Buffer({65536, CopyUsage}, {}, RenderResourceNativeLifetime{lease});
    const auto first = fixture.resources->Readback(buffer, 0, 16);
    REQUIRE(first.HasValue());
    REQUIRE(fixture.resources->Readback(buffer, 0, 16).HasError());
    REQUIRE(fixture.resources->Cancel(first.Value()).HasValue());
    fixture.resources->DestroyBuffer(buffer);
    REQUIRE(lease.claims == 1);
    REQUIRE(lease.releases == 0);
    REQUIRE(fixture.resources->TakeReadback(first.Value()).HasError());
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
    REQUIRE(lease.releases == 1);
    REQUIRE(fixture.resources->Snapshot().committedBytes == 0);
}

TEST_CASE("D3D12 compatible placements share one charged heap until its final native allocation retires",
          "[renderer][d3d12][resources][warp][lifetime]") {
    Lease firstLease;
    Lease secondLease;
    Fixture fixture;
    const RenderBufferDescriptor descriptor{256, CopyUsage};
    const auto plan = fixture.resources->QueryBufferMemoryCost(descriptor);
    REQUIRE(plan.HasValue());
    REQUIRE(plan.Value().allocationClass == RenderMemoryAllocationClass::Suballocated);
    auto firstPlacement = fixture.Placement(plan.Value());
    firstPlacement.backingBytes = 2 * plan.Value().requiredBytes;
    auto secondPlacement = firstPlacement;
    secondPlacement.attempt = {firstPlacement.attempt.value + 1};
    secondPlacement.offsetBytes = plan.Value().requiredBytes;
    const auto first = fixture.resources->CreateBuffer(descriptor, {}, firstPlacement, RenderResourceNativeLifetime{firstLease});
    REQUIRE(first.HasValue());
    const auto second = fixture.resources->CreateBuffer(descriptor, {}, secondPlacement, RenderResourceNativeLifetime{secondLease});
    REQUIRE(second.HasValue());
    REQUIRE(fixture.resources->Snapshot().committedBytes == firstPlacement.backingBytes);
    REQUIRE(fixture.resources->CreateBuffer(descriptor, {}, firstPlacement).HasError());
    auto wrongCapacity = secondPlacement;
    wrongCapacity.offsetBytes += plan.Value().requiredBytes;
    wrongCapacity.backingBytes += plan.Value().requiredBytes;
    REQUIRE(fixture.resources->CreateBuffer(descriptor, {}, wrongCapacity).HasError());
    fixture.resources->DestroyBuffer(first.Value());
    REQUIRE(firstLease.releases == 1);
    REQUIRE(secondLease.releases == 0);
    REQUIRE(fixture.resources->Snapshot().committedBytes == firstPlacement.backingBytes);
    const std::array<std::byte, 256> bytes{std::byte{42}};
    const auto reused = fixture.resources->CreateBuffer(descriptor, bytes, firstPlacement);
    REQUIRE(reused.HasValue());
    const auto readback = fixture.resources->Readback(reused.Value(), 0, bytes.size());
    REQUIRE(readback.HasValue());
    REQUIRE(fixture.Read(readback.Value()) == std::vector<std::byte>(bytes.begin(), bytes.end()));
    fixture.resources->DestroyBuffer(reused.Value());
    fixture.resources->DestroyBuffer(second.Value());
    REQUIRE(secondLease.releases == 1);
    REQUIRE(fixture.resources->Snapshot().committedBytes == 0);
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

TEST_CASE("D3D12 native buffer retirement releases one frontend lease only after real copy completion",
          "[renderer][d3d12][resources][warp][lifetime]") {
    Lease lease;
    Fixture fixture;
    const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const auto buffer = fixture.Buffer({4, CopyUsage}, bytes, RenderResourceNativeLifetime{lease});
    REQUIRE(lease.claims == 1);
    REQUIRE(lease.releases == 0);
    const auto readback = fixture.resources->Readback(buffer, 0, bytes.size());
    REQUIRE(readback.HasValue());
    fixture.resources->DestroyBuffer(buffer);
    REQUIRE(lease.releases == 0);
    REQUIRE(fixture.Read(readback.Value()) == std::vector<std::byte>(bytes.begin(), bytes.end()));
    REQUIRE(fixture.resources->Poll().HasValue());
    REQUIRE(lease.releases == 1);
    REQUIRE(lease.releasedOn == std::this_thread::get_id());
    fixture.resources->DestroyBuffer(buffer);
    REQUIRE(lease.releases == 1);
}

TEST_CASE("D3D12 native texture view and target dependencies preserve the same frontend leases",
          "[renderer][d3d12][resources][warp][lifetime]") {
    Lease textureLease;
    Lease viewLease;
    Lease targetLease;
    Fixture fixture;
    RenderTextureDescriptor descriptor;
    descriptor.extent = {3, 2};
    descriptor.usage = RenderTextureUsage::CopySource | RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
    std::vector<std::byte> bytes(24, std::byte{42});
    const auto texture = fixture.Texture(descriptor, bytes, RenderResourceNativeLifetime{textureLease});
    RenderTextureViewDescriptor view;
    view.texture = {{1}, 1, 1};
    const auto nativeView = fixture.resources->CreateTextureView(view, texture, RenderResourceNativeLifetime{viewLease});
    REQUIRE(nativeView.HasValue());
    RenderTargetDescriptor target;
    target.extent = descriptor.extent;
    target.colorAttachment = {{1}, 2, 1};
    const auto nativeTarget =
        fixture.resources->CreateRenderTarget(target, nativeView.Value(), 0, RenderResourceNativeLifetime{targetLease});
    REQUIRE(nativeTarget.HasValue());
    const auto readback = fixture.resources->Readback(texture, 0, bytes.size());
    REQUIRE(readback.HasValue());
    fixture.resources->DestroyTexture(texture);
    fixture.resources->DestroyTextureView(nativeView.Value());
    REQUIRE(textureLease.claims == 1);
    REQUIRE(textureLease.releases == 0);
    REQUIRE(viewLease.releases == 0);
    fixture.resources->DestroyRenderTarget(nativeTarget.Value());
    REQUIRE(fixture.resources->Poll().HasValue());
    REQUIRE(targetLease.claims == 1);
    REQUIRE(targetLease.releases == 1);
    REQUIRE(viewLease.claims == 1);
    REQUIRE(viewLease.releases == 1);
    REQUIRE(textureLease.releases == 0);
    REQUIRE(fixture.Read(readback.Value()) == bytes);
    REQUIRE(textureLease.releases == 1);
}

TEST_CASE("D3D12 rejected creation releases a claimed lease without native work", "[renderer][d3d12][resources][warp][lifetime]") {
    Lease lease;
    Fixture fixture;
    const RenderBufferDescriptor descriptor{256, CopyUsage};
    const auto plan = fixture.resources->QueryBufferMemoryCost(descriptor);
    REQUIRE(plan.HasValue());
    auto placement = fixture.Placement(plan.Value());
    placement.backingBytes -= 1;
    REQUIRE(fixture.resources->CreateBuffer(descriptor, {}, placement, RenderResourceNativeLifetime{lease}).HasError());
    REQUIRE(lease.claims == 1);
    REQUIRE(lease.releases == 1);
    REQUIRE(fixture.resources->Snapshot().committedBytes == 0);
}

TEST_CASE("D3D12 failed creation after actual Execute retains its lease through native disposal",
          "[renderer][d3d12][resources][warp][lifetime]") {
    Lease lease;
    Fixture fixture{{}, {.afterExecute = RemoveAfterExecute}};
    const RenderBufferDescriptor descriptor{256, CopyUsage};
    const auto plan = fixture.resources->QueryBufferMemoryCost(descriptor);
    REQUIRE(plan.HasValue());
    const std::array<std::byte, 256> bytes{};
    const auto created =
        fixture.resources->CreateBuffer(descriptor, bytes, fixture.Placement(plan.Value()), RenderResourceNativeLifetime{lease});
    REQUIRE(created.HasError());
    REQUIRE(FAILED(fixture.device->GetDeviceRemovedReason()));
    REQUIRE(created.ErrorValue().code.Value() == "render.d3d12.device_lost");
    REQUIRE(lease.claims == 1);
    REQUIRE(lease.releases == 0);
    REQUIRE(fixture.resources->Snapshot().committedBytes > 0);
    fixture.resources->StopAdmission();
    REQUIRE(fixture.resources->PollDrain().HasError());
    REQUIRE(lease.releases == 0);
    fixture.resources.reset();
    REQUIRE(lease.releases == 1);
}

TEST_CASE("D3D12 native mesh owns buffer dependencies until its one canonical lease retires",
          "[renderer][d3d12][resources][warp][lifetime]") {
    Lease verticesLease;
    Lease indicesLease;
    Lease meshLease;
    Fixture fixture;
    const auto vertices = fixture.Buffer({36, RenderBufferUsage::Vertex}, {}, RenderResourceNativeLifetime{verticesLease});
    const auto indices = fixture.Buffer({6, RenderBufferUsage::Index}, {}, RenderResourceNativeLifetime{indicesLease});
    RenderMeshDescriptor mesh;
    mesh.vertexBuffer = {{1}, 1, 1};
    mesh.indexBuffer = {{1}, 2, 1};
    mesh.vertexStride = 12;
    mesh.vertexCount = 3;
    mesh.indexFormat = RenderIndexFormat::UInt16;
    mesh.indexCount = 3;
    mesh.localBounds = {{0, 0, 0}, {1, 1, 1}};
    const auto created = fixture.resources->CreateMesh(mesh, vertices, indices, RenderResourceNativeLifetime{meshLease});
    REQUIRE(created.HasValue());
    fixture.resources->DestroyBuffer(vertices);
    fixture.resources->DestroyBuffer(indices);
    REQUIRE(verticesLease.releases == 0);
    REQUIRE(indicesLease.releases == 0);
    fixture.resources->DestroyMesh(created.Value());
    REQUIRE(fixture.resources->Poll().HasValue());
    REQUIRE(meshLease.claims == 1);
    REQUIRE(meshLease.releases == 1);
    REQUIRE(verticesLease.releases == 1);
    REQUIRE(indicesLease.releases == 1);
    REQUIRE(fixture.resources->Snapshot().committedBytes == 0);
}

TEST_CASE("D3D12 stale transfer value one cannot alias a new native device incarnation", "[renderer][d3d12][resources][warp][lifecycle]") {
    Fixture original;
    const auto firstBuffer = original.Buffer({16, CopyUsage});
    const auto stale = original.resources->Readback(firstBuffer, 0, 16);
    REQUIRE(stale.HasValue());
    REQUIRE(stale.Value().value == 1);
    Fixture replacement;
    const auto secondBuffer = replacement.Buffer({16, CopyUsage});
    const auto current = replacement.resources->Readback(secondBuffer, 0, 16);
    REQUIRE(current.HasValue());
    REQUIRE(current.Value().value == 1);
    REQUIRE(stale.Value().deviceIncarnation != current.Value().deviceIncarnation);
    REQUIRE(replacement.resources->Snapshot().deviceIncarnation == current.Value().deviceIncarnation);
    const auto rejected = replacement.resources->TakeReadback(stale.Value());
    REQUIRE(rejected.HasError());
    REQUIRE(rejected.ErrorValue().code.Value() == "render.d3d12.transfer_invalid");
    REQUIRE(replacement.resources->Cancel(stale.Value()).HasError());
    REQUIRE(replacement.resources->Snapshot().transferCount == 1);
    REQUIRE(replacement.Read(current.Value()).size() == 16);
}

#endif
