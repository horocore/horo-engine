#include "UiBindingStoreInternal.h"

namespace Horo::Runtime::Ui {
    using BindingStoreInternal::Failure;

    namespace {

        /** @brief Checks exact snapshot lineage and immutable-provider admission without invoking producer code. */
        [[nodiscard]] Result<void> ValidateLineage(const UiBindingChangeBatch &batch, const UiBindingProviderSchema &schema,
                                                   const UiBindingSnapshotRevision revision) {
            if (batch.schema != schema.Version())
                return Failure(UiErrors::BindingSchemaIncompatible);
            if (!batch.expected.IsValid() || !batch.revision.IsValid())
                return Failure(UiErrors::RevisionInvalid);
            if (batch.expected != revision || batch.revision <= batch.expected)
                return Failure(UiErrors::RevisionStale);
            if (!batch.changes.empty() && HasFlag(schema.Flags(), UiBindingProviderFlags::Immutable))
                return Failure(UiErrors::BindingAccessInvalid);
            return Result<void>::Success();
        }

        /** @brief Validates every borrowed property value and deterministic slot ordering before target work. */
        [[nodiscard]] Result<void> ValidateChanges(const std::span<const UiBindingPropertyUpdate> changes,
                                                   const UiBindingProviderSchema &schema) {
            const auto properties = schema.Properties();
            for (std::size_t index = 0; index < changes.size(); ++index) {
                const auto &change = changes[index];
                if (change.property >= properties.size())
                    return Failure(UiErrors::BindingPropertyUnknown);
                if (index != 0 && changes[index - 1].property >= change.property)
                    return Failure(UiErrors::BindingDescriptorConflict);
                const auto &property = properties[change.property];
                if (const auto valid = BindingInternal::ValidateValue(change.value, property.type, property.limits); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Checks complete transaction ordering and aggregate count/byte budgets before provider validation. */
        [[nodiscard]] Result<void> ValidateEnvelope(const std::span<const UiBindingChangeBatch> batches,
                                                    const UiBindingStoreLimits &limits) {
            if (batches.size() > limits.providers)
                return Failure(UiErrors::BindingCapacityExceeded);
            std::size_t changeCount = 0;
            std::size_t changeBytes = 0;
            for (std::size_t index = 0; index < batches.size(); ++index) {
                const auto &batch = batches[index];
                if (batch.changes.size() > limits.changes - changeCount)
                    return Failure(UiErrors::BindingCapacityExceeded);
                changeCount += batch.changes.size();
                for (const auto &change : batch.changes) {
                    const auto bytes = BindingStoreInternal::ValueBytes(change.value);
                    if (bytes > limits.changeBytes - changeBytes)
                        return Failure(UiErrors::BindingCapacityExceeded);
                    changeBytes += bytes;
                }
                if (index != 0 && !(batches[index - 1].provider < batch.provider))
                    return Failure(UiErrors::BindingDescriptorConflict);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiBindingStore::Storage::ValidateTree */
    Result<void> UiBindingStore::Storage::ValidateTree(const UiElementTree &tree) const {
        if (!active || tree.State() != UiElementTreeState::Active)
            return Failure(UiErrors::BindingLifecycleUnavailable);
        if (tree.Instance() != instance || tree.Canvas() != canvas || tree.SourceDocument() != document)
            return Failure(UiErrors::HandleOwnerMismatch);
        if (tree.SourceDocumentRevision() != documentRevision || tree.Revision() != treeRevision)
            return Failure(UiErrors::RevisionStale);
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::MatchesTree */
    bool UiBindingStore::Storage::MatchesTree(const UiElementTree &tree) const noexcept {
        const bool identity = tree.Instance() == instance && tree.Canvas() == canvas && tree.SourceDocument() == document;
        const bool revision = tree.SourceDocumentRevision() == documentRevision && tree.Revision() == treeRevision;
        return active && tree.State() == UiElementTreeState::Active && identity && revision;
    }

    /** @copydoc UiBindingStore::Storage::ValidateBatches */
    Result<void> UiBindingStore::Storage::ValidateBatches(const std::span<const UiBindingChangeBatch> batches) {
        if (const auto valid = ValidateEnvelope(batches, limits); valid.HasError())
            return valid;
        for (const auto &batch : batches) {
            const auto *provider = FindProvider(batch.provider);
            if (!provider)
                return Failure(UiErrors::HandleStale);
            if (!provider->active)
                return Failure(UiErrors::BindingLifecycleUnavailable);
            if (const auto valid = ValidateLineage(batch, provider->schema, provider->revision); valid.HasError())
                return valid;
            if (const auto valid = ValidateChanges(batch.changes, provider->schema); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::StageBatches */
    Result<void> UiBindingStore::Storage::StageBatches(const std::span<const UiBindingChangeBatch> batches) {
        for (const auto &batch : batches) {
            const auto *provider = FindProvider(batch.provider);
            for (const auto &change : batch.changes)
                if (const auto result = StageProperty(*provider, change); result.HasError())
                    return result;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::StageProperty */
    Result<void> UiBindingStore::Storage::StageProperty(const Provider &provider, const UiBindingPropertyUpdate &change) {
        for (const auto target : provider.targets[change.property]) {
            if (targets[target].direction == UiBindingDirection::TargetToSource)
                continue;
            if (const auto result = Stage(target, &change.value, UiBindingValueOrigin::Provider); result.HasError())
                return result;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::StageUnregister */
    Result<void> UiBindingStore::Storage::StageUnregister(const Provider &provider) {
        for (const auto &subscribers : provider.targets)
            for (const auto index : subscribers) {
                const auto &fallback = targets[index].fallback;
                const auto origin = fallback ? UiBindingValueOrigin::Fallback : UiBindingValueOrigin::Unavailable;
                if (const auto result = Stage(index, fallback ? &*fallback : nullptr, origin); result.HasError())
                    return result;
            }
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::Stage */
    Result<void> UiBindingStore::Storage::Stage(const std::size_t targetIndex, const UiBindingValue *value,
                                                const UiBindingValueOrigin origin) {
        const auto &target = targets[targetIndex];
        if (value) {
            if (const auto valid = BindingInternal::ValidateValue(*value, *UiBindingTargetValueType(target.bound.property), target.limits);
                valid.HasError())
                return valid;
        }
        if (target.bound.origin == origin && (!value || target.bound.value == *value))
            return Result<void>::Success();
        staged.emplace_back(targetIndex, value, origin);
        if (HasFlag(target.categories, UiBindingDirty::Layout))
            invalidations.emplace_back(target.bound.element, treeRevision, UiLayoutDirtyKind::Measure);
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::Publish */
    Result<UiBindingApplyResult> UiBindingStore::Storage::Publish(const UiElementTree &tree, UiLayoutEngine &layout) {
        UiBindingApplyResult result{current.revision, current.content, staged.size(), 0};
        if (!staged.empty()) {
            const auto revision = current.revision.Next();
            if (revision.HasError())
                return Result<UiBindingApplyResult>::Failure(revision.ErrorValue());
            result.revision = revision.Value();
        }
        if (!invalidations.empty()) {
            const auto content = current.content.Next();
            if (content.HasError())
                return Result<UiBindingApplyResult>::Failure(content.ErrorValue());
            result.content = content.Value();
        }
        if (const auto queued = layout.InvalidateBatch(tree, invalidations); queued.HasError())
            return Result<UiBindingApplyResult>::Failure(queued.ErrorValue());
        for (const auto &change : staged) {
            auto &target = targets[change.target];
            if (change.value)
                BindingStoreInternal::CopyValue(target.bound.value, *change.value);
            else
                BindingStoreInternal::ClearValue(target.bound.value);
            target.bound.origin = change.origin;
            target.pending = target.pending | target.categories;
            if (change.origin == UiBindingValueOrigin::Unavailable)
                ++result.requiredUnavailable;
        }
        current = result;
        return Result<UiBindingApplyResult>::Success(result);
    }

    /** @copydoc UiBindingStore::UiBindingStore */
    UiBindingStore::UiBindingStore(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiBindingStore::~UiBindingStore */
    UiBindingStore::~UiBindingStore() = default;
    /** @copydoc UiBindingStore::UiBindingStore */
    UiBindingStore::UiBindingStore(UiBindingStore &&) noexcept = default;
    /** @copydoc UiBindingStore::operator= */
    UiBindingStore &UiBindingStore::operator=(UiBindingStore &&) noexcept = default;

    /** @copydoc UiBindingStore::Apply */
    Result<UiBindingApplyResult> UiBindingStore::Apply(const UiElementTree &tree, const std::span<const UiBindingChangeBatch> batches,
                                                       UiLayoutEngine &layout) {
        if (!storage_ || storage_->processingWrite)
            return Failure<UiBindingApplyResult>(UiErrors::BindingLifecycleUnavailable);
        if (const auto valid = storage_->ValidateTree(tree); valid.HasError())
            return Result<UiBindingApplyResult>::Failure(valid.ErrorValue());
        if (batches.empty())
            return Result<UiBindingApplyResult>::Success({storage_->current.revision, storage_->current.content});
        if (const auto valid = storage_->ValidateBatches(batches); valid.HasError())
            return Result<UiBindingApplyResult>::Failure(valid.ErrorValue());
        const Storage::StagingScope staging{*storage_};
        if (const auto staged = storage_->StageBatches(batches); staged.HasError())
            return Result<UiBindingApplyResult>::Failure(staged.ErrorValue());
        const auto published = storage_->Publish(tree, layout);
        if (published.HasValue())
            for (const auto &batch : batches)
                storage_->FindProvider(batch.provider)->revision = batch.revision;
        return published;
    }

    /** @copydoc UiBindingStore::Unregister */
    Result<UiBindingApplyResult> UiBindingStore::Unregister(const UiElementTree &tree, const UiBindingProviderInstanceId providerId,
                                                            UiLayoutEngine &layout) {
        if (!storage_ || storage_->processingWrite)
            return Failure<UiBindingApplyResult>(UiErrors::BindingLifecycleUnavailable);
        if (const auto valid = storage_->ValidateTree(tree); valid.HasError())
            return Result<UiBindingApplyResult>::Failure(valid.ErrorValue());
        auto *provider = storage_->FindProvider(providerId);
        if (!provider)
            return Failure<UiBindingApplyResult>(UiErrors::HandleStale);
        if (!provider->active)
            return Result<UiBindingApplyResult>::Success({storage_->current.revision, storage_->current.content});
        const Storage::StagingScope staging{*storage_};
        provider->writesRevoked = true;
        for (auto &target : storage_->targets)
            if (target.provider == static_cast<std::size_t>(provider - storage_->providers.data()))
                storage_->CancelWrite(target, UiBindingWriteCancellationReason::ProviderUnavailable);
        if (const auto staged = storage_->StageUnregister(*provider); staged.HasError())
            return Result<UiBindingApplyResult>::Failure(staged.ErrorValue());
        const auto published = storage_->Publish(tree, layout);
        if (published.HasValue())
            provider->active = false;
        return published;
    }

    /** @copydoc UiBindingStore::Find */
    const UiBoundTarget *UiBindingStore::Find(const UiElementTree &tree, const UiBindingId binding) const noexcept {
        if (!storage_ || !storage_->MatchesTree(tree))
            return nullptr;
        const auto found = std::ranges::find_if(storage_->targets, [binding](const Storage::Target &target) {
            return target.bound.binding == binding && target.bound.origin != UiBindingValueOrigin::Unavailable;
        });
        return found != storage_->targets.end() ? &found->bound : nullptr;
    }

    /** @copydoc UiBindingStore::DrainDirty */
    Result<std::size_t> UiBindingStore::DrainDirty(const std::span<UiBindingTargetDirty> output) {
        if (!storage_ || !storage_->active)
            return Failure<std::size_t>(UiErrors::BindingLifecycleUnavailable);
        const auto count = static_cast<std::size_t>(std::ranges::count_if(storage_->targets, [](const Storage::Target &target) {
            return target.pending != UiBindingDirty::None;
        }));
        if (output.size() < count)
            return Failure<std::size_t>(UiErrors::BindingCapacityExceeded);
        std::size_t index = 0;
        for (auto &target : storage_->targets) {
            if (target.pending == UiBindingDirty::None)
                continue;
            output[index++] = {target.bound.binding, target.bound.element,       target.bound.property,
                               target.pending,       storage_->current.revision, storage_->treeRevision};
            target.pending = UiBindingDirty::None;
        }
        return Result<std::size_t>::Success(count);
    }

    /** @copydoc UiBindingStore::Current */
    UiBindingApplyResult UiBindingStore::Current() const noexcept {
        return storage_ ? storage_->current : UiBindingApplyResult{};
    }

    /** @copydoc UiBindingStore::BeginRetirement */
    void UiBindingStore::BeginRetirement() noexcept {
        if (storage_ && storage_->processingWrite) {
            storage_->reentryAttempted = true;
            return;
        }
        if (storage_) {
            storage_->active = false;
            for (auto &target : storage_->targets)
                storage_->CancelWrite(target, UiBindingWriteCancellationReason::OwnerRetired);
        }
    }

    /** @copydoc UiBindingStore::Shutdown */
    void UiBindingStore::Shutdown() noexcept {
        if (storage_ && storage_->processingWrite) {
            storage_->reentryAttempted = true;
            return;
        }
        if (storage_) {
            storage_->active = false;
            for (auto &target : storage_->targets)
                storage_->CancelWrite(target, UiBindingWriteCancellationReason::Shutdown);
        }
        storage_.reset();
    }
}  // namespace Horo::Runtime::Ui
