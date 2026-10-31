#include "RuntimeUiAccessState.h"
#include "UiAnimationPublisher.h"
#include "UiSceneReconciliationInternal.h"

#include <exception>
#include <limits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Closes an unpublished incoming owner while its preparation continues to pin the source. */
        void RevokeIncomingAnimation(Detail::UiAnimationSourceReservation &source, const SceneRuntimeId scene) noexcept {
            const auto prepared = source.preparation.lock();
            if (!prepared)
                return;
            for (auto &incoming : prepared->incoming) {
                if (incoming.descriptor.owner.scene != scene)
                    continue;
                incoming.sceneRevoked = true;
                source.retiring = true;
            }
        }
    }  // namespace

    /** @copydoc UiSceneReconciliation::Storage::FlushSceneRevocations */
    void UiSceneReconciliation::Storage::FlushSceneRevocations() noexcept {
        if (collecting || publicationSeal)
            return;
        for (auto &entry : active) {
            if (!entry || !entry->sceneRevoked)
                continue;
            const auto free = std::ranges::find_if(retired, [](const auto &value) {
                return !value;
            });
            // Admission always reserves maximumInstances additional retired slots for forced revocation.
            if (free == retired.end())
                std::terminate();
            entry->publisher.Shutdown();
            free->emplace(std::move(*entry));
            entry.reset();
        }
    }

    /** @copydoc UiSceneReconciliation::FlushSceneRevocations */
    void UiSceneReconciliation::FlushSceneRevocations() noexcept {
        if (storage_)
            storage_.Get()->FlushSceneRevocations();
    }

    /** @copydoc UiSceneReconciliation::RevokeScene */
    void UiSceneReconciliation::RevokeScene(const SceneRuntimeId scene, const bool defer) noexcept {
        if (!storage_ || !scene.IsValid())
            return;
        bool changed = false;
        for (auto &entry : storage_.Get()->active) {
            if (!entry)
                continue;
            if (entry->descriptor.owner.scene == scene)
                changed = !std::exchange(entry->sceneRevoked, true) || changed;
            else if (entry->descriptor.providerScene == scene)
                changed = !std::exchange(entry->providerRevoked, true) || changed;
        }
        for (const auto &weak : storage_.Get()->preparations) {
            const auto candidate = weak.lock();
            if (!candidate || candidate->consumed || candidate->request.next != scene)
                continue;
            if (!std::exchange(candidate->abandoned, true))
                changed = true;
            for (const auto &source : candidate->preparedAnimations)
                if (const auto reservation = source.lock())
                    reservation->retiring = true;
        }
        if (changed && storage_.Get()->revision != std::numeric_limits<std::uint64_t>::max())
            ++storage_.Get()->revision;
        if (!defer)
            FlushSceneRevocations();
    }

    /** @copydoc RuntimeUiService::RevokeScene */
    void RuntimeUiService::RevokeScene(const SceneRuntimeId scene) const noexcept {
        if (!state_ || !state_->active || state_->thread != std::this_thread::get_id() || !scene.IsValid())
            return;
        for (const auto &weak : state_->scopes)
            if (const auto scope = weak.lock(); scope && scope->grant.scene == scene.value)
                scope->revoked.store(true);
        const auto matching = [&](const RuntimeUiInstanceId instance) {
            const auto slot = state_->registry.storage_.Get()->Find(instance);
            if (slot == state_->registry.storage_.Get()->active.size())
                return false;
            const auto &entry = *state_->registry.storage_.Get()->active[slot];
            return entry.descriptor.owner.scene == scene || entry.descriptor.providerScene == scene;
        };
        for (auto &dispatcher : state_->dispatchers)
            if (dispatcher.events && matching(dispatcher.instance))
                dispatcher.sceneRevoked = true;
        for (auto &action : state_->actions)
            if (action.occupied && matching(action.instance))
                action.sceneRevoked = true;
        for (const auto &weak : state_->animationSources) {
            const auto source = weak.lock();
            if (!source)
                continue;
            if (matching(source->instance))
                source->retiring = true;
            RevokeIncomingAnimation(*source, scene);
        }
        state_->registry.RevokeScene(scene, state_->dispatching || state_->scenePublication);
        state_->sceneRevocationsPending = true;
        Detail::FinishSceneRevocations(*state_);
    }
}  // namespace Horo::Runtime::Ui

namespace Horo::Runtime::Ui::Detail {
    /** @copydoc FinishSceneRevocations */
    void FinishSceneRevocations(RuntimeUiServiceState &owner) noexcept {
        if (!owner.sceneRevocationsPending || owner.dispatching || owner.scenePublication || owner.registry.storage_.Get()->collecting)
            return;
        owner.registry.FlushSceneRevocations();
        for (auto &dispatcher : owner.dispatchers)
            if (dispatcher.sceneRevoked)
                dispatcher = {};
        for (auto &action : owner.actions)
            if (action.sceneRevoked)
                action = {};
        owner.sceneRevocationsPending = false;
    }
}  // namespace Horo::Runtime::Ui::Detail
