#include "D3D12NativeCommandsInternal.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {
    namespace {
        /** @brief Checks exact physical kind before using any native interface. */
        bool MatchingResourceKind(const RenderGraphResource &graph, const Resource &native) noexcept {
            if (graph.kind == RenderGraphResourceKind::Buffer)
                return native.kind == Resource::Kind::Buffer;
            return native.kind == Resource::Kind::Texture;
        }

        /** @brief Canonical buffer creation flags remain the authority for every graph access. */
        std::optional<RenderBufferUsage> RequiredBufferUsage(const RenderGraphUsageKind kind) noexcept {
            switch (kind) {
                case RenderGraphUsageKind::Sampled:
                case RenderGraphUsageKind::Storage:
                    return RenderBufferUsage::Storage;
                case RenderGraphUsageKind::CopySource:
                    return RenderBufferUsage::CopySource;
                case RenderGraphUsageKind::CopyDestination:
                    return RenderBufferUsage::CopyDestination;
                case RenderGraphUsageKind::VertexBuffer:
                    return RenderBufferUsage::Vertex;
                case RenderGraphUsageKind::IndexBuffer:
                    return RenderBufferUsage::Index;
                case RenderGraphUsageKind::UniformBuffer:
                    return RenderBufferUsage::Uniform;
                default:
                    return std::nullopt;
            }
        }

        std::optional<RenderTextureUsage> RequiredTextureUsage(const RenderGraphUsageKind kind) noexcept {
            switch (kind) {
                case RenderGraphUsageKind::Sampled:
                    return RenderTextureUsage::Sampled;
                case RenderGraphUsageKind::Storage:
                    return RenderTextureUsage::Storage;
                case RenderGraphUsageKind::ColorAttachment:
                case RenderGraphUsageKind::DepthStencilAttachment:
                    return RenderTextureUsage::RenderAttachment;
                case RenderGraphUsageKind::CopySource:
                    return RenderTextureUsage::CopySource;
                case RenderGraphUsageKind::CopyDestination:
                    return RenderTextureUsage::CopyDestination;
                default:
                    return std::nullopt;
            }
        }

        bool NativeUsageSupported(const Resource &resource, const RenderGraphResourceUsage &usage) noexcept {
            if (resource.kind == Resource::Kind::Buffer) {
                const auto required = RequiredBufferUsage(usage.kind);
                return required && HasBufferUsage(resource.buffer.usage, *required);
            }
            const auto required = RequiredTextureUsage(usage.kind);
            if (!required || !HasTextureUsage(resource.texture.usage, *required))
                return false;
            if (usage.kind == RenderGraphUsageKind::ColorAttachment)
                return !IsDepth(resource.texture.format);
            if (usage.kind == RenderGraphUsageKind::DepthStencilAttachment)
                return IsDepth(resource.texture.format);
            return true;
        }

        /** @brief Immutable upload heaps admit only read-compatible usages, never illegal destination/state transitions. */
        bool HostUsageSupported(const Resource &resource, const RenderGraphResourceUsage &usage) noexcept {
            if (resource.kind != Resource::Kind::Buffer || resource.buffer.access != RenderBufferAccess::HostVisible)
                return true;
            if (usage.access != RenderGraphAccess::Read)
                return false;
            return usage.kind == RenderGraphUsageKind::CopySource || usage.kind == RenderGraphUsageKind::VertexBuffer ||
                   usage.kind == RenderGraphUsageKind::IndexBuffer || usage.kind == RenderGraphUsageKind::UniformBuffer;
        }
    }  // namespace

    std::size_t NativeCommandRuntime::ResourceIndex(const RenderGraphResourceId identity) const noexcept {
        const auto found = std::ranges::find(resourceIds_, identity);
        return static_cast<std::size_t>(found - resourceIds_.begin());
    }

    namespace {
        /** @brief Physical overlap uses the canonical admitted heap record, not graph identity or guessed resource state. */
        bool OverlappingPlacement(const Resource &first, const Resource &second) noexcept {
            return first.heap && first.heap == second.heap && first.offsetBytes < second.offsetBytes + second.requiredBytes &&
                   second.offsetBytes < first.offsetBytes + first.requiredBytes;
        }
    }  // namespace

    Result<void> NativeCommandRuntime::AdmitDistinctResource(const std::shared_ptr<Resource> &resource) const {
        for (const auto &previous : boundResources_) {
            if (!previous)
                continue;
            if (previous == resource)
                return Result<void>::Failure(ResourceError("render.d3d12.command_alias_unsupported",
                                                           "This command adapter requires distinct used canonical "
                                                           "resources; logical transient reuse is not native admission."));
            if (OverlappingPlacement(*previous, *resource))
                return Result<void>::Failure(
                    ResourceError("render.d3d12.command_alias_unsupported",
                                  "Overlapping placed graph resources require a separate exact native alias schedule."));
        }
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::BindResources(const RenderGraphExecutionRequest &request) {
        for (const auto &graph : request.graph.Resources()) {
            const auto found = std::ranges::find(request.resources, graph.id, &RenderGraphResourceInstance::resource);
            const bool used = std::ranges::any_of(request.graph.Usages(), [&graph](const auto &usage) {
                return usage.resource == graph.id;
            });
            if (!used) {
                boundResources_.push_back(nullptr);
                resourceIds_.push_back(graph.id);
                continue;
            }
            if (found == request.resources.end() || found->instance == 0)
                return Result<void>::Failure(
                    ResourceError("render.d3d12.command_resource", "Bind every used graph resource to its exact live native instance."));
            auto native = resources_.Find(found->instance);
            if (!native || !MatchingResourceKind(graph, *native))
                return Result<void>::Failure(
                    ResourceError("render.d3d12.command_resource", "Use live matching resources from this canonical device service."));
            if (auto distinct = AdmitDistinctResource(native); distinct.HasError())
                return distinct;
            boundResources_.push_back(std::move(native));
            resourceIds_.push_back(graph.id);
        }
        logicalStates_.resize(boundResources_.size());
        activationRecorded_.resize(boundResources_.size(), false);
        for (const auto &imported : request.graph.ImportedStates())
            logicalStates_[ResourceIndex(imported.resource)] = imported.state;
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::ValidateResourceBoundaries(const RenderGraphExecutionRequest &request) const {
        for (const auto &release : request.graph.ExternalReleases())
            if (release.newState.operation != RenderGraphSynchronizationOperation::None ||
                release.newState.access != RenderGraphSynchronizationAccess::None)
                return Result<void>::Failure(ResourceError("render.d3d12.command_external_state",
                                                           "This resource service publishes COMMON boundaries; non-COMMON "
                                                           "external handoffs require a separate admitted adapter."));
        for (const auto &usage : request.graph.Usages()) {
            const auto &resource = *boundResources_[ResourceIndex(usage.resource)];
            if (!NativeUsageSupported(resource, usage))
                return Result<void>::Failure(
                    ResourceError("render.d3d12.command_resource",
                                  "Graph use must match the canonical native allocation's declared usage and format."));
            if (!HostUsageSupported(resource, usage))
                return Result<void>::Failure(ResourceError("render.d3d12.command_resource",
                                                           "Upload heaps remain GENERIC_READ and cannot be graph write destinations."));
        }
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::ValidatePassStates(const RenderGraphExecutionRequest &request) const {
        for (const auto &pass : request.graph.Passes()) {
            const auto *queue = FindQueue(pass.queue);
            if (!queue)
                return Result<void>::Failure(
                    ResourceError("render.d3d12.command_queue", "Use an explicitly initialized logical queue mapping."));
            for (const auto &transition : request.graph.Transitions().subspan(pass.transitions.offset, pass.transitions.count))
                if (auto valid = ValidateCommandState(transition.newState, queue->kind); valid.HasError())
                    return valid;
        }
        for (const auto &imported : request.graph.ImportedStates()) {
            const auto *queue = FindQueue(imported.state.queue);
            if (queue)
                if (auto valid = ValidateCommandState(imported.state, queue->kind); valid.HasError())
                    return valid;
        }
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::ValidateWorkloads(const RenderGraphExecutionRequest &request) const {
        for (const auto &pass : request.workloads) {
            if (!std::holds_alternative<std::monostate>(pass.workload) && !std::holds_alternative<RenderGraphBufferCopy>(pass.workload))
                return Result<void>::Failure(ResourceError("render.d3d12.command_workload_unsupported",
                                                           "This command component encodes ordering and buffer copies; graphics/effect "
                                                           "execution belongs to its separately admitted pipeline component."));
            if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&pass.workload)) {
                const auto &source = *boundResources_[ResourceIndex(copy->source)];
                const auto &destination = *boundResources_[ResourceIndex(copy->destination)];
                if (!resources_.ValidRange(source, copy->sourceOffset, copy->byteCount, true) ||
                    !resources_.ValidRange(destination, copy->destinationOffset, copy->byteCount, false))
                    return Result<void>::Failure(
                        ResourceError("render.d3d12.command_resource", "Use bounded copy-compatible native buffer ranges."));
            }
        }
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::CheckGraphAdmission(const RenderGraphExecutionRequest &request) {
        if (!accepting_ || !resources_.accepting_ || !prepared_.empty())
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_closed", "Finish preparation or shutdown before admitting more command work."));
        if (!request.lease || !request.transfer)
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_lease", "Provide the stable graph lease and synchronous ownership-transfer receipt."));
        if (auto polled = Poll(); polled.HasError())
            return polled;
        return resources_.Poll();
    }

    Result<void> NativeCommandRuntime::AdmitGraphResources(const RenderGraphExecutionRequest &request) {
        const std::uint64_t barrierBound = std::uint64_t(request.graph.Usages().size()) * 2 +
                                           std::uint64_t(request.graph.Resources().size()) * request.submissions.size() +
                                           request.graph.ReleaseTransfers().size();
        if (barrierBound > limits_.maximumTransitions)
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_capacity",
                              "Graph, COMMON-boundary and placed-activation barriers exceed the admitted native envelope."));
        if (auto bound = BindResources(request); bound.HasError())
            return bound;
        if (auto boundaries = ValidateResourceBoundaries(request); boundaries.HasError())
            return boundaries;
        if (auto states = ValidatePassStates(request); states.HasError())
            return states;
        return ValidateWorkloads(request);
    }

    Result<void> NativeCommandRuntime::AcquireGraphLease() {
        const auto available = std::ranges::find_if(leaseGroups_, [](const auto &group) {
            return group.use_count() == 1;
        });
        if (available == leaseGroups_.end())
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_capacity", "Retire an existing bounded graph lease before admission."));
        preparedLease_ = *available;
        HORO_INVARIANT(!preparedLease_->owned && !preparedLease_->lease);
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::PrepareGraph(const RenderGraphExecutionRequest &request, const CancellationToken &cancellation) {
        if (auto admitted = CheckGraphAdmission(request); admitted.HasError())
            return admitted;
        if (auto resources = AdmitGraphResources(request); resources.HasError())
            return resources;
        if (auto lease = AcquireGraphLease(); lease.HasError())
            return lease;
        for (const auto &submission : request.submissions) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(
                    ResourceError("render.d3d12.command_cancelled", "Graph preparation cancelled before native execution."));
            if (auto prepared = PrepareSubmission(request, submission); prepared.HasError())
                return prepared;
        }
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::PrepareSubmission(const RenderGraphExecutionRequest &request,
                                                         const RenderGraphQueueSubmission &submission) {
        auto *queue = FindQueue(submission.submission.queue);
        if (!queue)
            return Result<void>::Failure(ResourceError("render.d3d12.command_queue", "Use an initialized native queue."));
        const auto slot = std::ranges::find_if(queue->slots, [](const CommandSlot &candidate) {
            return candidate.signal == 0;
        });
        if (slot == queue->slots.end())
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_capacity", "Poll real allocator retirement before submitting more bounded work."));
        if (submission.submission.waits.size() > RenderGraphSubmissionLimits::MaximumSubmissions)
            return Result<void>::Failure(ResourceError("render.d3d12.command_capacity",
                                                       "GPU dependency waits exceed the startup-reserved canonical submission envelope."));
        slot->signal = submission.submission.signal.value;
        slot->waits.assign(submission.submission.waits.begin(), submission.submission.waits.end());
        slot->pins.assign(boundResources_.begin(), boundResources_.end());
        slot->graphLease = preparedLease_;
        prepared_.push_back({static_cast<std::size_t>(queue - queues_.data()), static_cast<std::size_t>(slot - queue->slots.begin())});
        HRESULT status = slot->allocator->Reset();
        if (SUCCEEDED(status))
            status = slot->list->Reset(slot->allocator.Get(), nullptr);
        if (FAILED(status))
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_reset_failed", "Reset only an actually retired allocator/list pair.", status));
        if (auto recorded = RecordSubmission(request, submission, *queue, *slot); recorded.HasError())
            return recorded;
        status = slot->list->Close();
        if (FAILED(status))
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_close_failed", "The complete native batch failed command-list validation.", status));
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::RecordCopy(const RenderGraphBufferCopy &copy, ID3D12GraphicsCommandList &list) {
        const auto &source = boundResources_[ResourceIndex(copy.source)];
        const auto &destination = boundResources_[ResourceIndex(copy.destination)];
        list.CopyBufferRegion(destination->native.Get(), copy.destinationOffset, source->native.Get(), copy.sourceOffset, copy.byteCount);
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::RecordPass(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                  CommandQueue &queue, CommandSlot &slot,
                                                  std::vector<RenderGraphLogicalState> &nativeStates) {
        for (const auto &transition : request.graph.Transitions().subspan(pass.transitions.offset, pass.transitions.count))
            logicalStates_[ResourceIndex(transition.resource)] = transition.newState;
        for (const auto &usage : request.graph.Usages().subspan(pass.usages.offset, pass.usages.count)) {
            const auto index = ResourceIndex(usage.resource);
            auto &resource = *boundResources_[index];
            if (std::ranges::find(slot.accessed, index) == slot.accessed.end())
                slot.accessed.push_back(index);
            if (!activationRecorded_[index]) {
                RecordCommandActivation(*slot.list.Get(), resource);
                activationRecorded_[index] = true;
            }
            if (auto valid = ValidateCommandState(logicalStates_[index], queue.kind); valid.HasError())
                return valid;
            RecordCommandBarrier(*slot.list.Get(), slot.enhanced.Get(), resource, nativeStates[index], logicalStates_[index], queue.kind);
            nativeStates[index] = logicalStates_[index];
        }
        const auto workload = std::ranges::find(request.workloads, pass.pass, &RenderGraphPassWorkload::pass);
        if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload->workload))
            return RecordCopy(*copy, *slot.list.Get());
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::RecordSubmission(const RenderGraphExecutionRequest &request,
                                                        const RenderGraphQueueSubmission &submission, CommandQueue &queue,
                                                        CommandSlot &slot) {
        nativeStates_.assign(boundResources_.size(), {});
        auto &nativeStates = nativeStates_;
        for (const auto &pass : request.graph.Passes().subspan(submission.passes.offset, submission.passes.count)) {
            if (auto recorded = RecordPass(request, pass, queue, slot, nativeStates); recorded.HasError())
                return recorded;
            for (const auto &release :
                 request.graph.ReleaseTransfers().subspan(pass.releaseTransfers.offset, pass.releaseTransfers.count)) {
                const auto index = ResourceIndex(release.resource);
                RecordCommandBarrier(*slot.list.Get(), slot.enhanced.Get(), *boundResources_[index], nativeStates[index], {}, queue.kind);
                nativeStates[index] = {};
            }
        }
        for (std::size_t index = 0; index < nativeStates.size(); ++index)
            if (nativeStates[index].operation != RenderGraphSynchronizationOperation::None)
                RecordCommandBarrier(*slot.list.Get(), slot.enhanced.Get(), *boundResources_[index], nativeStates[index], {}, queue.kind);
        return Result<void>::Success();
    }
}  // namespace Horo::Render::D3D12Detail
#endif
