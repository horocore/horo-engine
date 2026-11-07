#include "VfxGpuScheduling.h"

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphErrors.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "RenderFrontendErrors.h"
#include "RenderResourceRegistry.h"
#include "RenderSubmissionAllocator.h"
#include "VfxGpuReadback.h"

#include <algorithm>

namespace Horo::Render::Detail {
    namespace {
        std::size_t GenerationSlot(const VfxGpuRecord &record, std::uint64_t generation) noexcept {
            const auto found = std::ranges::find(record.generations, generation);
            return static_cast<std::size_t>(found - record.generations.begin());
        }

        bool DestinationHeld(const VfxGpuRecord &record, const std::size_t destination) noexcept {
            return std::ranges::any_of(record.steps, [destination](const auto &step) {
                return step.occupied && step.destination == destination;
            });
        }

        std::size_t WritableSlot(const VfxGpuRecord &record) noexcept {
            const auto writable = record.states.size() - 1;
            for (std::size_t offset = 0; offset < writable; ++offset) {
                const auto index = 1 + (record.nextDestination - 1 + offset) % writable;
                if (index != record.currentSource && !DestinationHeld(record, index))
                    return index;
            }
            return record.states.size();
        }

        const ErrorCodeDescriptor *SelectNewStep(VfxGpuAppendPlan &plan) noexcept {
            auto &record = plan.record;
            const auto free = std::ranges::find_if(record.steps, [](const auto &step) {
                return !step.occupied;
            });
            if (free == record.steps.end())
                return &VfxFrameErrors::Pending;
            plan.step = static_cast<std::size_t>(free - record.steps.begin());
            plan.destination = WritableSlot(record);
            if (plan.destination == record.states.size())
                return &VfxFrameErrors::Pending;
            plan.observation = 0;
            if (plan.work.observe) {
                const auto available = std::ranges::find(record.observationHeld, std::uint8_t{0});
                if (available == record.observationHeld.end())
                    return &VfxFrameErrors::Pending;
                plan.observation = static_cast<std::size_t>(available - record.observationHeld.begin());
            }
            plan.dispatch = true;
            plan.draw = plan.destination;
            return nullptr;
        }

        const ErrorCodeDescriptor *SelectPending(VfxGpuAppendPlan &plan, const RenderGraphOwnerId graph) noexcept {
            auto &record = plan.record;
            const auto existing = std::ranges::find_if(record.steps, [&](const auto &step) {
                return step.occupied && step.identity == plan.work.step;
            });
            if (existing == record.steps.end())
                return SelectNewStep(plan);
            if (existing->parameters != plan.parameters)
                return &VfxFrameErrors::InvalidInput;
            if (record.scheduledGraph == graph && !existing->accepted) {
                plan.producer = record.computePass;
                plan.draw = existing->destination;
                return nullptr;
            }
            if (existing->dispatched)
                return nullptr;  // An authentic receipt, not a duplicate snapshot, decides acceptance.
            plan.step = static_cast<std::size_t>(existing - record.steps.begin());
            plan.destination = existing->destination;
            plan.observation = existing->observation;
            plan.replaceUndispatched = true;
            plan.dispatch = true;
            plan.draw = plan.destination;
            return nullptr;
        }

        const ErrorCodeDescriptor *NormalizePending(VfxGpuAppendPlan &plan) noexcept {
            auto &record = plan.record;
            plan.work = *plan.request.pending;
            if (plan.work.callerCancellation.IsCancellationRequested())
                return &VfxFrameErrors::Cancelled;
            plan.work.lifetimeCancellation = *plan.request.lifetimeCancellation;
            const auto prior = std::ranges::find_if(record.steps, [&](const auto &step) {
                return step.occupied && step.identity == plan.work.step;
            });
            const bool priorOmission = prior != record.steps.end() && !prior->observe;
            if (record.readback != VfxGpuReadbackAdmission::Available || priorOmission) {
                plan.schedule.observationOmitted = plan.work.observe;
                plan.work.observe = false;
            }
            if (!PackVfxGpuDispatchParameters(record.descriptor, plan.work, plan.parameters))
                return &VfxFrameErrors::InvalidInput;
            return nullptr;
        }

