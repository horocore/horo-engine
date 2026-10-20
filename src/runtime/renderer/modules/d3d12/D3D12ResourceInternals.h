#pragma once

/** @file D3D12ResourceInternals.h
 * @brief Target-private native resource ownership and implementation declarations.
 */
#include "D3D12Initialization.h"
#include "D3D12NativeResources.h"

#if defined(_WIN32) && defined(_M_X64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <limits>
#include <thread>
#include <wrl/client.h>

namespace Horo::Render::D3D12Detail {
    using Microsoft::WRL::ComPtr;
    Error ResourceError(const char *stage, const char *message, HRESULT cause = S_OK);
    DXGI_FORMAT NativeFormat(RenderTextureFormat format);
    bool IsDepth(RenderTextureFormat format);
    D3D12_RESOURCE_DESC BufferShape(std::size_t bytes);
    D3D12_RESOURCE_DESC TextureShape(const RenderTextureDescriptor &descriptor);
    DXGI_FORMAT ShaderViewFormat(const RenderTextureViewDescriptor &view);
    D3D12_SHADER_RESOURCE_VIEW_DESC ShaderView(const RenderTextureViewDescriptor &view, bool multisampled);
    D3D12_RENDER_TARGET_VIEW_DESC ColorView(const RenderTextureViewDescriptor &view, const RenderTextureDescriptor &texture);
    D3D12_DEPTH_STENCIL_VIEW_DESC DepthView(const RenderTextureViewDescriptor &view, const RenderTextureDescriptor &texture);
    D3D12_UNORDERED_ACCESS_VIEW_DESC StorageView(const RenderTextureViewDescriptor &view, const RenderTextureDescriptor &texture);

    /** @brief Owns finite CPU-visible descriptor indices; free slots are reused only after retirement. */
    struct DescriptorPool {
        ComPtr<ID3D12DescriptorHeap> heap;
        std::vector<bool> occupied;
        UINT stride{0};

        Result<void> Initialize(ID3D12Device &device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count) {
            D3D12_DESCRIPTOR_HEAP_DESC description{};
            description.Type = type;
            description.NumDescriptors = count;
            const HRESULT status = device.CreateDescriptorHeap(&description, IID_PPV_ARGS(&heap));
            if (FAILED(status))
                return Result<void>::Failure(
                    ResourceError("render.d3d12.descriptor_heap_failed", "Create bounded descriptor storage.", status));
            stride = device.GetDescriptorHandleIncrementSize(type);
            occupied.resize(count, false);
            return Result<void>::Success();
        }

        std::optional<UINT> Allocate() {
            for (UINT index = 0; index < occupied.size(); ++index) {
                if (!occupied[index]) {
                    occupied[index] = true;
                    return index;
                }
            }
            return std::nullopt;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE Handle(UINT index) const {
            auto result = heap->GetCPUDescriptorHandleForHeapStart();
            result.ptr += static_cast<SIZE_T>(index) * stride;
            return result;
        }
    };

    /** @brief Resource metadata and native backing survive both dependency and transfer references. */
    struct Resource {
        enum class Kind {
            Buffer,
            Texture,
            View,
            Sampler,
            Mesh,
            Target
        };
        std::uint64_t id{0};
        Kind kind{Kind::Buffer};
        bool retired{false};
        ComPtr<ID3D12Heap> heap;
        ComPtr<ID3D12Resource> native;
        RenderBufferDescriptor buffer;
        RenderTextureDescriptor texture;
        RenderTextureViewDescriptor view;
        std::size_t backingBytes{0};
        RenderMemoryPoolId pool;
        std::array<std::optional<UINT>, 5> descriptors;
        std::vector<std::shared_ptr<Resource>> dependencies;
    };

    /** @brief One immutable copy batch with fence-owned allocator, staging and source/destination lifetime. */
    struct Transfer {
        D3D12TransferId id;
        std::uint64_t fence{0};
        bool readback{false};
        bool cancelled{false};
        bool signalPending{false};
        std::size_t backingBytes{0};
        std::size_t payloadBytes{0};
        std::shared_ptr<Resource> resource;
        ComPtr<ID3D12Resource> staging;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    };

    /** @brief Owns exact initialized device resources; no host/backend discovery is performed here. */
    class Resources final : public ID3D12Resources {
    public:
        Resources(ID3D12Device *device, ID3D12CommandQueue *queue, const D3D12ResourceLimits &limits);

        ~Resources() override;

