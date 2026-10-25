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
#include <d3d12.h>
#include <dxgi1_6.h>
#include <limits>
#include <thread>
#include <utility>
#include <wrl/client.h>

namespace Horo::Render::D3D12Detail {
    using Microsoft::WRL::ComPtr;
    Error ResourceError(const char *stage, const char *message, HRESULT cause = S_OK);
    DXGI_FORMAT NativeFormat(RenderTextureFormat format);
    bool IsDepth(RenderTextureFormat format);
    D3D12_RESOURCE_DESC BufferShape(std::size_t bytes);
    /** @brief Queries checked native requirements, preserving device loss from void-returning D3D12 queries. */
    Result<D3D12_RESOURCE_ALLOCATION_INFO> QueryAllocationInfo(ID3D12Device &device, const D3D12_RESOURCE_DESC &shape);
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
            description.Flags = type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                                    ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                    : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
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

    /** @brief Opaque compatibility groups for explicit tier-1-safe native heap reuse. */
    enum class MemoryCompatibility : std::uint64_t {
        DeviceBuffer = 0xD31201U,
        Texture = 0xD31202U,
        Attachment = 0xD31203U,
        UploadBuffer = 0xD31204U,
        MultisampledAttachment = 0xD31205U,
        Sampler = 0xD31206U
    };

    /** @brief Owner-thread native heap lifetime, never a second reservation ledger. */
    struct HeapAllocation {
        RenderMemoryPoolId pool;
        RenderMemoryBlockId block;
        RenderMemoryScopeId scope;
        RenderMemoryClass memoryClass;
        RenderMemoryCompatibilityId compatibility;
        std::size_t capacity{0};
        std::size_t alignment{0};
        D3D12_HEAP_FLAGS flags{D3D12_HEAP_FLAG_NONE};
        std::size_t slot{std::numeric_limits<std::size_t>::max()};
        std::uint32_t allocationCount{0}; /**< Includes native allocations still retiring behind copy references. */
        ComPtr<ID3D12Heap> native;
    };

    /** @brief Resource metadata and native backing survive both dependency and transfer references. */
    struct Resource {
        /** @brief Takes the sole moved completion lease before acquiring COM backing. */
        explicit Resource(RenderResourceNativeLifetime nativeLifetime = {}) : lifetime(std::move(nativeLifetime)) {}

        Resource(const Resource &) = delete;
        Resource &operator=(const Resource &) = delete;

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
        bool activationPending{true}; /**< First GPU access activates placed memory, including reused heap ranges. */
        // Declared before native backing: reverse member destruction releases the heap before the lease.
        RenderResourceNativeLifetime lifetime;
        std::shared_ptr<HeapAllocation> heap;
        ComPtr<ID3D12Resource> native;
        RenderBufferDescriptor buffer;
        RenderTextureDescriptor texture;
        RenderTextureViewDescriptor view;
        RenderMeshDescriptor mesh;
        D3D12_VERTEX_BUFFER_VIEW vertexView{};
        D3D12_INDEX_BUFFER_VIEW indexView{};
        RenderTargetDescriptor target;
        RenderSamplerDescriptor sampler;
        std::size_t backingBytes{0};
        std::size_t offsetBytes{0};
        std::size_t requiredBytes{0};
        RenderMemoryPoolId pool;
        RenderMemoryBlockId block;
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
        Resources(ID3D12Device *device, ID3D12CommandQueue *queue, const D3D12ResourceLimits &limits, D3D12ResourceTestHooks testHooks);

        ~Resources() override;

