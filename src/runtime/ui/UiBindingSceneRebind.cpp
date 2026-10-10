#include "UiBindingStoreInternal.h"

namespace Horo::Runtime::Ui {
    using BindingStoreInternal::Failure;

    namespace {
        /** @brief Compares complete copied provider schema contracts without invoking the provider. */
        [[nodiscard]] bool SameSchema(const UiBindingProviderSchema &before, const UiBindingProviderSchema &after) noexcept {
            return before.Type() == after.Type() && before.Version() == after.Version() &&
                   std::ranges::equal(before.Properties(), after.Properties());
        }
    }  // namespace

    /** @copydoc UiBindingStore::HasSceneProviders */
    bool UiBindingStore::HasSceneProviders() const noexcept {
        return storage_ && std::ranges::any_of(storage_->providers, [](const Storage::Provider &provider) {
            return provider.scope == UiBindingProviderScopeKind::Scene;
        });
    }

    /** @copydoc UiBindingStore::Storage::FindProvider */
    const UiBindingStore::Storage::Provider *UiBindingStore::Storage::FindProvider(const UiBindingProviderInstanceId id) const noexcept {
        const auto found = std::ranges::lower_bound(providers, id, {}, &Provider::instance);
        return found != providers.end() && found->instance == id ? std::to_address(found) : nullptr;
    }

    /** @copydoc UiBindingStore::Storage::ValidatePersistentProvider */
    Result<void> UiBindingStore::Storage::ValidatePersistentProvider(const Provider &before) const {
        const auto *after = FindProvider(before.instance);
        if (!after || after->scope != before.scope || after->active != before.active || after->revision != before.revision ||
            after->writesRevoked != before.writesRevoked || !SameSchema(after->schema, before.schema))
            return Failure(UiErrors::BindingDescriptorConflict);
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::ValidateSceneProvider */
    Result<void> UiBindingStore::Storage::ValidateSceneProvider(const Provider &after, const Storage &source, const bool removing) const {
        const auto previous = std::ranges::find_if(source.providers, [&](const Provider &before) {
            return before.scope == UiBindingProviderScopeKind::Scene && SameSchema(before.schema, after.schema);
        });
        if (previous == source.providers.end())
            return Failure(UiErrors::BindingSchemaInvalid);
        const auto *before = source.FindProvider(after.instance);
        if (removing) {
            if (after.active || !before || before->scope != UiBindingProviderScopeKind::Scene)
                return Failure(UiErrors::BindingLifecycleUnavailable);
        } else if (!after.active || before) {
            return Failure(UiErrors::HandleStale);
        }
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::ValidateTargetContract */
    Result<void> UiBindingStore::Storage::ValidateTargetContract(const Target &old, const Target &next, const UiElementTree &sourceTree,
                                                                 const UiElementTree &tree) const {
        const auto oldElement = sourceTree.Get(old.bound.element);
        const auto newElement = tree.Get(next.bound.element);
        if (oldElement.HasError() || newElement.HasError() || oldElement.Value().id != newElement.Value().id ||
            old.bound.property != next.bound.property || old.direction != next.direction || old.property != next.property ||
            old.limits != next.limits || old.fallback != next.fallback)
            return Failure(UiErrors::BindingDescriptorConflict);
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::ValidatePersistentTarget */
    Result<void> UiBindingStore::Storage::ValidatePersistentTarget(const Target &old, const Target &next) const {
        if (old.bound.value != next.bound.value || old.bound.origin != next.bound.origin ||
            old.admission.has_value() != next.admission.has_value())
            return Failure(UiErrors::BindingDescriptorConflict);
        if (old.admission && (old.fence != next.fence || old.admission->authority != next.admission->authority ||
                              old.admission->action != next.admission->action || old.admission->trigger != next.admission->trigger ||
                              old.admission->conflict != next.admission->conflict))
            return Failure(UiErrors::BindingAccessInvalid);
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::ValidateSceneTarget */
    Result<void> UiBindingStore::Storage::ValidateSceneTarget(const Target &old, const Target &next, const Storage &source,
                                                              const UiElementTree &sourceTree, const UiElementTree &tree,
                                                              const bool removing) const {
        if (const auto valid = ValidateTargetContract(old, next, sourceTree, tree); valid.HasError())
            return valid;
        const auto &before = source.providers[old.provider];
        const auto &after = providers[next.provider];
        if (before.scope != after.scope || !SameSchema(before.schema, after.schema))
            return Failure(UiErrors::BindingSchemaInvalid);
        if (before.scope == UiBindingProviderScopeKind::Scene) {
            if (removing &&
                (after.active || next.bound.origin == UiBindingValueOrigin::Provider || (next.admission && !after.writesRevoked)))
                return Failure(UiErrors::BindingLifecycleUnavailable);
            return Result<void>::Success();
        }
        if (before.instance != after.instance)
            return Failure(UiErrors::BindingDescriptorConflict);
        return ValidatePersistentTarget(old, next);
    }

    /** @copydoc UiBindingStore::ValidateSceneRebind */
    Result<void> UiBindingStore::ValidateSceneRebind(const UiBindingStore &source, const UiElementTree &sourceTree,
                                                     const UiElementTree &tree, const bool removing) const {
        if (const auto valid = source.ValidateOwner(sourceTree); valid.HasError())
            return valid;
        if (const auto valid = ValidateOwner(tree); valid.HasError())
            return valid;
        if (storage_->targets.size() != source.storage_->targets.size() || storage_->providers.size() != source.storage_->providers.size())
            return Failure(UiErrors::BindingDescriptorConflict);
        for (const auto &before : source.storage_->providers)
            if (before.scope != UiBindingProviderScopeKind::Scene)
                if (const auto valid = storage_->ValidatePersistentProvider(before); valid.HasError())
                    return valid;
        for (const auto &after : storage_->providers)
            if (after.scope == UiBindingProviderScopeKind::Scene)
                if (const auto valid = storage_->ValidateSceneProvider(after, *source.storage_, removing); valid.HasError())
                    return valid;
        for (const auto &old : source.storage_->targets) {
            const auto next = std::ranges::find(storage_->targets, old.bound.binding, [](const Storage::Target &target) {
                return target.bound.binding;
            });
            if (next == storage_->targets.end())
                return Failure(UiErrors::BindingDescriptorConflict);
            if (const auto valid = storage_->ValidateSceneTarget(old, *next, *source.storage_, sourceTree, tree, removing);
                valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