        const ErrorCodeDescriptor *PendingPlan(VfxGpuAppendPlan &plan, const RenderGraphOwnerId graph) noexcept {
            auto &record = plan.record;
            if (const auto *error = NormalizePending(plan))
                return error;
            if (plan.work.step.simulation != record.id || plan.work.step.deviceIncarnation != record.deviceIncarnation)
                return &VfxFrameErrors::InvalidInput;
            if (plan.work.step.targetGeneration <= record.generation) {
                const auto target = GenerationSlot(record, plan.work.step.targetGeneration);
                if (target == record.states.size())
                    return &VfxFrameErrors::Pending;
                plan.draw = target;
                return nullptr;
            }
            if (plan.work.step.sourceGeneration != record.generation || plan.work.step.tick != record.nextTick)
                return &VfxFrameErrors::InvalidInput;
            return SelectPending(plan, graph);
        }
    }  // namespace

    /** @copydoc PlanVfxGpu */
    const ErrorCodeDescriptor *PlanVfxGpu(VfxGpuAppendPlan &plan, const RenderGraphOwnerId graph) noexcept {
        auto &record = plan.record;
        if (record.stopped || record.released || VfxGpuLifetimeCancelled(record))
            return &VfxFrameErrors::Cancelled;
        if (plan.request.generation == 0)
            return &VfxFrameErrors::InvalidInput;
        plan.source = record.currentSource;
        plan.draw = GenerationSlot(record, plan.request.generation);
        if (plan.draw == record.states.size())
            return &VfxFrameErrors::Pending;
        if (plan.request.pending)
            if (const auto *error = PendingPlan(plan, graph))
                return error;
        if (plan.dispatch) {
            plan.handles = {record.states[plan.source].handle, record.states[plan.destination].handle, record.forces.handle,
                            record.observations[plan.observation].handle};
            plan.inputCount = 4;
        } else {
            plan.handles[0] = record.states[plan.draw].handle;
            plan.inputCount = 1;
        }
        return nullptr;
    }

    /** @copydoc CommitVfxGpuPlan */
    void CommitVfxGpuPlan(VfxGpuAppendPlan &plan, const RenderGraphOwnerId graph, const std::uint64_t frame) noexcept {
        auto &record = plan.record;
        if (!record.unitLifetimeBound) {
            record.unitLifetime = *plan.request.lifetimeCancellation;
            record.unitLifetimeBound = true;
        }
        if (!plan.dispatch)
            return;
        auto &step = record.steps[plan.step];
        step = {.identity = plan.work.step,
                .unitLifetime = *plan.request.lifetimeCancellation,
                .callerCancellation = plan.work.callerCancellation,
                .parameters = plan.parameters,
                .destination = plan.destination,
                .observation = plan.observation,
                .occupied = true,
                .observe = plan.work.observe};
        if (step.observe) {
            record.observationHeld[step.observation] = 1;
            record.observationCompleted[step.observation] = 0;
            record.observationSteps[step.observation] = step.identity;
        }
        record.scheduledGraph = graph;
        record.scheduledFrame = frame;
        record.computePass = plan.schedule.records[0].pass;
        record.graphBuffers = plan.resources;
        record.nextDestination = 1 + plan.destination % (record.states.size() - 1);
    }
}  // namespace Horo::Render::Detail

namespace Horo::Render {
    namespace {
        using namespace Detail;

        VfxGpuGraphSchedulingOutcome SnapshotAuthority(const VfxGpuGraphRequest &request, const VfxGpuRecord &record) noexcept {
            if (!request.preparation.IsLive() || request.preparation.DeviceIncarnation() != record.deviceIncarnation ||
                !SameVfxGpuDescriptor(request.preparation.Descriptor(), record.descriptor))
                return VfxGpuGraphSchedulingOutcome::Failure(VfxFrameErrors::InvalidInput);
            if (!request.lifetimeCancellation)
                return VfxGpuGraphSchedulingOutcome::Failure(VfxFrameErrors::InvalidInput);
            if (request.lifetimeCancellation->IsCancellationRequested())
                return VfxGpuGraphSchedulingOutcome::Failure(VfxFrameErrors::Cancelled);
            return VfxGpuGraphSchedulingOutcome::Success({});
        }

