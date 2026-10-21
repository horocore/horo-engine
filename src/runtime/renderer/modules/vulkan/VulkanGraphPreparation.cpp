#include "VulkanGraphRuntimeInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
#include "VulkanGraphExternalReleases.inl"

    /** @brief Validates exact imported generation evidence against native committed state. */
    Result<void> VulkanGraphRuntime::Impl::InitialStates(const RenderGraphExecutionRequest &request) {
        const auto graphResources = request.graph.Resources();
        const auto imported = request.graph.ImportedStates();
        if (imported.size() != graphResources.size())
            return Result<void>::Failure(Unsupported("Vulkan graph imports require one exact initial-state record per resident resource."));
        std::fill(used.begin(), used.end(), false);
        std::fill(discarded.begin(), discarded.end(), false);
        for (std::size_t index = 0; index < graphResources.size(); ++index) {
            const auto &resource = graphResources[index];
            if (resource.id.value != index + 1 || bindings[index].state == nullptr)
                return Result<void>::Failure(Invalid("Graph resource ordering/native state identity is invalid."));
            const auto *initial = &imported[index];
            if (initial->resource != resource.id)
                return Result<void>::Failure(Invalid("Canonical imported generation state is missing or foreign."));
            if (auto external = ValidateExternalAcquisition(request, index); external.HasError())
                return external;
            states[index] = *bindings[index].state;
            discarded[index] = initial->contents == RenderGraphInitialContents::Discarded;
            if (discarded[index]) {
                states[index].logical = {};
                states[index].contentsDefined = false;
            } else {
                if (states[index].hasNativeState && states[index].logical != initial->state)
                    return Result<void>::Failure(
                        Invalid("Imported generation state differs from the native successfully submitted state."));
                if (!states[index].hasNativeState && !states[index].contentsDefined)
                    return Result<void>::Failure(Invalid("Fresh undefined native storage cannot claim preserved imported contents."));
                auto translated = TranslateVulkanGraphBarrier(bindings[index], initial->state, initial->state, VK_QUEUE_FAMILY_IGNORED,
                                                              VK_QUEUE_FAMILY_IGNORED, false, false, false);
                if (translated.HasError())
                    return Result<void>::Failure(std::move(translated).ErrorValue());
                states[index].logical = initial->state;
            }
        }
        return Result<void>::Success();
    }

    /** @brief Prepares every state, binding and workload before any command is recorded. */
    Result<void> VulkanGraphRuntime::Impl::Prepare(const RenderGraphExecutionRequest &request) {
        barriers.clear();
        updates.clear();
        submissions.clear();
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
                if (auto prepared = PreparePass(request, pass); prepared.HasError())
                    return prepared;
            }
            submission.updates.count = updates.size() - submission.updates.offset;
            for (auto &update : std::span{updates}.subspan(submission.updates.offset, submission.updates.count))
                update.value.completion = record.submission.signal;
        }
        return Result<void>::Success();
    }

    /** @brief Preserves matched release/acquire layout and family evidence without synthesizing external release work. */
    Result<void> VulkanGraphRuntime::Impl::PrepareTransitions(const RenderGraphExecutionRequest &request,
                                                              const RenderGraphExecutionPass &pass, const bool releases) {
        const auto &graph = request.graph;
        const auto append = [&](const RenderGraphTransition &transition, const bool release, const bool acquire) -> Result<void> {
            const auto index = transition.resource.value - 1;
            const auto *source = Queue(transition.oldState.queue);
            const auto *destination = Queue(transition.newState.queue);
            if (destination == nullptr || ((release || acquire) && source == nullptr))
                return Result<void>::Failure(Unsupported("Cross-graph queue ownership lacks matched native release evidence."));
            const auto sourceFamily = release || acquire ? source->family : VK_QUEUE_FAMILY_IGNORED;
            const auto destinationFamily = release || acquire ? destination->family : VK_QUEUE_FAMILY_IGNORED;
            if (!release && states[index].logical != transition.oldState)
                return Result<void>::Failure(Invalid("Compiled transition source differs from exact tracked graph state."));
            const bool priorDiscardedAccess = discarded[index] && !used[index] && bindings[index].state->hasNativeState;
            auto translated = TranslateVulkanGraphBarrier(bindings[index], transition.oldState, transition.newState, sourceFamily,
                                                          destinationFamily, release, acquire, priorDiscardedAccess);
            if (translated.HasError())
                return Result<void>::Failure(std::move(translated).ErrorValue());
            if (barriers.size() == barriers.capacity())
                return Result<void>::Failure(Invalid("Prepared native graph barrier capacity was exceeded."));
            barriers.push_back(translated.Value());
            if (!release)
                states[index].logical = transition.newState;
            return Result<void>::Success();
        };
        if (releases) {
            for (const auto &transfer : graph.ReleaseTransfers().subspan(pass.releaseTransfers.offset, pass.releaseTransfers.count)) {
                const auto transition = std::ranges::find_if(graph.Transitions(), [&](const RenderGraphTransition &candidate) {
                    return candidate.resource == transfer.resource && candidate.after == transfer.acquireBefore &&
                           candidate.oldState.queue == transfer.sourceQueue && candidate.newState.queue == transfer.destinationQueue;
                });
                if (transition == graph.Transitions().end())
                    return Result<void>::Failure(Invalid("Ownership release has no exact matching state transition."));
                if (auto added = append(*transition, true, false); added.HasError())
                    return added;
            }
        } else {
            for (const auto &transition : graph.Transitions().subspan(pass.transitions.offset, pass.transitions.count)) {
                const bool acquire =
                    std::ranges::any_of(graph.AcquireTransfers().subspan(pass.acquireTransfers.offset, pass.acquireTransfers.count),
                                        [&](const RenderGraphOwnershipTransfer &transfer) {
                    return transfer.resource == transition.resource;
                });
                if (auto added = append(transition, false, acquire); added.HasError())
                    return added;
            }
        }
        return Result<void>::Success();
    }

    /** @brief Validates contents and commits only scratch updates, later attached to each exact successful submission. */
    Result<void> VulkanGraphRuntime::Impl::PreparePass(const RenderGraphExecutionRequest &request, const std::size_t index) {
        const auto &pass = request.graph.Passes()[index];
        auto &prepared = passes[index];
        const auto usages = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
        for (const auto &usage : usages) {
            const auto resourceIndex = usage.resource.value - 1;
            const auto &actual = *bindings[resourceIndex].state;
            if (!used[resourceIndex] && actual.hasNativeState && actual.logical.queue != pass.queue)
                return Result<void>::Failure(
                    Unsupported("Cross-graph queue change requires matched previous release evidence, which this import does not carry."));
            if (usage.access != RenderGraphAccess::Write && !states[resourceIndex].contentsDefined)
                return Result<void>::Failure(Invalid("Native graph read observes undefined whole-resource contents."));
        }
        prepared.before.offset = barriers.size();
        if (auto transitions = PrepareTransitions(request, pass, false); transitions.HasError())
            return transitions;
        prepared.before.count = barriers.size() - prepared.before.offset;
        if (auto workload = ValidateWorkload(request.workloads[index].workload, pass.queue); workload.HasError())
            return workload;
        for (const auto &usage : usages) {
            const auto resourceIndex = usage.resource.value - 1;
            if (states[resourceIndex].logical.queue != pass.queue)
                return Result<void>::Failure(Invalid("Graph use has no exact native ownership/state transition."));
            states[resourceIndex].hasNativeState = true;
            used[resourceIndex] = true;
            updates.push_back(Update{bindings[resourceIndex].state, states[resourceIndex]});
        }
        prepared.after.offset = barriers.size();
        if (auto transitions = PrepareTransitions(request, pass, true); transitions.HasError())
            return transitions;
        if (auto external = PrepareExternalReleases(request, pass); external.HasError())
            return external;
        prepared.after.count = barriers.size() - prepared.after.offset;
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
