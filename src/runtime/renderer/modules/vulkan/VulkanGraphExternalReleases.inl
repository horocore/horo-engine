/** @brief Encodes qualified OpenXR graphics-queue handoff requirements after the exact final producer.
 * @details Called inside PreparePass's after-barrier range, before any command recording/submission.
 * This extends the canonical graph compiler and command runtime, never a parallel queue or completion ledger.
 */
Result<void> VulkanGraphRuntime::Impl::PrepareExternalReleases(const RenderGraphExecutionRequest &request,
                                                               const RenderGraphExecutionPass &pass) {
    for (const auto &release : request.graph.ExternalReleases()) {
        if (release.releaseAfter != pass.pass)
            continue;
        const std::size_t index = release.resource.value - 1;
        const auto *source = resources->ExternalTextureSource(request.resources[index].instance);
        const auto *queue = Queue(pass.queue);
        if (source == nullptr || queue == nullptr || !queue->supportsGraphics || queue->family != source->NativeQueueFamily())
            return Result<void>::Failure(
                Unsupported("External release requires the exact native source and OpenXR-bound graphics queue family."));
        if (auto current = source->Validate(); current.HasError())
            return current;
        if (release.oldState != states[index].logical || release.newState != source->RequiredFinalState() ||
            release.newState.queue != pass.queue || release.newState.queue != source->AcquiredState().queue)
            return Result<void>::Failure(Invalid("External final state differs from actual native image ownership or its last producer."));
        auto nativeFinal = release.newState;
        nativeFinal.scope = RenderGraphPipelineScope::Graphics;
        auto barrier = TranslateVulkanGraphBarrier(bindings[index], release.oldState, nativeFinal, VK_QUEUE_FAMILY_IGNORED,
                                                   VK_QUEUE_FAMILY_IGNORED, false, false, false);
        if (barrier.HasError())
            return Result<void>::Failure(std::move(barrier).ErrorValue());
        if (barriers.size() == barriers.capacity())
            return Result<void>::Failure(Invalid("Prepared external-release barrier capacity was exceeded."));
        barriers.push_back(barrier.Value());
        states[index].logical = nativeFinal;
        // Only the current producer's final update changes. Rewriting prior submissions' updates
        // would claim this handoff before a later failed submission had actually executed it.
        bool updated{};
        for (auto update = updates.rbegin(); update != updates.rend(); ++update) {
            if (update->target != bindings[index].state)
                continue;
            update->value = states[index];
            updated = true;
            break;
        }
        if (!updated)
            return Result<void>::Failure(Invalid("External release has no current native producer state update."));
    }
    return Result<void>::Success();
}
