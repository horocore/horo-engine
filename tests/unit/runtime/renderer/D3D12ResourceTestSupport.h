#pragma once

/** @file D3D12ResourceTestSupport.h
 * @brief Shared real WARP device fixture and retirement probes for the resource test executable.
 */
#include "D3D12NativeResources.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32) && defined(_M_X64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace Horo::Render::D3D12Tests {

    using namespace Horo;
    using namespace Horo::Render;
    using Microsoft::WRL::ComPtr;

    inline bool EnableDebugLayerOnce() {
        static const bool enabled = [] {
            ComPtr<ID3D12Debug> debug;
            if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                return false;
            debug->EnableDebugLayer();
            return true;
        }();
        return enabled;
    }

    // WARP is explicitly selected by tests. This bypasses production host admission only
    // to test native resource ownership on CI; it is not a renderer fallback or qualification of startup.
    class Fixture {
    public:
        explicit Fixture(const D3D12ResourceLimits &limits = {}, D3D12ResourceTestHooks hooks = {}) {
            ComPtr<IDXGIFactory4> factory;
            REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
            ComPtr<IDXGIAdapter> warp;
            REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
            REQUIRE(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))));
            D3D12_COMMAND_QUEUE_DESC description{};
            description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            REQUIRE(SUCCEEDED(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue))));
            auto created = CreateD3D12Resources(device.Get(), queue.Get(), limits, hooks);
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

        std::uint64_t Buffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> bytes = {},
                             RenderResourceNativeLifetime lifetime = {}) {
            const auto plan = resources->QueryBufferMemoryCost(descriptor);
            REQUIRE(plan.HasValue());
            const auto created = resources->CreateBuffer(descriptor, bytes, Placement(plan.Value()), std::move(lifetime));
            REQUIRE(created.HasValue());
            return created.Value();
        }

        std::uint64_t Texture(const RenderTextureDescriptor &descriptor, std::span<const std::byte> bytes = {},
                              RenderResourceNativeLifetime lifetime = {}) {
            const auto plan = resources->QueryTextureMemoryCost(descriptor);
            REQUIRE(plan.HasValue());
            const auto created = resources->CreateTexture(descriptor, bytes, Placement(plan.Value()), std::move(lifetime));
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

        const bool debugEnabled{EnableDebugLayerOnce()};
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        std::unique_ptr<ID3D12Resources> resources;
        std::uint64_t nextPool{1};
    };

    struct Lease final : IRenderResourceRetirementLease {
        std::uint32_t claims{0};
        std::uint32_t releases{0};
        std::thread::id releasedOn;

        void Claim() noexcept override {
            ++claims;
        }

        void Release() noexcept override {
            ++releases;
            releasedOn = std::this_thread::get_id();
        }
    };

    inline void RemoveAfterExecute(ID3D12Device *device) noexcept {
        ComPtr<ID3D12Device5> removable;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&removable))))
            removable->RemoveDevice();
    }

    inline constexpr RenderBufferUsage CopyUsage = RenderBufferUsage::CopySource | RenderBufferUsage::CopyDestination;
}  // namespace Horo::Render::D3D12Tests
#endif
