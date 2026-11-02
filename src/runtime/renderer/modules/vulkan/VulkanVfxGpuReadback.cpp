#include "VulkanVfxGpuInternal.h"

#include <algorithm>
#include <cstring>

namespace Horo::Render::Detail {
    namespace {
        bool Lost(const Error &error) {
            return error.code.Value() == VulkanBackendErrors::DeviceLost.code.Value();
        }

        void Terminal(VulkanVfxGpuSimulation &simulation, VulkanVfxGpuOperation &operation, bool failed) noexcept {
            if (operation.journal == std::numeric_limits<std::size_t>::max())
                return;
            auto &journal = simulation.journals[operation.journal];
            if (journal.observe) {
                auto &output = simulation.observations[journal.observation];
                output.completed = true;
                output.failed = failed || journal.uncertain;
            }
            if (journal.accepted &&
                !simulation.Queue(
                    {journal.step, failed ? VfxGpuSubmissionState::DeviceLost : VfxGpuSubmissionState::Completed, journal.point}))
                std::terminate();
            --simulation.reserved;
            journal = {};
        }

        VulkanVfxGpuBuffer *Observation(VulkanVfxGpuSimulation &simulation, const VfxGpuStepIdentity &step) noexcept {
            for (auto &buffer : simulation.observations)
                if (buffer.held && buffer.observationStep == step)
                    return &buffer;
            return nullptr;
        }

        bool Quiescent(const VulkanVfxGpuSimulation &simulation) noexcept {
            return simulation.active.empty();
        }
    }  // namespace

    Result<void> VulkanVfxGpuRuntime::Impl::Observe(VulkanVfxGpuSimulation &simulation) {
        const auto polled = commands->Poll();
        if (polled.HasError() && !Lost(polled.ErrorValue()))
            return polled;
        std::size_t index{};
        while (index < simulation.active.size()) {
            auto &operation = simulation.operations[simulation.active[index]];
            if (!operation.accepted && !operation.uncertain) {
                ++index;
                continue;
            }
            const bool lost = polled.HasError();
            const auto complete = lost ? Result<bool>::Success(true) : Complete(operation.point, false);
            if (complete.HasError())
                return Result<void>::Failure(complete.ErrorValue());
            if (!complete.Value()) {
                ++index;
                continue;
            }
            Terminal(simulation, operation, lost);
            simulation.stopped = simulation.stopped || lost;
            operation = {};
            simulation.active[index] = simulation.active.back();
            simulation.active.pop_back();
        }
        return Result<void>::Success();
    }

    Result<void> VulkanVfxGpuRuntime::Impl::Retire(std::size_t budget) {
        const auto count = std::min(budget, simulations.size());
        for (std::size_t visit = 0; visit < count && !simulations.empty(); ++visit) {
            retirementCursor %= simulations.size();
            auto &simulation = simulations[retirementCursor];
            if (const auto observed = Observe(*simulation); observed.HasError())
                return observed;
            if (simulation.use_count() != 1 || !Quiescent(*simulation)) {
                ++retirementCursor;
                continue;
            }
            simulation->ReleaseNative();
            simulations.erase(simulations.begin() + static_cast<std::ptrdiff_t>(retirementCursor));
        }
        for (auto entry = kernels.begin(); entry != kernels.end();) {
            if (entry->use_count() == 1) {
                (*entry)->Reset();
                entry = kernels.erase(entry);
            } else
                ++entry;
        }
        return Result<void>::Success();
    }

    Result<void> VulkanVfxGpuRuntime::CollectRetired() {
        if (!Available())
            return Result<void>::Success();
        if (const auto access = impl_->Access(false); access.HasError())
            return access;
        return impl_->Retire(64);
    }

    Result<std::size_t> VulkanVfxGpuRuntime::Poll(const std::shared_ptr<IResidentVfxGpuSimulation> &source,
                                                  std::span<NativeGpuVfxStepReceipt> destination) {
        using Outcome = Result<std::size_t>;
        auto found = impl_->Simulation(source);
        if (found.HasError())
            return Outcome::Failure(found.ErrorValue());
        auto &simulation = *found.Value();
        if (const auto observed = impl_->Observe(simulation); observed.HasError())
            return Outcome::Failure(observed.ErrorValue());
        const auto count = std::min(destination.size(), simulation.count);
        for (std::size_t i = 0; i < count; ++i) {
            destination[i] = simulation.receipts[simulation.head];
            simulation.head = (simulation.head + 1) % simulation.receipts.size();
            --simulation.count;
        }
        return Outcome::Success(count);
    }

    Result<std::size_t> VulkanVfxGpuRuntime::Copy(const std::shared_ptr<IResidentVfxGpuSimulation> &source, const VfxGpuStepIdentity &step,
                                                  std::span<std::byte> destination) {
        using Outcome = Result<std::size_t>;
        auto found = impl_->Simulation(source);
        if (found.HasError())
            return Outcome::Failure(found.ErrorValue());
        auto &simulation = *found.Value();
        if (const auto observed = impl_->Observe(simulation); observed.HasError())
            return Outcome::Failure(observed.ErrorValue());
        auto *output = Observation(simulation, step);
        if (!output)
            return Outcome::Failure(VulkanVfxGpuInvalid("No exact held native observation exists for this step."));
        if (!output->completed)
            return Outcome::Failure(MakeError(VfxFrameErrors::Pending));
        if (output->failed)
            return Outcome::Failure(MakeError(VulkanBackendErrors::DeviceLost));
        const auto bytes = std::size_t{simulation.descriptor.readback.maximumRecords} * VfxGpuObservationWords::Bytes;
        if (destination.size() < bytes || !output->capture.mapped)
            return Outcome::Failure(VulkanVfxGpuInvalid("Observation destination/mapping is invalid."));
        std::memcpy(destination.data(), output->capture.mapped, bytes);
        output->held = false;
        return Outcome::Success(bytes);
    }

    Result<void> VulkanVfxGpuRuntime::Discard(const std::shared_ptr<IResidentVfxGpuSimulation> &source, const VfxGpuStepIdentity &step) {
        auto found = impl_->Simulation(source);
        if (found.HasError())
            return Result<void>::Failure(found.ErrorValue());
        auto &simulation = *found.Value();
        if (const auto observed = impl_->Observe(simulation); observed.HasError())
            return observed;
        auto *output = Observation(simulation, step);
        if (!output)
            return Result<void>::Failure(VulkanVfxGpuInvalid("No exact held native observation exists for discard."));
        if (!output->completed)
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        output->held = false;
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