        Result<void> Initialize();

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override;

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override;

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> bytes,
                                           const RenderMemoryPlacement &placement, RenderResourceNativeLifetime lifetime) override;

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, std::span<const std::byte> bytes,
                                            const RenderMemoryPlacement &placement, RenderResourceNativeLifetime lifetime) override;

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, std::uint64_t texture,
                                                RenderResourceNativeLifetime lifetime) override;
        Result<RenderMemoryCostPlan> QuerySamplerMemoryCost(const RenderSamplerDescriptor &descriptor) const override;
        Result<std::uint64_t> CreateSampler(const RenderSamplerDescriptor &descriptor, const RenderMemoryPlacement &placement) override;
        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertex, std::uint64_t index,
                                         RenderResourceNativeLifetime lifetime) override;
        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, std::uint64_t color, std::uint64_t depth,
                                                 RenderResourceNativeLifetime lifetime) override;

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

        /** @brief Retires an unpublished native record on typed failure or metadata-exception unwind. */
        class CreationRollback final {
        public:
            CreationRollback(Resources &owner, const Resource &resource) noexcept : owner_(owner), id_(resource.id), kind_(resource.kind) {}

            ~CreationRollback() {
                if (!committed_)
                    owner_.Retire(id_, kind_);
            }

            CreationRollback(const CreationRollback &) = delete;
            CreationRollback &operator=(const CreationRollback &) = delete;

            void Commit() noexcept {
                committed_ = true;
            }

        private:
            Resources &owner_;
            std::uint64_t id_;
            Resource::Kind kind_;
            bool committed_{false};
        };

        /** @brief Checks ownership without invoking native queries or allocating a device identity. */
        Result<void> CheckOwner() const;
        /** @brief Rejects stale transfer identity before any native completion counter is observed. */
        Result<void> CheckTransferIdentity(D3D12TransferId id) const;
        Result<void> Check(bool admission = true) const;

        Result<RenderMemoryCostPlan> Cost(const D3D12_RESOURCE_DESC &shape, std::size_t payload, RenderMemoryClass memoryClass) const;

        bool PlacementMatches(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) const;
        /** @brief Acquires only the exact previously admitted compatible native pool capacity. */
        Result<std::shared_ptr<HeapAllocation>> AcquireHeap(const D3D12_RESOURCE_DESC &shape, const RenderMemoryCostPlan &plan,
                                                            const RenderMemoryPlacement &placement);
        /** @brief Removes an empty heap's service reference before its final allocation lease is released. */
        void RemoveEmptyHeap(HeapAllocation &heap) noexcept;

        /** @brief Drops newly acquired empty backing if placed-resource creation fails or metadata unwinds. */
        struct HeapRollback {
            Resources &owner;
            HeapAllocation &heap;

            ~HeapRollback() {
                if (heap.allocationCount == 0)
                    owner.RemoveEmptyHeap(heap);
            }
        };

        Result<std::shared_ptr<Resource>> NewResource(Resource::Kind kind, RenderResourceNativeLifetime lifetime = {});

        Result<std::shared_ptr<Resource>> Allocate(const D3D12_RESOURCE_DESC &shape, const RenderMemoryCostPlan &plan,
                                                   const RenderMemoryPlacement &placement, Resource::Kind kind,
                                                   RenderResourceNativeLifetime lifetime);

        std::shared_ptr<Resource> Find(std::uint64_t id) const;

        void Retire(std::uint64_t id, Resource::Kind kind) noexcept;

        void Reclaim() noexcept;

        bool ValidRange(const Resource &resource, std::size_t offset, std::size_t bytes, bool readback) const;

        Transfer *FindTransfer(D3D12TransferId id);
        /** @brief Checks the readback fence without interpreting device removal as terminal success. */
        Result<bool> ReadbackReady(const Transfer &transfer);

        Result<D3D12TransferId> Submit(const std::shared_ptr<Resource> &resource, std::size_t offset, std::size_t bytes,
                                       std::span<const std::byte> payload, bool readback, const CancellationToken &cancellation);
        /** @brief Computes bounded per-layer base-level footprints, retaining native padding. */
        Result<void> PrepareFootprints(Transfer &transfer);
        /** @brief Prepares and closes a batch before irreversible submission. */
        Result<void> PrepareCopyCommands(Transfer &transfer, std::size_t offset);
        /** @brief Submits an already-owned batch; failure retains its backing and native retirement lease. */
        Result<void> SubmitCommands(Transfer &transfer);
        /** @brief Writes a validated initial payload or mutable copy destination before publication. */
        Result<D3D12TransferId> Write(const std::shared_ptr<Resource> &resource, std::span<const std::byte> bytes, std::size_t offset,
                                      const CancellationToken &cancellation);
        Result<void> PrepareStaging(Transfer &transfer, std::span<const std::byte> payload);
        /** @brief Creates constant/raw descriptors after size validation; record owns rollback. */
        Result<void> CreateBufferViews(Resource &resource);
        /** @brief Creates native image descriptors under the owning record rollback. */
        Result<void> CreateTextureDescriptors(Resource &resource, const Resource &storage);
        void RecordCopy(Transfer &transfer, std::size_t offset);

        ComPtr<ID3D12Device> device_;
        ComPtr<ID3D12CommandQueue> queue_;
        D3D12ResourceLimits limits_;
        D3D12ResourceTestHooks testHooks_;
        std::thread::id owner_{std::this_thread::get_id()};
        bool accepting_{true};
        std::uint64_t deviceIncarnation_{0};
        std::uint64_t nextResource_{1};
        std::uint64_t nextTransfer_{1};
        std::uint64_t nextFence_{1};
        ComPtr<ID3D12Fence> fence_;
        std::array<DescriptorPool, 4> pools_;
        std::vector<std::shared_ptr<HeapAllocation>> heapAllocations_;
        std::vector<std::shared_ptr<Resource>> resources_;
        std::vector<Transfer> transfers_;
    };
}  // namespace Horo::Render::D3D12Detail
#endif