        Result<void> Initialize();

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override;

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override;

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> bytes,
                                           const RenderMemoryPlacement &placement) override;

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, std::span<const std::byte> bytes,
                                            const RenderMemoryPlacement &placement) override;

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, std::uint64_t texture) override;
        Result<RenderMemoryCostPlan> QuerySamplerMemoryCost(const RenderSamplerDescriptor &descriptor) const override;
        Result<std::uint64_t> CreateSampler(const RenderSamplerDescriptor &descriptor, const RenderMemoryPlacement &placement) override;
        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertex, std::uint64_t index) override;
        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, std::uint64_t color,
                                                 std::uint64_t depth) override;

        void DestroyBuffer(std::uint64_t id) noexcept override;

        void DestroyTexture(std::uint64_t id) noexcept override;

        void DestroyTextureView(std::uint64_t id) noexcept override;

        void DestroySampler(std::uint64_t id) noexcept override;

        void DestroyMesh(std::uint64_t id) noexcept override;

        void DestroyRenderTarget(std::uint64_t id) noexcept override;

        Result<D3D12TransferId> Upload(std::uint64_t instance, std::span<const std::byte> bytes, std::size_t offset,
                                       const CancellationToken &cancellation) override;

        Result<D3D12TransferId> Readback(std::uint64_t instance, std::size_t offset, std::size_t bytes,
                                         const CancellationToken &cancellation) override;

        Result<std::vector<std::byte>> TakeReadback(D3D12TransferId transfer) override;

        Result<void> Cancel(D3D12TransferId transfer) override;

        Result<void> Poll() override;

        void StopAdmission() noexcept override;

        Result<bool> PollDrain() override;

        Result<D3D12ResidencyObservation> QueryResidency() const override;

        D3D12ResourceSnapshot Snapshot() const noexcept override;

    private:
        template <typename T> static Result<T> Failure(const char *code, const char *message) {
            return Result<T>::Failure(ResourceError(code, message));
        }

        Result<void> Check(bool admission = true) const;

        Result<RenderMemoryCostPlan> Cost(const D3D12_RESOURCE_DESC &shape, std::size_t payload, RenderMemoryClass memoryClass) const;

        bool PlacementMatches(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) const;

        Result<std::shared_ptr<Resource>> NewResource(Resource::Kind kind);

        Result<std::shared_ptr<Resource>> Allocate(const D3D12_RESOURCE_DESC &shape, const RenderMemoryCostPlan &plan,
                                                   const RenderMemoryPlacement &placement, Resource::Kind kind);

        std::shared_ptr<Resource> Find(std::uint64_t id) const;

        void Retire(std::uint64_t id, Resource::Kind kind) noexcept;

        void Reclaim() noexcept;

        bool ValidRange(const Resource &resource, std::size_t offset, std::size_t bytes, bool readback) const;

        Transfer *FindTransfer(D3D12TransferId id);

        Result<D3D12TransferId> Submit(const std::shared_ptr<Resource> &resource, std::size_t offset, std::size_t bytes,
                                       std::span<const std::byte> payload, bool readback, const CancellationToken &cancellation);
        /** @brief Computes bounded per-layer base-level footprints, retaining native padding. */
        Result<void> PrepareFootprints(Transfer &transfer);
        /** @brief Prepares and closes a batch before irreversible submission. */
        Result<void> PrepareCopyCommands(Transfer &transfer, std::size_t offset);
        Result<void> PrepareStaging(Transfer &transfer, std::span<const std::byte> payload);
        /** @brief Creates constant/raw descriptors after size validation; record owns rollback. */
        Result<void> CreateBufferViews(Resource &resource);
        /** @brief Creates native image descriptors under the owning record rollback. */
        Result<void> CreateTextureDescriptors(Resource &resource, const Resource &storage);
        void RecordCopy(Transfer &transfer, std::size_t offset);

        ComPtr<ID3D12Device> device_;
        ComPtr<ID3D12CommandQueue> queue_;
        D3D12ResourceLimits limits_;
        std::thread::id owner_{std::this_thread::get_id()};
        bool accepting_{true};
        std::uint64_t nextResource_{1};
        std::uint64_t nextTransfer_{1};
        std::uint64_t nextFence_{1};
        ComPtr<ID3D12Fence> fence_;
        std::array<DescriptorPool, 4> pools_;
        std::vector<std::shared_ptr<Resource>> resources_;
        std::vector<Transfer> transfers_;
    };
}  // namespace Horo::Render::D3D12Detail
#endif
