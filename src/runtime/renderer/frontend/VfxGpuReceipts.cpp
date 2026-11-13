#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "RenderFrontendErrors.h"
#include "RenderFrontendResourceAccess.h"
#include "RenderSubmissionAllocator.h"
#include "VfxGpuReadback.h"
#include "VfxGpuStorage.h"

#include <algorithm>

namespace Horo::Render {
    namespace {
        using namespace Detail;

        Result<std::size_t> PollNativeFacts(IRenderBackend &backend, const std::shared_ptr<IResidentVfxGpuSimulation> &simulation,
                                            const std::span<NativeGpuVfxStepReceipt> destination) {
            try {
                return backend.PollNativeVfxGpuReceipts(simulation, destination);
            } catch (...) {  // Preserve already validated acknowledgement prefixes across a foreign backend exception.
                return Result<std::size_t>::Failure(MakeError(FrontendErrors::ResourceBackendException));
            }
        }

        RenderTimelinePoint CoveringSignal(const RenderGraphExecutionRequest &request, const std::size_t index) noexcept {
            for (const auto &partition : request.submissions)
                if (index >= partition.passes.offset && index - partition.passes.offset < partition.passes.count)
                    return partition.submission.signal;
            return {};
        }

        RenderBufferHandle BoundBuffer(const RenderGraphExecutionRequest &request, RenderGraphResourceId resource) noexcept {
            const auto found = std::ranges::find(request.graph.Resources(), resource, &RenderGraphResource::id);
            if (found == request.graph.Resources().end())
                return {};
            const auto *handle = std::get_if<RenderBufferHandle>(&found->binding);
            return handle ? *handle : RenderBufferHandle{};
        }

        VfxGpuFrontendStep *RegisteredStep(VfxGpuRecord &record, const VfxGpuStepIdentity &identity) noexcept {
            const auto found = std::ranges::find_if(record.steps, [&](const auto &step) {
                return step.occupied && step.identity == identity;
            });
            return found == record.steps.end() ? nullptr : &*found;
        }

        bool ComputeBuffers(const VfxGpuRecord &record, const VfxGpuFrontendStep &step, const RenderGraphVfxGpuCompute &work,
                            const RenderGraphExecutionRequest &request) noexcept {
            const std::array actual{BoundBuffer(request, work.source), BoundBuffer(request, work.destination),
                                    BoundBuffer(request, work.forces), BoundBuffer(request, work.observations)};
            const std::array expected{record.states[record.currentSource].handle, record.states[step.destination].handle,
                                      record.forces.handle, record.observations[step.observation].handle};
            return actual == expected;
        }

        bool ComputeMetadata(const VfxGpuRecord &record, const RenderGraphVfxGpuCompute &work) noexcept {
            return !record.released && !record.stopped && !VfxGpuLifetimeCancelled(record) && work.simulationLease == record.native &&
                   work.kernel == record.kernel && SameVfxGpuDescriptor(work.descriptor, record.descriptor);
        }

        bool ExactCompute(VfxGpuRecord &record, const RenderGraphPassWorkload &entry, const RenderGraphVfxGpuCompute &work,
                          const RenderGraphExecutionRequest &request) noexcept {
            const auto *step = RegisteredStep(record, work.work.step);
            if (!step || step->dispatched || record.scheduledGraph != request.graph.Owner() || entry.pass != record.computePass)
                return false;
            if (step->unitLifetime.IsCancellationRequested() || step->callerCancellation.IsCancellationRequested())
                return false;
            if (!ComputeMetadata(record, work) || !ComputeBuffers(record, *step, work, request))
                return false;
            std::array<std::uint32_t, VfxGpuDispatchWords::Count> parameters;
            return PackVfxGpuDispatchParameters(record.descriptor, work.work, parameters) && parameters == step->parameters;
        }

        bool DrawMetadata(const VfxGpuRecord &record, const RenderGraphVfxGpuDraw &draw) noexcept {
            return !record.released && !record.stopped && !VfxGpuLifetimeCancelled(record) && draw.simulationLease == record.native &&
                   draw.kernel == record.kernel && draw.deviceIncarnation == record.deviceIncarnation &&
                   SameVfxGpuDescriptor(draw.descriptor, record.descriptor);
        }

