#include "D3D12ResourceInternals.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {
    /** @copydoc Resources::Resources */
    Resources::Resources(ID3D12Device *device, ID3D12CommandQueue *queue, const D3D12ResourceLimits &limits)
        : device_(device), queue_(queue), limits_(limits) {
        resources_.reserve(limits.maximumResources);
        transfers_.reserve(limits.maximumTransfers);
    }

    /** @copydoc Resources::~Resources */
    Resources::~Resources() {
        HORO_INVARIANT_MSG(owner_ == std::this_thread::get_id(), "Destroy D3D12 resources on their owner thread.");
        // A real removed device can no longer execute outstanding work. Otherwise the host must drain.
        HORO_INVARIANT_MSG(transfers_.empty() || FAILED(device_->GetDeviceRemovedReason()), "Drain D3D12 transfers before destruction.");
    }

    /** @copydoc Resources::Initialize */
    Result<void> Resources::Initialize() {
        for (UINT index = 0; index < pools_.size(); ++index) {
            auto result =
                pools_[index].Initialize(*device_.Get(), static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(index), limits_.maximumDescriptors);
            if (result.HasError())
                return result;
        }
        const HRESULT status = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(status))
            return Result<void>::Failure(ResourceError("render.d3d12.fence_failed", "Create transfer completion fence.", status));
        return Result<void>::Success();
    }

    /** @copydoc Resources::DestroyBuffer */
    void Resources::DestroyBuffer(std::uint64_t id) noexcept {
        Retire(id, Resource::Kind::Buffer);
    }

    /** @copydoc Resources::DestroyTexture */
    void Resources::DestroyTexture(std::uint64_t id) noexcept {
        Retire(id, Resource::Kind::Texture);
    }

    /** @copydoc Resources::DestroyTextureView */
    void Resources::DestroyTextureView(std::uint64_t id) noexcept {
        Retire(id, Resource::Kind::View);
    }

    /** @copydoc Resources::DestroySampler */
    void Resources::DestroySampler(std::uint64_t id) noexcept {
        Retire(id, Resource::Kind::Sampler);
    }

    /** @copydoc Resources::DestroyMesh */
    void Resources::DestroyMesh(std::uint64_t id) noexcept {
        Retire(id, Resource::Kind::Mesh);
    }

    /** @copydoc Resources::DestroyRenderTarget */
    void Resources::DestroyRenderTarget(std::uint64_t id) noexcept {
        Retire(id, Resource::Kind::Target);
    }

    /** @copydoc Resources::Poll */
    Result<void> Resources::Poll() {
        if (auto checked = Check(false); checked.HasError())
            return checked;
        for (auto &transfer : transfers_) {
            if (transfer.signalPending) {
                const HRESULT status = queue_->Signal(fence_.Get(), transfer.fence);
                if (FAILED(status))
                    return Result<void>::Failure(ResourceError("render.d3d12.signal_failed",
                                                               "Retry completion signaling; native references remain retained.", status));
                transfer.signalPending = false;
            }
        }
        const auto completed = fence_->GetCompletedValue();
        if (completed == std::numeric_limits<UINT64>::max())
            return Result<void>::Failure(
                ResourceError("render.d3d12.device_lost", "The transfer fence reports device removal.", DXGI_ERROR_DEVICE_REMOVED));
        std::erase_if(transfers_, [&](const Transfer &entry) {
            return entry.fence <= completed && (!entry.readback || entry.cancelled);
        });
        Reclaim();
        return Result<void>::Success();
    }

    /** @copydoc Resources::StopAdmission */
    void Resources::StopAdmission() noexcept {
        HORO_INVARIANT_MSG(owner_ == std::this_thread::get_id(), "Stop resource admission on the render owner thread.");
        accepting_ = false;
        for (auto &entry : transfers_)
            entry.cancelled = true;
    }

    /** @copydoc Resources::PollDrain */
    Result<bool> Resources::PollDrain() {
        if (auto checked = Check(false); checked.HasError())
            return Result<bool>::Failure(checked.ErrorValue());
        if (accepting_)
            return Failure<bool>("render.d3d12.invalid_state", "Stop admission before polling shutdown drain.");
        auto result = Poll();
        if (result.HasError())
            return Result<bool>::Failure(result.ErrorValue());
        return Result<bool>::Success(transfers_.empty());
    }

    /** @copydoc Resources::QueryResidency */
    Result<D3D12ResidencyObservation> Resources::QueryResidency() const {
        if (auto checked = Check(false); checked.HasError())
            return Result<D3D12ResidencyObservation>::Failure(checked.ErrorValue());
        ComPtr<IDXGIFactory4> factory;
        HRESULT status = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter3> adapter;
        if (SUCCEEDED(status))
            status = factory->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
        if (SUCCEEDED(status))
            status = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
        if (SUCCEEDED(status))
            status = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
        if (FAILED(status))
            return Result<D3D12ResidencyObservation>::Failure(
                ResourceError("render.d3d12.residency_unavailable", "DXGI cannot observe this adapter's current process budgets.", status));
        return Result<D3D12ResidencyObservation>::Success({local.CurrentUsage, local.Budget, nonLocal.CurrentUsage, nonLocal.Budget});
    }

    /** @copydoc Resources::Snapshot */
    D3D12ResourceSnapshot Resources::Snapshot() const noexcept {
        HORO_INVARIANT_MSG(owner_ == std::this_thread::get_id(), "Read resource accounting on the owner thread.");
        D3D12ResourceSnapshot result;
        result.accepting = accepting_;
        result.estimatedDescriptorHeapBytes = pools_.size() * 65536U;
        result.resourceCount = static_cast<std::uint32_t>(resources_.size());
        result.transferCount = static_cast<std::uint32_t>(transfers_.size());
        for (const auto &resource : resources_) {
            result.committedBytes += resource->backingBytes;
            if (resource->retired)
                result.retiringBytes += resource->backingBytes;
            for (const auto &descriptor : resource->descriptors)
                if (descriptor)
                    ++result.descriptorCount;
        }
        for (const auto &transfer : transfers_) {
            result.committedBytes += transfer.backingBytes;
            (transfer.readback ? result.readbackBytes : result.uploadBytes) += transfer.backingBytes;
        }
        return result;
    }

    /** @copydoc Resources::Check */
    Result<void> Resources::Check(bool admission) const {
        if (owner_ != std::this_thread::get_id())
            return Failure<void>("render.d3d12.wrong_thread", "Use the initialized device's render owner thread.");
        if (admission && !accepting_)
            return Failure<void>("render.d3d12.stopping", "Resource admission is closed during shutdown.");
        const HRESULT status = device_->GetDeviceRemovedReason();
        if (FAILED(status))
            return Result<void>::Failure(
                ResourceError("render.d3d12.device_lost", "Recreate the lost device through host recovery.", status));
        return Result<void>::Success();
    }

    /** @copydoc Resources::NewResource */
    Result<std::shared_ptr<Resource>> Resources::NewResource(Resource::Kind kind) {
        if (auto checked = Check(); checked.HasError())
            return Result<std::shared_ptr<Resource>>::Failure(checked.ErrorValue());
        Reclaim();
        if (resources_.size() == limits_.maximumResources || nextResource_ == std::numeric_limits<std::uint64_t>::max())
            return Failure<std::shared_ptr<Resource>>("render.d3d12.resource_capacity",
                                                      "Retire resources before requesting more bounded metadata.");
        auto result = std::make_shared<Resource>();
        result->id = nextResource_++;
        result->kind = kind;
        return Result<std::shared_ptr<Resource>>::Success(std::move(result));
    }

    /** @copydoc Resources::Find */
    std::shared_ptr<Resource> Resources::Find(std::uint64_t id) const {
        const auto found = std::ranges::find_if(resources_, [&](const auto &entry) {
            return entry->id == id && !entry->retired;
        });
        return found == resources_.end() ? nullptr : *found;
    }

    /** @copydoc Resources::Retire */
    void Resources::Retire(std::uint64_t id, Resource::Kind kind) noexcept {
        HORO_INVARIANT_MSG(owner_ == std::this_thread::get_id(), "Retire D3D12 resources on the render owner thread.");
        if (const auto resource = Find(id); resource && resource->kind == kind)
            resource->retired = true;
        Reclaim();
    }

    /** @copydoc Resources::Reclaim */
    void Resources::Reclaim() noexcept {
        // One reverse pass handles ordinary parent-after-child creation; later polls handle arbitrary graphs.
        for (std::size_t index = resources_.size(); index > 0; --index) {
            auto &resource = resources_[index - 1];
            if (resource->retired && resource.use_count() == 1) {
                for (std::size_t slot = 0; slot < resource->descriptors.size(); ++slot)
                    if (resource->descriptors[slot])
                        pools_[slot == 4 ? 0 : slot].occupied[*resource->descriptors[slot]] = false;
                if (index != resources_.size())
                    resource = std::move(resources_.back());
                resources_.pop_back();
            }
        }
    }
}  // namespace Horo::Render::D3D12Detail
#endif
