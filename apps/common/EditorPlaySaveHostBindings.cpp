#include "EditorPlaySaveHostInternal.h"

#include <algorithm>
#include <new>
#include <tuple>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Compares frozen values independently of native code ownership or a hash. */
        bool SameDeclaration(const Gameplay::GameplayPersistenceDeclaration &a,
                             const Gameplay::GameplayPersistenceDeclaration &b) noexcept {
            const auto &x = a.descriptor;
            const auto &y = b.descriptor;
            return a.ownerIdentity == b.ownerIdentity && std::tie(x.participant, x.record, x.owner, x.moduleId, x.moduleVersion) ==
                                                             std::tie(y.participant, y.record, y.owner, y.moduleId, y.moduleVersion);
        }

        /** @brief Metadata never substitutes for the current installation's admitted adapter. */
        bool LiveDeclaration(const Editor::PlaySaveGameplayBinding &binding) noexcept {
            if (!binding.installation.CanUse() || !binding.adapter)
                return false;
            const auto &a = binding.declaration.descriptor;
            const auto &b = binding.installation.Descriptor();
            const auto &c = binding.adapter->Descriptor();
            const auto value = [](const auto &d) {
                return std::tie(d.participant, d.record, d.owner, d.moduleId, d.moduleVersion);
            };
            return value(a) == value(b) && value(a) == value(c);
        }
    }  // namespace

    /** @copydoc EditorPlaySaveHost::State::BindActiveGameplay */
    Result<void> EditorPlaySaveHost::State::BindActiveGameplay() {
        if (!liveBindings)
            return Result<void>::Success();
        const auto active = scenes.ActiveScene();
        if (!active)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreContextInvalid));
        auto identities = CapturePlaySaveIdentities(*active, schema->baseline, schema->identities);
        if (identities.HasError())
            return Result<void>::Failure(identities.ErrorValue());
        for (const auto &binding : *liveBindings) {
            const auto *type = std::get_if<Gameplay::BehaviorTypeId>(&binding.declaration.ownerIdentity);
            if (!type)
                continue;
            if (!behaviorOwner)
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
            auto target = behaviorOwner->BindingForActive(*type, identities.Value());
            if (target.HasError())
                return Result<void>::Failure(target.ErrorValue());
            const auto installed = binding.adapter->BindRestoreCandidate(std::move(target).Value());
            if (installed.HasError())
                return Result<void>::Failure(installed.ErrorValue());
            // The actual stable SourceSlot keeps the bound source; the original capture
            // registry retains its canonical proxy.
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveHost::State::RecomposeBindings */
    Result<void> EditorPlaySaveHost::State::RecomposeBindings() {
        if (const auto bound = BindActiveGameplay(); bound.HasError())
            return bound;
        return Compose();
    }

    /** @copydoc EditorPlaySaveHost::State::RegisterBehaviorLifecycle */
    Result<void> EditorPlaySaveHost::State::RegisterBehaviorLifecycle() {
        if (!schema->lifecycle)
            return behaviorOwner ? Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreContextInvalid)) : Result<void>::Success();
        if (!behaviorOwner || *schema->lifecycle != Gameplay::BehaviorLifecycleParticipant())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreContextInvalid));
        auto created = Gameplay::BehaviorLifecyclePersistenceAdapter::Create(behaviorOwner, [scene = &scenes, values = schema] {
            const auto active = scene->ActiveScene();
            if (!active)
                return Result<Runtime::PersistentEntityIdentityMap>::Failure(MakeError(Runtime::SaveErrors::RestoreContextInvalid));
            return CapturePlaySaveIdentities(*active, values->baseline, values->identities);
        });
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        lifecycle = std::move(created).Value();
        auto added = registry.Register(*schema->lifecycle, lifecycle);
        return added.HasError() ? Result<void>::Failure(added.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveHost::State::ValidateLiveBindings */
    Result<void> EditorPlaySaveHost::State::ValidateLiveBindings(std::span<const Editor::PlaySaveGameplayBinding> bindings) const {
        if (bindings.size() != schema->gameplay.size() || bindings.size() >= Runtime::MaximumSaveParticipantCount)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureRegistryStale));
        for (const auto &declaration : schema->gameplay) {
            const auto found = std::ranges::find_if(bindings, [&](const auto &binding) {
                return SameDeclaration(declaration, binding.declaration);
            });
            if (found == bindings.end() || !LiveDeclaration(*found))
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureRegistryStale));
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveHost::State::ReleaseRuntimeBindings */
    Result<void> EditorPlaySaveHost::State::ReleaseRuntimeBindings() {
        runtime.reset();
        participants = {};
        if (liveBindings)
            for (const auto &binding : *liveBindings) {
                const auto removed = registry.Unregister(binding.declaration.descriptor.participant.participant);
                if (removed.HasError())
                    return Result<void>::Failure(removed.ErrorValue());
            }
        if (schema->lifecycle) {
            const auto removed = registry.Unregister(schema->lifecycle->participant);
            if (removed.HasError())
                return Result<void>::Failure(removed.ErrorValue());
        }
        lifecycle.reset();
        const auto removed = registry.Unregister(PlaySaveSceneParticipant());
        if (removed.HasError())
            return Result<void>::Failure(removed.ErrorValue());
        barrier.reset();
        authority.reset();
        barrierOwners.clear();
        mutations.clear();
        liveBindings.reset();
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveHost::State::AdvanceModuleSuspension */
    Result<void> EditorPlaySaveHost::State::AdvanceModuleSuspension() {
        if (!suspending || request || storageWork || !mutations.empty())
            return Result<void>::Success();
        if (runtime && !runtime->IsSettled())
            return Result<void>::Success();
        const auto transient = scenes.RetirePublishedRestorePreparation();
        if (transient.HasError())
            return Result<void>::Failure(transient.ErrorValue());
        if (!transient.Value())
            return Result<void>::Success();
        if (const auto released = ReleaseRuntimeBindings(); released.HasError())
            return released;
        suspending = false;
        suspended = true;
        dirty = true;
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveHost::SuspendForModuleReload */
    bool EditorPlaySaveHost::SuspendForModuleReload() noexcept {
        if (std::this_thread::get_id() != state_->owner || state_->retiring)
            return false;
        if (state_->suspended)
            return true;
        state_->suspending = true;
        state_->dirty = true;
        try {
            if (state_->runtime)
                (void)state_->runtime->Retire();
            const auto advanced = state_->AdvanceModuleSuspension();
            if (advanced.HasError())
                state_->error = advanced.ErrorValue();
        } catch (...) {
            // Retain original owners until a later safe point can prove settlement.
        }
        return state_->suspended;
    }

    /** @copydoc EditorPlaySaveHost::HasRuntimeBinding */
    bool EditorPlaySaveHost::HasRuntimeBinding() const noexcept {
        return state_->runtime && !state_->suspending && !state_->suspended && !state_->retiring;
    }

    /** @copydoc EditorPlaySaveHost::RebindGameplay */
    Result<void> EditorPlaySaveHost::RebindGameplay(std::vector<Editor::PlaySaveGameplayBinding> bindings) {
        if (std::this_thread::get_id() != state_->owner || !state_->suspended || state_->retiring)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
        if (const auto valid = state_->ValidateLiveBindings(bindings); valid.HasError())
            return valid;
        try {
            state_->liveBindings = std::make_shared<const std::vector<Editor::PlaySaveGameplayBinding>>(std::move(bindings));
            const auto composed = state_->RecomposeBindings();
            if (composed.HasError()) {
                (void)state_->ReleaseRuntimeBindings();
                return composed;
            }
            state_->suspended = false;
            state_->error.reset();
            state_->dirty = true;
            return state_->Publish();
        } catch (const std::bad_alloc &) {
            (void)state_->ReleaseRuntimeBindings();
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::StorageAllocationFailed));
        }
    }
}  // namespace Horo::Application::Internal