        Result<void> Uses(RenderGraphBuilder &builder, const RenderGraphPassRef pass, const VfxGpuAppendPlan &plan, const bool compute) {
            if (compute) {
                constexpr std::array access{RenderGraphAccess::Read, RenderGraphAccess::Write, RenderGraphAccess::Read,
                                            RenderGraphAccess::Write};
                for (std::size_t index = 0; index < access.size(); ++index)
                    if (const auto used = builder.AddUsage({pass, plan.resources[index], access[index], RenderGraphUsageKind::Storage});
                        used.HasError())
                        return used;
            } else {
                const auto particles = plan.dispatch ? plan.resources[1] : plan.resources[0];
                if (const auto used = builder.AddUsage({pass, particles, RenderGraphAccess::Read, RenderGraphUsageKind::Storage});
                    used.HasError())
                    return used;
                const auto colorAccess = plan.request.colorOperations.loadOperation == AttachmentLoadOperation::Load
                                             ? RenderGraphAccess::ReadWrite
                                             : RenderGraphAccess::Write;
                if (const auto used = builder.AddUsage({pass, plan.request.color, colorAccess, RenderGraphUsageKind::ColorAttachment});
                    used.HasError())
                    return used;
                if (plan.request.depth.IsValid())
                    return builder.AddUsage(
                        {pass, plan.request.depth, RenderGraphAccess::Read, RenderGraphUsageKind::DepthStencilAttachment});
            }
            return Result<void>::Success();
        }

        const ErrorCodeDescriptor *PrepareObservation(VfxGpuStorage &storage, VfxGpuAppendPlan &plan) noexcept {
            if (!plan.dispatch || !plan.work.observe)
                return nullptr;
            std::size_t request{};
            const auto status = RequestVfxGpuObservation(storage, plan.record, plan.work.step, plan.observation, request);
            if (status == RenderReadbackPreparedStatus::Complete)
                return nullptr;
            if (status != RenderReadbackPreparedStatus::CapacityExceeded)
                return &VfxFrameErrors::InvalidInput;
            if (plan.record.descriptor.readback.requirement != VfxGpuReadbackRequirement::Optional)
                return &VfxFrameErrors::Pending;
            plan.work.observe = false;
            plan.schedule.observationOmitted = true;
            if (!PackVfxGpuDispatchParameters(plan.record.descriptor, plan.work, plan.parameters))
                return &VfxFrameErrors::InvalidInput;
            std::get<RenderGraphVfxGpuCompute>(plan.schedule.records[0].workload).work = plan.work;
            return nullptr;
        }

        struct AppendBudget final {
            std::uint32_t passes{}, usages{};
        };

        AppendBudget RequiredBudget(const VfxGpuAppendPlan &plan) noexcept {
            return {plan.dispatch ? 2U : 1U, (plan.dispatch ? 4U : 0U) + 2U + (plan.request.depth.IsValid() ? 1U : 0U)};
        }

        bool FitsBudget(const VfxGpuGraphRequest &request, const AppendBudget budget) noexcept {
            return request.remainingDrawCalls > 0 && request.remainingPasses >= budget.passes && request.remainingUsages >= budget.usages;
        }

        Result<void> ComputePass(RenderGraphBuilder &builder, VfxGpuAppendPlan &plan) {
            if (!plan.dispatch)
                return Result<void>::Success();
            const auto pass = builder.AddPass(RenderPassKind::Compute, RenderQueueRole::Graphics);
            if (pass.HasError())
                return Result<void>::Failure(pass.ErrorValue());
            if (const auto used = Uses(builder, pass.Value(), plan, true); used.HasError())
                return used;
            auto &record = plan.record;
            plan.schedule.records[plan.schedule.recordCount++] = {pass.Value(),
                                                                  RenderGraphVfxGpuCompute{plan.resources[0], plan.resources[1],
                                                                                           plan.resources[2], plan.resources[3],
                                                                                           record.descriptor, plan.work, record.kernel,
                                                                                           record.native}};
            plan.producer = pass.Value();
            return Result<void>::Success();
        }

