#include "VulkanGraphRuntimeInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
#include "VulkanGraphExternalReleases.inl"

    /** @brief Validates exact imported generation evidence against native committed state. */
    Result<void> VulkanGraphRuntime::Impl::InitialStates(const RenderGraphExecutionRequest &request) {
        if (request.graph.ImportedStates().size() != request.graph.Resources().size())
            return Result<void>::Failure(Unsupported("Vulkan requires one exact initial state per resident resource."));
        for (std::vector<bool>::reference flag : used)
            flag = false;
        for (std::vector<bool>::reference flag : discarded)
            flag = false;
        for (std::size_t index = 0; index < request.graph.Resources().size(); ++index)
            if (const auto initial = InitializeImportedState(request, index); initial.HasError())
                return initial;
        return Result<void>::Success();
    }

    Result<void> VulkanGraphRuntime::Impl::InitializeImportedState(const RenderGraphExecutionRequest &request, std::size_t index) {
        const auto &resource = request.graph.Resources()[index];
        const auto &initial = request.graph.ImportedStates()[index];
        if (resource.id.value != index + 1 || !bindings[index].state || initial.resource != resource.id)
            return Result<void>::Failure(Invalid("Imported graph/native resource identity differs."));
        if (const auto external = ValidateExternalAcquisition(request, index); external.HasError())
            return external;
        states[index] = *bindings[index].state;
        discarded[index] = initial.contents == RenderGraphInitialContents::Discarded;
        if (!discarded[index])
            return PreserveImportedState(request, index);
        states[index].logical = {};
        states[index].contentsDefined = false;
        return Result<void>::Success();
    }

    Result<void> VulkanGraphRuntime::Impl::PreserveImportedState(const RenderGraphExecutionRequest &request, std::size_t index) {
        const auto &initial = request.graph.ImportedStates()[index];
        if (states[index].hasNativeState && states[index].logical != initial.state)
            return Result<void>::Failure(Invalid("Imported state differs from the actual successfully submitted native state."));
        if (!states[index].hasNativeState && !states[index].contentsDefined)
            return Result<void>::Failure(Invalid("Undefined native storage cannot claim preserved contents."));
        const auto translated = TranslateVulkanGraphBarrier(bindings[index], initial.state, initial.state,
                                                            {.particleHandoff = vfxGpu.Owns(bindings[index].buffer)});
        if (translated.HasError())
            return Result<void>::Failure(translated.ErrorValue());
        states[index].logical = initial.state;
        return Result<void>::Success();
    }

    /** @brief Prepares every state, binding and workload before any command is recorded. */
    Result<void> VulkanGraphRuntime::Impl::Prepare(const RenderGraphExecutionRequest &request, VulkanCommandRetention &pins) {
        barriers.clear();
        updates.clear();
        materialPassStateCount = 0;
        submissions.clear();
        vfxDraws.clear();
        identities.clear();
        for (std::size_t index = 0; index < request.resources.size(); ++index)
            identities.push_back(bindings[index].state);
        std::ranges::sort(identities, std::less<>{});
        if (std::ranges::adjacent_find(identities) != identities.end())
            return Result<void>::Failure(
                Unsupported("One actual resident generation cannot fork into independent graph state identities."));
        if (auto initial = InitialStates(request); initial.HasError())
            return initial;
        for (const auto &record : request.submissions) {
            if (Queue(record.submission.queue) == nullptr)
                return Result<void>::Failure(Invalid("A graph submission queue was not admitted by the selected native device."));
            submissions.emplace_back();
            auto &submission = submissions.back();
            submission.value = record.submission;
            std::ranges::copy(record.submission.waits, submission.waits.begin());
            submission.value.waits = std::span{submission.waits}.first(record.submission.waits.size());
            submission.passes = record.passes;
            submission.updates.offset = updates.size();
            for (std::size_t pass = record.passes.offset; pass < record.passes.offset + record.passes.count; ++pass) {
                if (auto prepared = PreparePass(request, pass, pins); prepared.HasError())
                    return prepared;
            }
            submission.updates.count = updates.size() - submission.updates.offset;
            for (auto &update : std::span{updates}.subspan(submission.updates.offset, submission.updates.count))
                update.value.completion = record.submission.signal;
        }
        return Result<void>::Success();
    }

    namespace {
        bool ComputeStateAvailable(const RenderGraphLogicalState &state, const VulkanCommandQueue *queue) {
            return state.scope != RenderGraphPipelineScope::Compute || (queue && queue->supportsCompute);
        }
    }  // namespace

    /** @copydoc VulkanGraphRuntime::Impl::AppendTransition */
    Result<VulkanGraphBarrierOptions> VulkanGraphRuntime::Impl::TransitionOptions(const RenderGraphTransition &transition, bool release,
                                                                                  bool acquire) const {
        using Outcome = Result<VulkanGraphBarrierOptions>;
        const auto index = transition.resource.value - 1;
        const auto *source = Queue(transition.oldState.queue), *destination = Queue(transition.newState.queue);
        const bool ownership = release || acquire;
        if (!destination || (ownership && !source))
            return Outcome::Failure(Unsupported("Queue ownership has no exact admitted native family."));
        const bool available =
            ComputeStateAvailable(transition.oldState, source) && ComputeStateAvailable(transition.newState, destination);
        return Outcome::Success({ownership ? source->family : VK_QUEUE_FAMILY_IGNORED,
                                 ownership ? destination->family : VK_QUEUE_FAMILY_IGNORED, release, acquire,
                                 discarded[index] && !used[index] && bindings[index].state->hasNativeState, available,
                                 vfxGpu.Owns(bindings[index].buffer)});
    }

    Result<void> VulkanGraphRuntime::Impl::AppendTransition(const RenderGraphTransition &transition, bool release, bool acquire) {
        const auto index = transition.resource.value - 1;
        if (!release && states[index].logical != transition.oldState)
            return Result<void>::Failure(Invalid("Transition source differs from tracked native state."));
        const auto options = TransitionOptions(transition, release, acquire);
        if (options.HasError())
            return Result<void>::Failure(options.ErrorValue());
        const auto translated = TranslateVulkanGraphBarrier(bindings[index], transition.oldState, transition.newState, options.Value());
        if (translated.HasError())
            return Result<void>::Failure(translated.ErrorValue());
        if (barriers.size() == barriers.capacity())
            return Result<void>::Failure(Invalid("Native barrier capacity exhausted."));
        barriers.push_back(translated.Value());
        if (!release)
            states[index].logical = transition.newState;
        return Result<void>::Success();
    }

    /** @brief Preserves matched release/acquire layout and family evidence without synthesizing external release work. */
    Result<void> VulkanGraphRuntime::Impl::PrepareTransitions(const RenderGraphExecutionRequest &request,
                                                              const RenderGraphExecutionPass &pass, const bool releases) {
        const auto &graph = request.graph;
        if (releases) {
            for (const auto &transfer : graph.ReleaseTransfers().subspan(pass.releaseTransfers.offset, pass.releaseTransfers.count)) {
                const auto transition = std::ranges::find_if(graph.Transitions(), [&](const RenderGraphTransition &candidate) {
                    return candidate.resource == transfer.resource && candidate.after == transfer.acquireBefore &&
                           candidate.oldState.queue == transfer.sourceQueue && candidate.newState.queue == transfer.destinationQueue;
                });
                if (transition == graph.Transitions().end())
                    return Result<void>::Failure(Invalid("Ownership release has no exact matching state transition."));
                if (auto added = AppendTransition(*transition, true, false); added.HasError())
                    return added;
            }
        } else {
            for (const auto &transition : graph.Transitions().subspan(pass.transitions.offset, pass.transitions.count)) {
                const bool acquire =
                    std::ranges::any_of(graph.AcquireTransfers().subspan(pass.acquireTransfers.offset, pass.acquireTransfers.count),
                                        [&](const RenderGraphOwnershipTransfer &transfer) {
                    return transfer.resource == transition.resource;
                });
                if (auto added = AppendTransition(transition, false, acquire); added.HasError())
                    return added;
            }
        }
        return Result<void>::Success();
    }

    /** @brief Validates contents and commits only scratch updates, later attached to each exact successful submission. */
    Result<void> VulkanGraphRuntime::Impl::ValidatePassUses(const RenderGraphExecutionRequest &request, std::size_t index) const {
        const auto &pass = request.graph.Passes()[index];
        for (const auto &usage : request.graph.Usages().subspan(pass.usages.offset, pass.usages.count)) {
            const auto resource = usage.resource.value - 1;
            const auto &actual = *bindings[resource].state;
            if (!used[resource] && actual.hasNativeState && actual.logical.queue != pass.queue)
                return Result<void>::Failure(Unsupported("Cross-graph queue change requires actual matched release evidence."));
            if (usage.access != RenderGraphAccess::Write && !states[resource].contentsDefined)
                return Result<void>::Failure(Invalid("Native read observes undefined whole-resource contents."));
        }
        return Result<void>::Success();
    }

    Result<void> VulkanGraphRuntime::Impl::PrepareClassicWork(const RenderGraphExecutionRequest &request, std::size_t index,
                                                              VulkanCommandRetention &pins) {
        const auto &work = request.workloads[index].workload;
        const auto *queue = Queue(request.graph.Passes()[index].queue);
        if (const auto *scene = std::get_if<RenderGraphMultiview>(&work)) {
            if (!queue || !queue->supportsGraphics)
                return Result<void>::Failure(Unsupported("Multiview requires actual graphics queue."));
            if (const auto prepared = multiview.Prepare(request, *scene, bindings, *resources, pins); prepared.HasError())
                return prepared;
        }
        if (std::holds_alternative<RenderGraphVfx>(work)) {
            if (!queue || !queue->supportsGraphics)
                return Result<void>::Failure(Unsupported("VFX raster requires actual graphics queue."));
            return PrepareVfx(request, index, pins);
        }
        return Result<void>::Success();
    }

    namespace {
        bool ParticleQueue(const VulkanCommandQueue *queue, bool compute, bool draw) {
            return queue && (!compute || queue->supportsCompute) && (!draw || queue->supportsGraphics);
        }
    }  // namespace

    Result<void> VulkanGraphRuntime::Impl::PrepareParticleWork(const RenderGraphExecutionRequest &request, std::size_t index) {
        const auto &work = request.workloads[index].workload;
        const bool compute = std::holds_alternative<RenderGraphVfxGpuCompute>(work),
                   draw = std::holds_alternative<RenderGraphVfxGpuDraw>(work);
        if (!compute && !draw)
            return Result<void>::Success();
        if (!ParticleQueue(Queue(request.graph.Passes()[index].queue), compute, draw))
            return Result<void>::Failure(Unsupported("Particle pass lacks queried native queue support."));
        if (const auto admitted = vfxGpu.Validate(request, index, bindings); admitted.HasError())
            return admitted;
        if (compute) {
            const auto &dispatch = std::get<RenderGraphVfxGpuCompute>(work);
            states[dispatch.destination.value - 1].contentsDefined = true;
            if (dispatch.work.observe)
                states[dispatch.observations.value - 1].contentsDefined = true;
        }
        if (draw)
            states[std::get<RenderGraphVfxGpuDraw>(work).color.value - 1].contentsDefined = true;
        return Result<void>::Success();
    }

    Result<void> VulkanGraphRuntime::Impl::FinishPass(const RenderGraphExecutionRequest &request, std::size_t index) {
        const auto &pass = request.graph.Passes()[index];
        const auto usages = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
        for (const auto &usage : usages) {
            const auto resource = usage.resource.value - 1;
            if (states[resource].logical.queue != pass.queue)
                return Result<void>::Failure(Invalid("Graph use has no exact ownership/state transition."));
            states[resource].hasNativeState = true;
            used[resource] = true;
            updates.emplace_back(bindings[resource].state, states[resource]);
        }
        passes[index].after.offset = barriers.size();
        if (const auto release = PrepareTransitions(request, pass, true); release.HasError())
            return release;
        if (const auto external = PrepareExternalReleases(request, pass); external.HasError())
            return external;
        if (const auto finished = FinishParticleStates(request, index); finished.HasError())
            return finished;
        passes[index].after.count = barriers.size() - passes[index].after.offset;
        return Result<void>::Success();
    }

    Result<void> VulkanGraphRuntime::Impl::PreparePass(const RenderGraphExecutionRequest &request, std::size_t index,
                                                       VulkanCommandRetention &pins) {
        // MaterialState's alias cache belongs to this pass, while updates retains the whole attempt.
        materialPassStateCount = 0;
        const auto &pass = request.graph.Passes()[index];
        auto &prepared = passes[index];
        prepared.vfxDraws = {};
        if (const auto valid = ValidatePassUses(request, index); valid.HasError())
            return valid;
        prepared.before.offset = barriers.size();
        if (const auto transitions = PrepareTransitions(request, pass, false); transitions.HasError())
            return transitions;
        if (const auto classic = PrepareClassicWork(request, index, pins); classic.HasError())
            return classic;
        if (const auto particles = PrepareParticleWork(request, index); particles.HasError())
            return particles;
        if (const auto work = ValidateWorkload(request.workloads[index].workload, pass.queue); work.HasError())
            return work;
        prepared.before.count = barriers.size() - prepared.before.offset;
        return FinishPass(request, index);
    }
}  // namespace Horo::Render::Detail
