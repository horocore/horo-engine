#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderGraphTransientResourcePool.h"

#include <algorithm>
#include <memory>

namespace Horo::Render::Detail {
    namespace {
        using Kernels = std::array<std::shared_ptr<IResidentUiPaintKernel>, 8>;

        /** @brief Retains the distinct bounded native pipeline owners without allocating or touching frame lease state. */
        Result<Kernels> CaptureKernels(std::span<const RenderGraphPassWorkload> workloads) {
            std::array<std::shared_ptr<IResidentUiPaintKernel>, 8> kernels;
            std::size_t kernelCount{};
            for (const auto &workload : workloads) {
                const auto *paint = std::get_if<RenderGraphUiPaint>(&workload.workload);
                if (!paint || !paint->kernel)
                    return Result<Kernels>::Failure(MakeError(FrontendErrors::ResourceNotReady));
                if (std::find(kernels.begin(), kernels.begin() + kernelCount, paint->kernel) != kernels.begin() + kernelCount)
                    continue;
                if (kernelCount == kernels.size())
                    return Result<Kernels>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
                kernels[kernelCount++] = paint->kernel;
            }
            return Result<Kernels>::Success(std::move(kernels));
        }
    }  // namespace

    /** @copydoc RenderGraphResourceLeasePool::Lease::RollbackPins */
    void RenderGraphResourceLeasePool::Lease::RollbackPins(std::size_t initialPins) noexcept {
        while (pins.size() > initialPins) {
            const auto pin = pins.back();
            static_cast<void>(pool->registry_->ReleaseSubmissionPin(pin.resourceClass, pin.identity));
            pins.pop_back();
            --pool->activePins_;
        }
    }

    /** @copydoc RenderGraphResourceLeasePool::AcquireUi */
    Result<IRenderGraphResourceLease *> RenderGraphResourceLeasePool::AcquireUi(const std::span<const RenderGraphResource> resources,
                                                                                UiRenderSubmission &ui,
                                                                                RenderGraphTransientResourceSet *transient,
                                                                                IRenderGraphResourceLease *existing,
                                                                                std::span<const RenderGraphPassWorkload> workloads) {
        using Outcome = Result<IRenderGraphResourceLease *>;
        auto capturedKernels = CaptureKernels(workloads);
        if (capturedKernels.HasError())
            return Outcome::Failure(capturedKernels.ErrorValue());
        auto kernels = std::move(capturedKernels).Value();
        if (existing == nullptr) {
            auto acquired = Acquire(resources, &ui, transient);
            if (acquired.HasValue())
                static_cast<Lease *>(acquired.Value())->ui.front().kernels = std::move(kernels);
            return acquired;
        }
        if (resources.size() > RenderGraphLimits::HardMaxResources || (transient && (transient->inFlight || transient->released)))
            return Outcome::Failure(MakeError(FrontendErrors::ResourceNotReady));
        const auto selected = std::ranges::find_if(leases_, [existing](const Lease &candidate) {
            return candidate.active && static_cast<const IRenderGraphResourceLease *>(&candidate) == existing;
        });
        if (selected == leases_.end() || selected->uiCount == selected->ui.size())
            return Outcome::Failure(MakeError(FrontendErrors::ResourceNotReady));
        if (std::ranges::any_of(std::span{selected->ui}.first(selected->uiCount), [&](const auto &capture) {
            return capture.source.HasGeometry(ui);
        }))
            return Outcome::Failure(MakeError(FrontendErrors::ResourceNotReady));
        const auto initialPins = selected->pins.size();
        const auto rollbackPins = [initialPins](Lease *pending) {
            pending->RollbackPins(initialPins);
        };
        std::unique_ptr<Lease, decltype(rollbackPins)> transaction{&*selected, rollbackPins};
        for (const auto &resource : resources)
            if (const auto retained = selected->PinResource(resource, transient); retained.HasError())
                return Outcome::Failure(retained.ErrorValue());
        if (const auto captured = selected->ui[selected->uiCount].source.Capture(ui, resources); captured.HasError())
            return Outcome::Failure(captured.ErrorValue());
        auto &capture = selected->ui[selected->uiCount++];
        capture.kernels = std::move(kernels);
        capture.transient = transient;
        if (transient)
            transient->inFlight = true;
        return Outcome::Success(transaction.release());
    }
}  // namespace Horo::Render::Detail
