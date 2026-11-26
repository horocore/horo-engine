#include "SaveSlotLifecycleInternal.h"

#include <limits>
#include <new>

namespace Horo::Runtime {
    namespace {
        using namespace SaveSlotLifecycleDetail;

        /** @brief Admits only the explicitly composed local cloud coordinator. */
        bool CloudAllowed(const SaveSlotLifecyclePolicy &policy) noexcept {
            return (policy.capabilities & (1U << static_cast<unsigned>(SaveSlotLifecycleKind::CloudSync))) != 0;
        }

        /** @brief Recovers the visible pair or derives the baseline for a pre-cloud catalog. */
        Result<SaveCloudRevisionSnapshot> ReadPair(const Catalog &catalog, const SaveCloudMetadataScope &scope) {
            using Response = Result<SaveCloudRevisionSnapshot>;
            auto index = CatalogIndex(catalog);
            if (catalog.cloud) {
                if (catalog.cloud->scope != scope)
                    return Response::Failure(MakeError(SaveErrors::CloudMetadataStale));
                return SaveCloudRevisionSnapshot::Create(std::move(index), scope, *catalog.cloud);
            }
            auto metadata = ReconcileSaveCloudRevisionMetadata(index, scope, {}, 1);
            if (metadata.HasError())
                return Response::Failure(metadata.ErrorValue());
            return SaveCloudRevisionSnapshot::Create(std::move(index), scope, std::move(metadata).Value());
        }

        /** @brief Compares the complete catalog consent instead of trusting a revision number alone. */
        bool SameIndex(const SaveSlotIndex &left, const SaveSlotIndex &right) {
            return left.schemaVersion == right.schemaVersion && left.revision == right.revision && left.entries == right.entries;
        }

        /** @brief Validates the caller's exact prior pair and one in-place cloud revision step. */
        bool CanPublish(const SaveCloudRevisionSnapshot &current, const SaveCloudRevisionSnapshot &expected,
                        const SaveCloudRevisionSnapshot &candidate) {
            if (!SameIndex(current.Index(), expected.Index()) || current.Metadata() != expected.Metadata() ||
                !SameIndex(current.Index(), candidate.Index()) || candidate.Metadata().scope != current.Metadata().scope)
                return false;
            if (candidate.Metadata() == current.Metadata())
                return true;
            return current.Metadata().revision < std::numeric_limits<std::uint64_t>::max() &&
                   candidate.Metadata().revision == current.Metadata().revision + 1;
        }
    }

    /** @copydoc SaveSlotLifecycle::ReadCloudRevision */
    Result<SaveCloudRevisionSnapshot> SaveSlotLifecycle::ReadCloudRevision(const SaveNamespaceAccessRequest &access,
                                                                         const SaveCloudMetadataScope &scope) const {
        using Response = Result<SaveCloudRevisionSnapshot>;
        if (!state_ || !CloudAllowed(state_->policy))
            return Response::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        if (scope.localNamespace != access.expected || !scope.IsValid())
            return Response::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        try {
            Operation operation{*state_};
            if (auto leased = BeginRetention(operation, access); leased.HasError())
                return Response::Failure(leased.ErrorValue());
            return ReadPair(operation.catalog, scope);
        } catch (const std::bad_alloc &) {
            return Response::Failure(MakeError(SaveErrors::CloudMetadataAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::PublishCloudRevision */
    Result<void> SaveSlotLifecycle::PublishCloudRevision(const SaveNamespaceAccessRequest &access,
                                                        const SaveCloudRevisionSnapshot &expected,
                                                        const SaveCloudRevisionSnapshot &candidate) {
        if (!state_ || !CloudAllowed(state_->policy))
            return Result<void>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        const auto &scope = expected.Metadata().scope;
        if (scope.localNamespace != access.expected)
            return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        try {
            Operation operation{*state_};
            if (auto leased = BeginRetention(operation, access); leased.HasError())
                return leased;
            auto current = ReadPair(operation.catalog, scope);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (!CanPublish(current.Value(), expected, candidate))
                return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataStale));
            if (candidate.Metadata() == current.Value().Metadata()) {
                // A previous replacement may be visible after its directory sync failed.
                return operation.catalog.cloud ? state_->storage.SynchronizeLifecycleCatalog() : Result<void>::Success();
            }
            operation.catalog.cloud = candidate.Metadata();
            return Publish(operation);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataAllocationFailed));
        }
    }
}
