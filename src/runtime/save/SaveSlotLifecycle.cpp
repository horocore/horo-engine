#include "SaveSlotLifecycleInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Runtime {
    using namespace SaveSlotLifecycleDetail;

    namespace {
        /** @brief Preserves independent capability authorization before byte acquisition or storage mutation. */
        [[nodiscard]] Result<void> Authorize(const SaveSlotLifecycleRequest &request, const SaveSlotLifecyclePolicy &policy,
                                             const ISaveSlotLifecycleHost &host) {
            using enum SaveSlotLifecycleKind;
            const auto kind = static_cast<std::uint8_t>(request.kind);
            if (kind >= static_cast<std::uint8_t>(Count))
                return Result<void>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
            if ((policy.capabilities & (std::uint16_t{1} << kind)) == 0)
                return Result<void>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
            if (request.destination.has_value() != (request.kind == Copy) || request.importSource.has_value() != (request.kind == Import) ||
                static_cast<bool>(request.imported.bytes) != (request.kind == Import) ||
                request.deleteMode > SaveSlotDeleteMode::PlatformRecycle ||
                (request.kind != Delete && request.deleteMode != SaveSlotDeleteMode::Soft))
                return Result<void>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
            if (request.kind == Delete) {
                if (request.deleteMode == SaveSlotDeleteMode::Permanent && !policy.allowPermanentDelete)
                    return Result<void>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
                if (request.deleteMode == SaveSlotDeleteMode::PlatformRecycle && (!policy.allowPlatformRecycle || !host.SupportsRecycle()))
                    return Result<void>::Failure(MakeError(SaveErrors::StorageCapabilityUnsupported));
            }
            if (request.kind == Import && (*request.importSource >= policy.importSources.size() || request.imported.bytes->empty() ||
                                           request.imported.bytes->size() > policy.archiveLimits.maximumArchiveBytes))
                return Result<void>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
            if (request.kind == Copy || request.kind == Import || request.kind == Rename)
                return ValidateSaveSlotDisplayMetadata(request.display);
            if (!request.display.displayName.empty() || !request.display.summary.empty())
                return Result<void>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
            return Result<void>::Success();
        }

        /** @brief Binds optimistic consent to exact namespace, index revision and selected generation. */
        [[nodiscard]] Result<void> CheckTarget(const SaveSlotLifecycleTarget &target, const Catalog &catalog,
                                               const SaveSlotLifecyclePolicy &policy) {
            if (target.address.namespaceAccess.expected != policy.destination.name ||
                target.address.namespaceAccess.expectedRevision == 0 || !target.address.slot.IsValid() || target.catalogRevision == 0 ||
                (target.generation && !target.generation->IsValid()))
                return Result<void>::Failure(MakeError(SaveErrors::NamespaceStale));
            const auto selected = std::ranges::find(catalog.records, target.address.slot, [](const Record &record) {
                return record.entry.publication.slot;
            });
            if (const auto generation =
                    selected == catalog.records.end() ? std::optional<SlotGenerationId>{} : selected->entry.publication.generation;
                target.catalogRevision != catalog.revision || target.generation != generation)
                return Result<void>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
            return Result<void>::Success();
        }

    }  // namespace

    SaveSlotLifecycle::SaveSlotLifecycle(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    SaveSlotLifecycle::SaveSlotLifecycle(SaveSlotLifecycle &&) noexcept = default;
    SaveSlotLifecycle &SaveSlotLifecycle::operator=(SaveSlotLifecycle &&) noexcept = default;
    SaveSlotLifecycle::~SaveSlotLifecycle() = default;

    /** @copydoc SaveSlotLifecycle::Open */
    Result<SaveSlotLifecycle> SaveSlotLifecycle::Open(const ProductSaveRoot &root, SaveSlotLifecyclePolicy policy,
                                                      ISaveSlotLifecycleHost &host, ISaveSlotLifecycleIoObserver *observer) {
        try {
            if (auto valid = ValidatePolicy(policy); valid.HasError())
                return Result<SaveSlotLifecycle>::Failure(valid.ErrorValue());
            auto files = SaveFilesystemStorage::Open(root, policy.destination.name);
            if (files.HasError())
                return Result<SaveSlotLifecycle>::Failure(files.ErrorValue());
            auto storage = std::move(files).Value();
            storage.SetLifecycleIoObserver(observer);
            SaveSlotLifecycle owner{std::make_unique<State>(std::move(storage), std::move(policy), host)};
            {
                Operation operation{*owner.state_};
                if (auto loaded = owner.Load(operation); loaded.HasError())
                    return Result<SaveSlotLifecycle>::Failure(loaded.ErrorValue());
            }
            return Result<SaveSlotLifecycle>::Success(std::move(owner));
        } catch (const std::bad_alloc &) {
            return Result<SaveSlotLifecycle>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::Load */
    Result<void> SaveSlotLifecycle::Load(Operation &operation) const {
        auto bytes = state_->storage.ReadLifecycleCatalog(state_->policy.maximumCatalogBytes);
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        if (!bytes.Value()) {
            operation.catalog = {};
            return Result<void>::Success();
        }
        auto catalog = DecodeCatalog(*bytes.Value(), state_->policy);
        if (catalog.HasError())
            return Result<void>::Failure(catalog.ErrorValue());
        operation.catalog = std::move(catalog).Value();
        return Result<void>::Success();
    }

    /** @copydoc SaveSlotLifecycle::Publish */
    Result<void> SaveSlotLifecycle::Publish(const Operation &operation) const {
        auto bytes = EncodeCatalog(operation.catalog, state_->policy);
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        if (auto published = state_->storage.ReplaceLifecycleCatalog(bytes.Value()); published.HasError())
            return published;
        return Result<void>::Success();
    }

    /** @copydoc SaveSlotLifecycle::List */
    Result<SaveSlotIndex> SaveSlotLifecycle::List(const SaveNamespaceAccessRequest &access) const {
        return ListSelected(access, false);
    }

    /** @copydoc SaveSlotLifecycle::ListDeleted */
    Result<SaveSlotIndex> SaveSlotLifecycle::ListDeleted(const SaveNamespaceAccessRequest &access) const {
        return ListSelected(access, true);
    }

    /** @copydoc SaveSlotLifecycle::ListSelected */
    Result<SaveSlotIndex> SaveSlotLifecycle::ListSelected(const SaveNamespaceAccessRequest &access, const bool deleted) const {
        if (!state_)
            return Result<SaveSlotIndex>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
        try {
            Operation operation{*state_};
            if (access.expected != state_->policy.destination.name)
                return Result<SaveSlotIndex>::Failure(MakeError(SaveErrors::NamespaceStale));
            auto lease = state_->host->AcquireBinding(access);
            if (lease.HasError())
                return Result<SaveSlotIndex>::Failure(lease.ErrorValue());
            if (!lease.Value())
                return Result<SaveSlotIndex>::Failure(MakeError(SaveErrors::StorageResultInvalid));
            if (auto loaded = Load(operation); loaded.HasError())
                return Result<SaveSlotIndex>::Failure(loaded.ErrorValue());
            SaveSlotIndex index{.revision = operation.catalog.revision};
            index.entries.reserve(operation.catalog.records.size());
            for (const auto &record : operation.catalog.records) {
                if (record.deleted == deleted)
                    index.entries.push_back(record.entry);
            }
            return Result<SaveSlotIndex>::Success(std::move(index));
        } catch (const std::bad_alloc &) {
            return Result<SaveSlotIndex>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::Reconcile */
    Result<bool> SaveSlotLifecycle::Reconcile(const SaveNamespaceAccessRequest &access) {
        if (!state_)
            return Result<bool>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
        try {
            Operation operation{*state_};
            if (access.expected != state_->policy.destination.name)
                return Result<bool>::Failure(MakeError(SaveErrors::NamespaceStale));
            auto lease = state_->host->AcquireBinding(access);
            if (lease.HasError())
                return Result<bool>::Failure(lease.ErrorValue());
            if (!lease.Value())
                return Result<bool>::Failure(MakeError(SaveErrors::StorageResultInvalid));
            if (auto loaded = Load(operation); loaded.HasError())
                return Result<bool>::Failure(loaded.ErrorValue());
            return Cleanup(operation);
        } catch (const std::bad_alloc &) {
            return Result<bool>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::Execute */
    Result<SaveSlotLifecycleResult> SaveSlotLifecycle::Execute(SaveSlotLifecycleRequest request, const CancellationToken &cancellation) {
        if (!state_)
            return Result<SaveSlotLifecycleResult>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
        try {
            if (auto authorized = Authorize(request, state_->policy, *state_->host); authorized.HasError())
                return Result<SaveSlotLifecycleResult>::Failure(authorized.ErrorValue());
            if (auto cancelled = CheckCancellation(cancellation); cancelled.HasError())
                return Result<SaveSlotLifecycleResult>::Failure(cancelled.ErrorValue());
            Operation operation{*state_};
            auto lease = state_->host->AcquireBinding(request.source.address.namespaceAccess);
            if (lease.HasError())
                return Result<SaveSlotLifecycleResult>::Failure(lease.ErrorValue());
            if (!lease.Value())
                return Result<SaveSlotLifecycleResult>::Failure(MakeError(SaveErrors::StorageResultInvalid));
            if (auto loaded = Load(operation); loaded.HasError())
                return Result<SaveSlotLifecycleResult>::Failure(loaded.ErrorValue());
            if (auto checked = CheckTarget(request.source, operation.catalog, state_->policy); checked.HasError())
                return Result<SaveSlotLifecycleResult>::Failure(checked.ErrorValue());
            if (request.destination) {
                if (request.destination->address.namespaceAccess.expectedRevision !=
                        request.source.address.namespaceAccess.expectedRevision ||
                    request.destination->address.slot == request.source.address.slot)
                    return Result<SaveSlotLifecycleResult>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
                if (auto checked = CheckTarget(*request.destination, operation.catalog, state_->policy); checked.HasError())
                    return Result<SaveSlotLifecycleResult>::Failure(checked.ErrorValue());
            }
            // Read-only export never retires artifacts. A pending journal instead blocks further
            // mutation until exact evidence can be reconciled under this same ownership domain.
            if (request.kind != SaveSlotLifecycleKind::Export) {
                auto cleaned = Cleanup(operation);
                if (cleaned.HasError())
                    return Result<SaveSlotLifecycleResult>::Failure(cleaned.ErrorValue());
                if (cleaned.Value())
                    return Result<SaveSlotLifecycleResult>::Failure(MakeError(SaveErrors::OperationInProgress));
            }
            return ExecuteLocked(operation, request, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<SaveSlotLifecycleResult>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::ExportTo */
    Result<SaveSlotLifecycleResult> SaveSlotLifecycle::ExportTo(SaveSlotLifecycleRequest request, const SaveFilesystemStorage &destination,
                                                                const SaveGameSlotId slot, const CancellationToken &cancellation) {
        if (request.kind != SaveSlotLifecycleKind::Export || !slot.IsValid())
            return Result<SaveSlotLifecycleResult>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
        auto exported = Execute(std::move(request), cancellation);
        if (exported.HasError())
            return exported;
        if (auto cancelled = CheckCancellation(cancellation); cancelled.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(cancelled.ErrorValue());
        if (auto published = destination.ReplaceLifecycleExport(slot, *exported.Value().exported.bytes); published.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(std::move(published).ErrorValue());
        return exported;
    }
}  // namespace Horo::Runtime
