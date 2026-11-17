#include "MetalResourceRuntimeInternal.h"
#include "MetalVfxGpuGraphInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
    MetalVfxGpuSimulation::~MetalVfxGpuSimulation() {
        if (resources && !CanRetire())
            std::terminate();
        if (resources)
            resources->Release();
    }

    /** @copydoc MetalVfxGpuSimulation::RetainedPayloadBytes */
    std::size_t MetalVfxGpuSimulation::RetainedPayloadBytes() const noexcept {
        return sizeof(*this) + states.capacity() * sizeof(MetalVfxGpuBuffer) + observations.capacity() * sizeof(MetalVfxGpuBuffer) +
               journals.capacity() * sizeof(MetalVfxGpuJournal) + receipts.capacity() * sizeof(NativeGpuVfxStepReceipt) +
               (kernel ? sizeof(MetalVfxGpuKernel) : 0);
    }

    bool MetalVfxGpuSimulation::CanRetire() const noexcept {
        const auto complete = [](const MetalVfxGpuBuffer &buffer) {
            return MetalVfxGpuQuiescent(*buffer.native);
        };
        const bool journalsTerminal = std::ranges::all_of(journals, [](const MetalVfxGpuJournal &journal) {
            return !journal.occupied || journal.terminal;
        });
        return journalsTerminal && std::ranges::all_of(states, complete) && std::ranges::all_of(observations, complete) &&
               (forces.native == nullptr || complete(forces));
    }

    bool MetalVfxGpuSimulation::QueueReceipt(const NativeGpuVfxStepReceipt &receipt) noexcept {
        if (receiptCount == receipts.size())
            return false;
        receipts[(receiptHead + receiptCount) % receipts.size()] = receipt;
        ++receiptCount;
        return true;
    }

    /** @brief A terminal uncertain transaction releases ownership without inventing an accepted/completed receipt. */
    void MetalVfxGpuSimulation::PublishTerminal(MetalVfxGpuJournal &journal, const MTLCommandBufferStatus status) noexcept {
        const auto state = status == MTLCommandBufferStatusCompleted ? VfxGpuSubmissionState::Completed : VfxGpuSubmissionState::DeviceLost;
        if (journal.observe) {
            auto &output = observations[journal.observation];
            output.observationCompleted = true;
            output.observationFailed = journal.uncertain || state != VfxGpuSubmissionState::Completed;
        }
        if (!journal.uncertain && !QueueReceipt({journal.step, state, journal.submission}))
            std::terminate();
        --reservedReceipts;
    }

    /** @brief Publishes actual terminal command status, preserving device failure separately from successful execution. */
    void MetalVfxGpuSimulation::ObserveCompletions() noexcept {
        for (auto &journal : journals) {
            if (!journal.occupied || (!journal.accepted && !journal.uncertain) || journal.terminal)
                continue;
            const auto status = journal.commands.status;
            if (status != MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError)
                continue;
            PublishTerminal(journal, status);
            journal.terminal = true;
            journal.occupied = false;
            journal.commands = nil;
            if (status == MTLCommandBufferStatusError)
                stopped = true;
        }
    }

    /** @brief Called only after native commit return or a proven unsent abort; no generation is advanced during encoding. */
    void MetalVfxGpuSimulation::FinishCommands(id<MTLCommandBuffer> commands, const bool committed) noexcept {
        for (auto &journal : journals) {
            if (!journal.occupied || journal.commands != commands || journal.accepted)
                continue;
            const auto state = committed ? VfxGpuSubmissionState::Accepted : VfxGpuSubmissionState::CancelledUnsubmitted;
            if (!QueueReceipt({journal.step, state, committed ? journal.submission : RenderTimelinePoint{}}))
                std::terminate();
            --reservedReceipts;
            if (!committed) {
                if (journal.observe)
                    observations[journal.observation].observationHeld = false;
                --reservedReceipts;
                journal = {};
                continue;
            }
            journal.accepted = true;
            currentSource = journal.destination;
            currentGeneration = journal.step.targetGeneration;
            nextTick = journal.step.tick + 1;
            states[currentSource].generation = currentGeneration;
            observations[journal.observation].generation = currentGeneration;
        }
    }

    /** @brief Commit uncertainty retains the whole inventory and never advances the accepted generation. */
    void MetalVfxGpuSimulation::PreserveUncertain(id<MTLCommandBuffer> commands) noexcept {
        for (auto &journal : journals) {
            if (!journal.occupied || journal.commands != commands || journal.accepted)
                continue;
            stopped = true;
            journal.uncertain = true;
            if (!QueueReceipt({journal.step, VfxGpuSubmissionState::SubmissionUncertain, journal.submission}))
                std::terminate();
            --reservedReceipts;
        }
    }

    /** @copydoc MetalResourceRuntime::PreserveUncertainVfxGpuCommands */
    void MetalResourceRuntime::PreserveUncertainVfxGpuCommands(void *commands) noexcept {
        for (const auto &simulation : impl_->vfxGpuSimulations)
            simulation->PreserveUncertain((__bridge id<MTLCommandBuffer>)commands);
    }

    namespace {
        bool CurrentUnsubmittedStep(const MetalVfxGpuSimulation &simulation, const RenderGraphVfxGpuCompute &work) noexcept {
            if (simulation.stopped || work.work.step.simulation != simulation.simulationId ||
                work.work.step.sourceGeneration != simulation.currentGeneration || work.work.step.tick != simulation.nextTick ||
                !SameVfxGpuDescriptor(simulation.descriptor, work.descriptor))
                return false;
            auto uncancelled = work.work;
            uncancelled.callerCancellation = {};
            uncancelled.lifetimeCancellation = {};
            return ValidateVfxGpuSimulationWork(simulation.descriptor, uncancelled);
        }

        bool AlreadyJournaled(const MetalVfxGpuSimulation &simulation, const VfxGpuStepIdentity &step) noexcept {
            for (const auto &journal : simulation.journals)
                if (journal.occupied && journal.step == step)
                    return true;
            for (std::size_t offset = 0; offset < simulation.receiptCount; ++offset)
                if (simulation.receipts[(simulation.receiptHead + offset) % simulation.receipts.size()].step == step)
                    return true;
            return false;
        }

        void RejectStep(MetalVfxGpuSimulation &simulation, const RenderGraphVfxGpuCompute &work) noexcept {
            if (!CurrentUnsubmittedStep(simulation, work) || AlreadyJournaled(simulation, work.work.step) ||
                simulation.receiptCount + simulation.reservedReceipts == simulation.receipts.size())
                return;
            const bool cancelled =
                work.work.callerCancellation.IsCancellationRequested() || work.work.lifetimeCancellation.IsCancellationRequested();
            const auto state = cancelled ? VfxGpuSubmissionState::CancelledUnsubmitted : VfxGpuSubmissionState::RejectedUnsubmitted;
            if (!simulation.QueueReceipt({work.work.step, state, {}}))
                std::terminate();
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::RejectUnsubmittedVfxGpuGraph */
    void MetalResourceRuntime::RejectUnsubmittedVfxGpuGraph(const RenderGraphExecutionRequest &request) noexcept {
        for (const auto &entry : request.workloads) {
            const auto *work = std::get_if<RenderGraphVfxGpuCompute>(&entry.workload);
            if (!work)
                continue;
            const auto simulation = std::dynamic_pointer_cast<MetalVfxGpuSimulation>(work->simulationLease);
            if (!simulation || simulation->owner != impl_->vfxGpuOwner || simulation->incarnation != impl_->lightKernelIncarnation ||
                work->work.step.deviceIncarnation != impl_->deviceIncarnation || !MetalVfxStepSignal(*work, request).IsValid())
                continue;
            RejectStep(*simulation, *work);
        }
    }

    /** @copydoc MetalResourceRuntime::FinishVfxGpuCommands */
    void MetalResourceRuntime::FinishVfxGpuCommands(void *commands, const bool committed) noexcept {
        for (const auto &simulation : impl_->vfxGpuSimulations)
            simulation->FinishCommands((__bridge id<MTLCommandBuffer>)commands, committed);
    }

    /** @copydoc MetalResourceRuntime::PollNativeVfxGpuReceipts */
    Result<std::size_t> MetalResourceRuntime::PollNativeVfxGpuReceipts(const std::shared_ptr<IResidentVfxGpuSimulation> &source,
                                                                       const std::span<NativeGpuVfxStepReceipt> destination) {
        const auto simulation = std::dynamic_pointer_cast<MetalVfxGpuSimulation>(source);
        if (!simulation || simulation->owner != impl_->vfxGpuOwner || simulation->incarnation != impl_->lightKernelIncarnation)
            return Result<std::size_t>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        simulation->ObserveCompletions();
        const auto count = std::min(destination.size(), simulation->receiptCount);
        for (std::size_t index = 0; index < count; ++index) {
            destination[index] = simulation->receipts[simulation->receiptHead];
            simulation->receiptHead = (simulation->receiptHead + 1) % simulation->receipts.size();
            --simulation->receiptCount;
        }
        return Result<std::size_t>::Success(count);
    }

    /** @copydoc DrainRetiredMetalVfxGpuSimulations */
    void DrainRetiredMetalVfxGpuSimulations(std::vector<std::shared_ptr<MetalVfxGpuSimulation>> &simulations, std::size_t &cursor,
                                            const std::size_t budget) noexcept {
        const auto visits = std::min(budget, simulations.size());
        for (std::size_t visit = 0; visit < visits && !simulations.empty(); ++visit) {
            cursor %= simulations.size();
            auto &simulation = simulations[cursor];
            if (simulation.use_count() != 1) {
                ++cursor;
                continue;
            }
            simulation->ObserveCompletions();
            if (!simulation->CanRetire()) {
                ++cursor;
                continue;
            }
            simulation->stopped = true;
            simulations.erase(simulations.begin() + static_cast<std::ptrdiff_t>(cursor));
        }
        if (simulations.empty())
            cursor = 0;
    }

    /** @copydoc MetalResourceRuntime::DrainVfxGpuRetirements */
    void MetalResourceRuntime::DrainVfxGpuRetirements(const std::size_t budget) noexcept {
        DrainRetiredMetalVfxGpuSimulations(impl_->vfxGpuSimulations, impl_->vfxGpuRetirementCursor, budget);
    }

    /** @copydoc MetalResourceRuntime::VfxGpuSimulationsDrained */
    bool MetalResourceRuntime::VfxGpuSimulationsDrained() noexcept {
        for (const auto &simulation : impl_->vfxGpuSimulations) {
            simulation->ObserveCompletions();
            if (!simulation->CanRetire())
                return false;
        }
        for (const auto &simulation : impl_->vfxGpuSimulations) {
            simulation->stopped = true;
            if (simulation->resources)
                simulation->resources->Release();
            simulation->resources.reset();
        }
        impl_->vfxGpuSimulations.clear();
        impl_->vfxGpuRetirementCursor = 0;
        return true;
    }
}  // namespace Horo::Render::Detail
