#include "UiBindingStoreInternal.h"

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

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

        /** @brief Counts variable-sized input before validation can scan text or identifiers. */
        [[nodiscard]] std::size_t ValueBytes(const UiBindingValue &value) noexcept {
            if (const auto *text = std::get_if<std::string>(&value))
                return text->size();
            if (const auto *message = std::get_if<UiBindingLocalizedMessage>(&value))
                return message->key.size();
            if (const auto *reference = std::get_if<UiBindingReference>(&value))
                return reference->value.size();
            return 0;
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

    /** @copydoc UiBindingStore::Storage::ValidateBatches */
    Result<void> UiBindingStore::Storage::ValidateBatches(const std::span<const UiBindingChangeBatch> batches) {
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
                const auto bytes = ValueBytes(change.value);
                if (bytes > limits.changeBytes - changeBytes)
                    return Failure(UiErrors::BindingCapacityExceeded);
                changeBytes += bytes;
            }
            if (index != 0 && !(batches[index - 1].provider < batch.provider))
                return Failure(UiErrors::BindingDescriptorConflict);
        }
        for (const auto &batch : batches) {
            auto *provider = FindProvider(batch.provider);
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

    /** @copydoc UiBindingStore::Storage::Stage */
    Result<void> UiBindingStore::Storage::Stage(const std::size_t targetIndex, const UiBindingValue *value,
                                                const UiBindingValueOrigin origin) {
        auto &target = targets[targetIndex];
        if (value) {
            if (const auto valid = BindingInternal::ValidateValue(*value, *UiBindingTargetValueType(target.bound.property), target.limits);
                valid.HasError())
                return valid;
        }
        if (target.bound.origin == origin && (!value || target.bound.value == *value))
            return Result<void>::Success();
        staged.push_back({targetIndex, value, origin});
        if (HasFlag(target.categories, UiBindingDirty::Layout))
            invalidations.push_back({target.bound.element, treeRevision, UiLayoutDirtyKind::Measure});
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
        if (!storage_)
            return Failure<UiBindingApplyResult>(UiErrors::BindingLifecycleUnavailable);
        if (const auto valid = storage_->ValidateTree(tree); valid.HasError())
            return Result<UiBindingApplyResult>::Failure(valid.ErrorValue());
        if (batches.empty())
            return Result<UiBindingApplyResult>::Success({storage_->current.revision, storage_->current.content});
        if (const auto valid = storage_->ValidateBatches(batches); valid.HasError())
            return Result<UiBindingApplyResult>::Failure(valid.ErrorValue());
        const Storage::StagingScope staging{*storage_};
        for (const auto &batch : batches) {
            const auto *provider = storage_->FindProvider(batch.provider);
            for (const auto &change : batch.changes)
                for (const auto target : provider->targets[change.property])
                    if (const auto staged = storage_->Stage(target, &change.value, UiBindingValueOrigin::Provider); staged.HasError())
                        return Result<UiBindingApplyResult>::Failure(staged.ErrorValue());
        }
        const auto published = storage_->Publish(tree, layout);
        if (published.HasValue())
            for (const auto &batch : batches)
                storage_->FindProvider(batch.provider)->revision = batch.revision;
        return published;
    }

    /** @copydoc UiBindingStore::Unregister */
    Result<UiBindingApplyResult> UiBindingStore::Unregister(const UiElementTree &tree, const UiBindingProviderInstanceId providerId,
                                                            UiLayoutEngine &layout) {
        if (!storage_)
            return Failure<UiBindingApplyResult>(UiErrors::BindingLifecycleUnavailable);
        if (const auto valid = storage_->ValidateTree(tree); valid.HasError())
            return Result<UiBindingApplyResult>::Failure(valid.ErrorValue());
        auto *provider = storage_->FindProvider(providerId);
        if (!provider)
            return Failure<UiBindingApplyResult>(UiErrors::HandleStale);
        if (!provider->active)
            return Result<UiBindingApplyResult>::Success({storage_->current.revision, storage_->current.content});
        const Storage::StagingScope staging{*storage_};
        for (const auto &subscribers : provider->targets)
            for (const auto index : subscribers) {
                const auto &fallback = storage_->targets[index].fallback;
                if (const auto staged = storage_->Stage(index, fallback ? &*fallback : nullptr,
                                                        fallback ? UiBindingValueOrigin::Fallback : UiBindingValueOrigin::Unavailable);
                    staged.HasError())
                    return Result<UiBindingApplyResult>::Failure(staged.ErrorValue());
            }
        const auto published = storage_->Publish(tree, layout);
        if (published.HasValue())
            provider->active = false;
        return published;
    }

    /** @copydoc UiBindingStore::Find */
    const UiBoundTarget *UiBindingStore::Find(const UiElementTree &tree, const UiBindingId binding) const noexcept {
        if (!storage_ || !storage_->active || tree.State() != UiElementTreeState::Active || tree.Instance() != storage_->instance ||
            tree.Canvas() != storage_->canvas || tree.SourceDocument() != storage_->document ||
            tree.SourceDocumentRevision() != storage_->documentRevision || tree.Revision() != storage_->treeRevision)
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
        if (storage_)
            storage_->active = false;
    }

    /** @copydoc UiBindingStore::Shutdown */
    void UiBindingStore::Shutdown() noexcept {
        storage_.reset();
    }
}  // namespace Horo::Runtime::Ui