        bool DrawGeneration(const VfxGpuRecord &record, const RenderGraphVfxGpuDraw &draw,
                            const RenderGraphExecutionRequest &request) noexcept {
            const auto buffer = BoundBuffer(request, draw.particles);
            for (std::size_t index = 0; index < record.states.size(); ++index)
                if (record.states[index].handle == buffer && record.generations[index] == draw.generation)
                    return true;
            for (const auto &step : record.steps)
                if (step.occupied && record.scheduledGraph == request.graph.Owner() && step.identity.targetGeneration == draw.generation &&
                    record.states[step.destination].handle == buffer)
                    return true;
            return false;
        }

        bool ExactDraw(const VfxGpuRecord &record, const RenderGraphVfxGpuDraw &draw, const RenderGraphExecutionRequest &request) noexcept {
            return DrawMetadata(record, draw) && DrawGeneration(record, draw, request);
        }

        bool NativeStateKnown(const VfxGpuSubmissionState state) noexcept {
            return state <= VfxGpuSubmissionState::DeviceLost;
        }

        bool Unsubmitted(const VfxGpuSubmissionState state) noexcept {
            return state == VfxGpuSubmissionState::RejectedUnsubmitted || state == VfxGpuSubmissionState::CancelledUnsubmitted;
        }

        /** @brief A trusted per-simulation native fact is scoped to its registered signal, independent of global history eviction. */
        Result<void> NativePoint(const VfxGpuFrontendStep &step, const NativeGpuVfxStepReceipt &receipt) {
            if (!NativeStateKnown(receipt.state))
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            if (Unsubmitted(receipt.state))
                return receipt.submission == RenderTimelinePoint{} && !step.accepted && !step.uncertain
                           ? Result<void>::Success()
                           : Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            // This exact point was checked with WasAdmittedPoint before dispatch and committed before the backend call.
            if (!step.submission.IsValid() || receipt.submission != step.submission)
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            return Result<void>::Success();
        }

        Result<void> AcceptNativeStep(VfxGpuRecord &record, VfxGpuFrontendStep &step, const NativeGpuVfxStepReceipt &receipt) {
            if (step.accepted || step.uncertain || receipt.step.sourceGeneration != record.generation)
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            step.accepted = true;
            record.currentSource = step.destination;
            record.generation = receipt.step.targetGeneration;
            record.nextTick = receipt.step.tick + 1;
            record.generations[step.destination] = record.generation;
            record.acceptedPoint = receipt.submission;
            return Result<void>::Success();
        }

        Result<void> TerminalNativeFact(VfxGpuRecord &record, VfxGpuFrontendStep &step, const NativeGpuVfxStepReceipt &receipt) {
            if (!Unsubmitted(receipt.state) && !step.accepted)
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            if (receipt.state == VfxGpuSubmissionState::DeviceLost)
                record.stopped = true;
            if (step.observe) {
                if (Unsubmitted(receipt.state))
                    record.observationHeld[step.observation] = 0;
                else
                    record.observationCompleted[step.observation] = 1;
            }
            step = {};
            return Result<void>::Success();
        }

        Result<void> ApplyNativeFact(VfxGpuRecord &record, VfxGpuFrontendStep &step, const NativeGpuVfxStepReceipt &receipt) {
            if (receipt.state == VfxGpuSubmissionState::Accepted)
                return AcceptNativeStep(record, step, receipt);
            if (receipt.state == VfxGpuSubmissionState::SubmissionUncertain) {
                if (step.accepted || step.uncertain)
                    return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
                record.stopped = step.uncertain = true;
                return Result<void>::Success();
            }
            return TerminalNativeFact(record, step, receipt);
        }