        Result<void> DrawPass(RenderGraphBuilder &builder, VfxGpuAppendPlan &plan) {
            const auto pass = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
            if (pass.HasError())
                return Result<void>::Failure(pass.ErrorValue());
            if (const auto used = Uses(builder, pass.Value(), plan, false); used.HasError())
                return used;
            if (const auto edge =
                    builder.AddDependency({plan.request.lightingComplete, pass.Value(), RenderGraphDependencyKind::ExecutionOrder});
                edge.HasError())
                return edge;
            if (plan.producer.IsValid())
                if (const auto edge = builder.AddDependency({plan.producer, pass.Value(), RenderGraphDependencyKind::ExecutionOrder});
                    edge.HasError())
                    return edge;
            if (const auto edge =
                    builder.AddDependency({pass.Value(), plan.request.outputTransform, RenderGraphDependencyKind::ExecutionOrder});
                edge.HasError())
                return edge;
            auto &record = plan.record;
            const auto generation =
                plan.dispatch || plan.producer.IsValid() ? plan.work.step.targetGeneration : record.generations[plan.draw];
            plan.schedule.records[plan.schedule.recordCount++] =
                {pass.Value(),
                 RenderGraphVfxGpuDraw{plan.dispatch ? plan.resources[1] : plan.resources[0], plan.request.color, plan.request.depth,
                                       record.id, record.deviceIncarnation, generation, record.descriptor, plan.request.view,
                                       plan.request.colorOperations, record.kernel, *plan.request.lifetimeCancellation, record.native}};
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RenderFrameScope::ScheduleVfxGpu */
    VfxGpuGraphSchedulingOutcome RenderFrameScope::ScheduleVfxGpu(RenderGraphBuilder &builder, const VfxGpuGraphRequest &request) {
        if (!owner_ || !backend_ || executed_)
            return VfxGpuGraphSchedulingOutcome::Failure(FrontendErrors::FrameNotActive);
        return owner_->AppendVfxGpu(builder, request, hostFrame_);
    }

    /** @copydoc RenderFrontend::ValidateVfxGpuGraphRequest */
    VfxGpuGraphSchedulingOutcome RenderFrontend::ValidateVfxGpuGraphRequest(const RenderGraphBuilder &builder,
                                                                            const VfxGpuGraphRequest &request,
                                                                            const Detail::VfxGpuRecord &record) const {
        if (!builder.FindPass(request.lightingComplete) || !builder.FindPass(request.outputTransform) ||
            !builder.FindResource(request.color))
            return VfxGpuGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph);
        if (request.depth.IsValid() && !builder.FindResource(request.depth))
            return VfxGpuGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::InvalidGraph);
        if (const auto provenance = SnapshotAuthority(request, record); provenance.HasError())
            return provenance;
        std::array<std::uint32_t, VfxGpuDrawWords::Count> view;
        if (!PackVfxGpuDrawParameters(record.descriptor, request.view, view))
            return VfxGpuGraphSchedulingOutcome::Failure(VfxFrameErrors::InvalidInput);
        return VfxGpuGraphSchedulingOutcome::Success({});
    }

