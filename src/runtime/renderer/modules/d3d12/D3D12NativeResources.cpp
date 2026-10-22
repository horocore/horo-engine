#include "D3D12ResourceInternals.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render {
    /** @copydoc CreateD3D12Resources */
    Result<std::unique_ptr<ID3D12Resources>> CreateD3D12Resources(ID3D12Device *device, ID3D12CommandQueue *queue,
                                                                  const D3D12ResourceLimits &limits, D3D12ResourceTestHooks testHooks) {
        if (!device || !queue || !limits.IsValid() || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return Result<std::unique_ptr<ID3D12Resources>>::Failure(
                D3D12Detail::ResourceError("render.d3d12.resource_config_invalid",
                                           "Supply the initialized device, its direct queue and finite resource limits."));
        Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
        const HRESULT status = queue->GetDevice(IID_PPV_ARGS(&queueDevice));
        if (FAILED(status) || queueDevice.Get() != device)
            return Result<std::unique_ptr<ID3D12Resources>>::Failure(
                D3D12Detail::ResourceError("render.d3d12.queue_device_mismatch", "Use a direct queue from the exact initialized device.",
                                           status));
        auto service = std::make_unique<D3D12Detail::Resources>(device, queue, limits, testHooks);
        auto initialized = service->Initialize();
        if (initialized.HasError())
            return Result<std::unique_ptr<ID3D12Resources>>::Failure(initialized.ErrorValue());
        return Result<std::unique_ptr<ID3D12Resources>>::Success(std::move(service));
    }
}  // namespace Horo::Render
#endif