        bool ComputeBindingValid(VfxGpuStorage *storage, const RenderGraphExecutionRequest &request,
                                 const RenderSubmissionAllocator &history, const std::size_t index,
                                 const RenderGraphVfxGpuCompute &work) noexcept {
            if (!storage)
                return false;
            auto *record = storage->Find(work.work.step.simulation);
            return record && ExactCompute(*record, request.workloads[index], work, request) &&
                   history.WasAdmittedPoint(CoveringSignal(request, index));
        }

        bool DrawBindingValid(VfxGpuStorage *storage, const RenderGraphExecutionRequest &request,
                              const RenderGraphVfxGpuDraw &draw) noexcept {
            if (!storage)
                return false;
            const auto *record = storage->Find(draw.simulation);
            return record && ExactDraw(*record, draw, request);
        }

        Result<void> ValidateGpuBindings(VfxGpuStorage *storage, const RenderGraphExecutionRequest &request,
                                         const RenderSubmissionAllocator &history) {
            for (std::size_t index = 0; index < request.workloads.size(); ++index) {
                const auto &entry = request.workloads[index];
                if (const auto *work = std::get_if<RenderGraphVfxGpuCompute>(&entry.workload))
                    if (!ComputeBindingValid(storage, request, history, index, *work))
                        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
                if (const auto *draw = std::get_if<RenderGraphVfxGpuDraw>(&entry.workload))
                    if (!DrawBindingValid(storage, request, *draw))
                        return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RenderFrontend::RegisterVfxGpuDispatch */
    Result<void> RenderFrontend::RegisterVfxGpuDispatch(const RenderGraphExecutionRequest &request,
                                                        const Detail::RenderSubmissionAllocator &history) {
        if (const auto validated = ValidateGpuBindings(vfxGpuStorage_.get(), request, history); validated.HasError())
            return validated;
        for (std::size_t index = 0; index < request.workloads.size(); ++index)
            if (const auto *work = std::get_if<RenderGraphVfxGpuCompute>(&request.workloads[index].workload)) {
                auto *record = vfxGpuStorage_->Find(work->work.step.simulation);
                auto *step = RegisteredStep(*record, work->work.step);
                step->submission = CoveringSignal(request, index);
                step->dispatched = true;
            }
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::AbandonUndispatchedVfxGpu */
    void RenderFrontend::AbandonUndispatchedVfxGpu(const std::uint64_t frame) noexcept {
        if (!vfxGpuStorage_)
            return;
        for (auto &slot : vfxGpuStorage_->slots) {
            if (!slot.record || slot.record->scheduledFrame != frame)
                continue;
            auto &record = *slot.record;
            for (auto &step : record.steps) {
                if (!step.occupied || step.dispatched)
                    continue;
                if (step.observe)
                    Detail::AbandonVfxGpuObservation(*vfxGpuStorage_, record, step.identity);
                step = {};
            }
            record.computePass = {};
            record.scheduledGraph = {};
        }
    }

    /** @copydoc RenderFrontend::ValidateVfxGpuReceipt */
    Result<GpuVfxStepAcknowledgement> RenderFrontend::ValidateVfxGpuReceipt(Detail::VfxGpuRecord &record,
                                                                            const NativeGpuVfxStepReceipt &receipt) {
        using Outcome = Result<GpuVfxStepAcknowledgement>;
        auto *step = RegisteredStep(record, receipt.step);
        if (!step || !step->dispatched || receipt.step.deviceIncarnation != record.deviceIncarnation)
            return Outcome::Failure(MakeError(VfxFrameErrors::InvalidInput));
        if (const auto valid = NativePoint(*step, receipt); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (const auto applied = ApplyNativeFact(record, *step, receipt); applied.HasError())
            return Outcome::Failure(applied.ErrorValue());
        if (const auto observation = ObserveVfxGpuReadbackFact(record, receipt); observation.HasError()) {
            record.stopped = true;
            vfxGpuStorage_->pendingError = observation.ErrorValue();
        }
        GpuVfxStepAcknowledgement acknowledgement;
        acknowledgement.step_ = receipt.step;
        acknowledgement.state_ = receipt.state;
        acknowledgement.submission_ = receipt.submission;
        acknowledgement.authority_ = vfxFrontendState_;
        return Outcome::Success(std::move(acknowledgement));
    }

    /** @copydoc RenderFrontend::ReportVfxGpuDrainFailure */
    Result<std::size_t> RenderFrontend::ReportVfxGpuDrainFailure(Error error, const std::size_t prefix) {
        if (prefix == 0)
            return Result<std::size_t>::Failure(std::move(error));
        vfxGpuStorage_->pendingError = std::move(error);
        return Result<std::size_t>::Success(prefix);
    }

    /** @copydoc RenderFrontend::DrainVfxGpuRecordReceipts */
    Result<std::size_t> RenderFrontend::DrainVfxGpuRecordReceipts(Detail::VfxGpuRecord &record,
                                                                  const std::span<GpuVfxStepAcknowledgement> destination) {
        std::size_t count{};
        while (count < destination.size()) {
            std::array<NativeGpuVfxStepReceipt, 1> native;
            const auto polled = PollNativeFacts(*backend_, record.native, native);
            if (polled.HasError())
                return ReportVfxGpuDrainFailure(polled.ErrorValue(), count);
            if (polled.Value() == 0)
                break;
            if (polled.Value() != 1)
                return ReportVfxGpuDrainFailure(MakeError(VfxFrameErrors::InvalidInput), count);
            const auto valid = ValidateVfxGpuReceipt(record, native[0]);
            if (valid.HasError()) {
                record.stopped = true;
                return ReportVfxGpuDrainFailure(valid.ErrorValue(), count);
            }
            destination[count++] = valid.Value();
            if (vfxGpuStorage_->pendingError)
                break;
        }
        return Result<std::size_t>::Success(count);
    }

    /** @copydoc RenderFrontend::AdmitVfxGpuDrain */
    Result<bool> RenderFrontend::AdmitVfxGpuDrain(const std::span<GpuVfxStepAcknowledgement> destination) {
        using Outcome = Result<bool>;
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        if (activeFrameScope_ || (vfxGpuStorage_ && vfxGpuStorage_->busy))
            return Outcome::Failure(MakeError(VfxFrameErrors::Pending));
        if (!vfxGpuStorage_ || destination.empty())
            return Outcome::Success(false);
        if (vfxGpuStorage_->pendingError) {
            auto error = std::move(*vfxGpuStorage_->pendingError);
            vfxGpuStorage_->pendingError.reset();
            return Outcome::Failure(std::move(error));
        }
        return Outcome::Success(true);
    }

    /** @copydoc RenderFrontend::DrainVfxGpuStorage */
    Result<std::size_t> RenderFrontend::DrainVfxGpuStorage(const std::span<GpuVfxStepAcknowledgement> destination) {
        using Outcome = Result<std::size_t>;
        Detail::VfxGpuBusyGuard guard{vfxGpuStorage_->busy};
        const auto visits = std::min(destination.size(), vfxGpuStorage_->slots.size());
        std::size_t count{};
        for (std::size_t visit = 0; visit < visits && count < destination.size(); ++visit) {
            auto &slot = vfxGpuStorage_->slots[vfxGpuStorage_->drainCursor++ % vfxGpuStorage_->slots.size()];
            if (!slot.record || !slot.record->native)
                continue;
            const auto drained = DrainVfxGpuRecordReceipts(*slot.record, destination.subspan(count));
            if (drained.HasError())
                return ReportVfxGpuDrainFailure(drained.ErrorValue(), count);
            count += drained.Value();
            if (vfxGpuStorage_->pendingError)
                break;
        }
        return Outcome::Success(count);
    }

    /** @copydoc RenderFrontend::DrainVfxGpuAcknowledgements */
    Result<std::size_t> RenderFrontend::DrainVfxGpuAcknowledgements(const std::span<GpuVfxStepAcknowledgement> destination) {
        const auto admitted = AdmitVfxGpuDrain(destination);
        if (admitted.HasError())
            return Result<std::size_t>::Failure(admitted.ErrorValue());
        if (!admitted.Value())
            return Result<std::size_t>::Success(0);
        return DrainVfxGpuStorage(destination);
    }
}  // namespace Horo::Render