    /** @copydoc RenderFrontend::ImportVfxGpuPlan */
    Result<void> RenderFrontend::ImportVfxGpuPlan(RenderGraphBuilder &builder, Detail::VfxGpuAppendPlan &plan) {
        const auto queues = submissionAllocator_->Assignments();
        const auto graphics = std::ranges::find(queues, RenderQueueRole::Graphics, &RenderQueueAssignment::role);
        if (graphics == queues.end())
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidQueueTopology));
        for (std::size_t index = 0; index < plan.inputCount; ++index) {
            const auto existing = std::ranges::find_if(builder.resources_, [&](const auto &resource) {
                const auto *handle = std::get_if<RenderBufferHandle>(&resource.binding);
                return handle && *handle == plan.handles[index];
            });
            if (existing != builder.resources_.end()) {
                plan.resources[index] = existing->id;
                continue;
            }
            const auto imported = builder.ImportBuffer(plan.handles[index], RenderGraphResourceClass::Persistent);
            if (imported.HasError())
                return Result<void>::Failure(imported.ErrorValue());
            plan.resources[index] = imported.Value();
            const bool initialized = !plan.dispatch || index == 0 || index == 2;
            const auto queue = graphics->queue;
            const RenderGraphLogicalState state =
                initialized ? RenderGraphLogicalState{RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::Storage,
                                                      RenderGraphPipelineScope::External, RenderGraphTextureLayout::NotApplicable, queue}
                            : RenderGraphLogicalState{};
            plan.schedule.initialStates[plan.schedule.initialStateCount++] = {imported.Value(), state,
                                                                              initialized ? RenderGraphInitialContents::Preserved
                                                                                          : RenderGraphInitialContents::Discarded};
            ++plan.schedule.resources;
        }
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::AdmitVfxGpuGraph */
    VfxGpuGraphSchedulingOutcome RenderFrontend::AdmitVfxGpuGraph(const RenderGraphBuilder &builder, const VfxGpuGraphRequest &request,
                                                                  Detail::VfxGpuRecord *&record) const {
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return VfxGpuGraphSchedulingOutcome::Failure(owner.ErrorValue());
        if (!vfxGpuStorage_ || vfxGpuStorage_->busy)
            return VfxGpuGraphSchedulingOutcome::Failure(VfxFrameErrors::Pending);
        record = vfxGpuStorage_->Find(request.preparation.Id());
        if (!record || !record->native)
            return VfxGpuGraphSchedulingOutcome::Failure(VfxFrameErrors::InvalidInput);
        if (builder.ownerThread_ != std::this_thread::get_id())
            return VfxGpuGraphSchedulingOutcome::Failure(RenderGraphErrors::WrongThread);
        if (builder.state_ != RenderGraphBuilderState::Open)
            return VfxGpuGraphSchedulingOutcome::Failure(RenderGraphErrors::BuilderClosed);
        return ValidateVfxGpuGraphRequest(builder, request, *record);
    }

    /** @copydoc RenderFrontend::AppendVfxGpu */
    VfxGpuGraphSchedulingOutcome RenderFrontend::AppendVfxGpu(RenderGraphBuilder &builder, const VfxGpuGraphRequest &request,
                                                              const std::uint64_t frame) {
        Detail::VfxGpuRecord *record{};
        if (const auto admitted = AdmitVfxGpuGraph(builder, request, record); admitted.HasError())
            return admitted;
        Detail::VfxGpuAppendPlan plan{*record, request};
        if (const auto *failure = Detail::PlanVfxGpu(plan, builder.Owner()))
            return VfxGpuGraphSchedulingOutcome::Failure(*failure);
        return AppendVfxGpuPlan(builder, plan, frame);
    }

    /** @copydoc RenderFrontend::AppendVfxGpuPlan */
    VfxGpuGraphSchedulingOutcome RenderFrontend::AppendVfxGpuPlan(RenderGraphBuilder &builder, Detail::VfxGpuAppendPlan &plan,
                                                                  const std::uint64_t frame) {
        const auto budget = RequiredBudget(plan);
        if (!FitsBudget(plan.request, budget))
            return VfxGpuGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::VfxDrawBudgetExceeded);
        const auto rollback = [&builder, passes = builder.passes_.size(), resources = builder.resources_.size(),
                               usages = builder.usages_.size(), dependencies = builder.dependencies_.size()](void *) noexcept {
            builder.passes_.resize(passes);
            builder.resources_.resize(resources);
            builder.exportedResources_.resize(resources);
            builder.usages_.resize(usages);
            builder.dependencies_.resize(dependencies);
        };
        std::unique_ptr<void, decltype(rollback)> transaction{&builder, rollback};
        if (const auto imported = ImportVfxGpuPlan(builder, plan); imported.HasError())
            return VfxGpuGraphSchedulingOutcome::Failure(imported.ErrorValue());
        if (plan.schedule.resources > plan.request.remainingResources)
            return VfxGpuGraphSchedulingOutcome::Failure(RenderGraphExecutionErrors::VfxDrawBudgetExceeded);
        if (const auto compute = ComputePass(builder, plan); compute.HasError())
            return VfxGpuGraphSchedulingOutcome::Failure(compute.ErrorValue());
        if (const auto draw = DrawPass(builder, plan); draw.HasError())
            return VfxGpuGraphSchedulingOutcome::Failure(draw.ErrorValue());
        if (const auto *observation = PrepareObservation(*vfxGpuStorage_, plan))
            return VfxGpuGraphSchedulingOutcome::Failure(*observation);
        plan.schedule.drawCalls = 1;
        plan.schedule.usages = budget.usages;
        plan.schedule.dispatched = plan.dispatch;
        if (plan.record.acceptedPoint.IsValid())
            plan.schedule.requiredWaits[plan.schedule.requiredWaitCount++] = plan.record.acceptedPoint;
        Detail::CommitVfxGpuPlan(plan, builder.Owner(), frame);
        static_cast<void>(transaction.release());
        return VfxGpuGraphSchedulingOutcome::Success(std::move(plan.schedule));
    }
}  // namespace Horo::Render
