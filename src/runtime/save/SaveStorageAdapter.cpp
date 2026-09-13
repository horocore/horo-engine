#include "Horo/Runtime/Save/SaveStorageAdapter.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <mutex>
#include <new>
#include <utility>

namespace Horo::Runtime {
    struct SaveStorageDetail::SharedOperation final {
        mutable std::mutex mutex;
        SaveOperationHandle operation;
        std::optional<SaveStorageValue> value;
        CancellationSource cancellation;
        JobSystem *jobs{};
        JobId job{};
    };

    namespace {
        [[nodiscard]] bool IsKnown(const SaveStorageOperationKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(SaveStorageOperationKind::Delete);
        }

        [[nodiscard]] bool IsMutation(const SaveStorageOperationKind kind) noexcept {
            return kind == SaveStorageOperationKind::Write || kind == SaveStorageOperationKind::Copy ||
                   kind == SaveStorageOperationKind::Rename || kind == SaveStorageOperationKind::Delete;
        }

        [[nodiscard]] SaveOperationKind LifecycleKind(const SaveStorageOperationKind kind) noexcept {
            if (kind == SaveStorageOperationKind::Delete)
                return SaveOperationKind::Delete;
            if (IsMutation(kind))
                return SaveOperationKind::Save;
            return SaveOperationKind::RefreshCatalog;
        }

        [[nodiscard]] bool ValidAddress(const SaveStorageAddress &address) noexcept {
            return address.namespaceAccess.expected.IsValid() && address.namespaceAccess.expectedRevision != 0 && address.slot.IsValid();
        }

        [[nodiscard]] bool SameAddress(const SaveStorageAddress &left, const SaveStorageAddress &right) noexcept {
            return left.namespaceAccess.expected == right.namespaceAccess.expected &&
                   left.namespaceAccess.expectedRevision == right.namespaceAccess.expectedRevision && left.slot == right.slot;
        }

        [[nodiscard]] bool ValidLimits(const SaveStorageLimits &limits) noexcept {
            return limits.maximumArchiveBytes != 0 && limits.maximumListedSlots != 0;
        }

        [[nodiscard]] bool ValidDestination(const SaveStorageRequest &request) noexcept {
            const bool needsDestination =
                request.kind == SaveStorageOperationKind::Copy || request.kind == SaveStorageOperationKind::Rename;
            return request.destination.has_value() == needsDestination &&
                   (!request.destination || (ValidAddress(*request.destination) && !SameAddress(*request.destination, request.source)));
        }

        [[nodiscard]] bool ValidWrite(const SaveStorageRequest &request, const SaveStorageLimits &limits) noexcept {
            if (request.write.has_value() != (request.kind == SaveStorageOperationKind::Write))
                return false;
            if (!request.write)
                return true;
            const SaveStorageWrite &write = *request.write;
            return write.archive.bytes && !write.archive.bytes->empty() && write.archive.bytes->size() <= limits.maximumArchiveBytes &&
                   write.metadata.publication.slot == request.source.slot &&
                   ValidateSaveSlotPublicationMetadata(write.metadata.publication).HasValue() &&
                   ValidateSaveSlotDisplayMetadata(write.metadata.display).HasValue();
        }

        [[nodiscard]] bool ValidRequest(const SaveStorageRequest &request, const SaveStorageLimits &limits) noexcept {
            return IsKnown(request.kind) && ValidAddress(request.source) && ValidDestination(request) && ValidWrite(request, limits);
        }

        [[nodiscard]] bool EntryLess(const SaveSlotCatalogEntry &left, const SaveSlotCatalogEntry &right) noexcept {
            return left.publication.slot < right.publication.slot;
        }

        [[nodiscard]] bool ValidListValue(const SaveStorageValue &value, const SaveStorageLimits &limits) noexcept {
            const auto *list = std::get_if<ImmutableSaveSlotList>(&value);
            if (!list || !list->entries || list->entries->size() > limits.maximumListedSlots ||
                !std::is_sorted(list->entries->begin(), list->entries->end(), EntryLess))
                return false;
            for (std::size_t index = 0; index < list->entries->size(); ++index) {
                const auto &entry = (*list->entries)[index];
                if (ValidateSaveSlotPublicationMetadata(entry.publication).HasError() ||
                    ValidateSaveSlotDisplayMetadata(entry.display).HasError() ||
                    (index != 0 && (*list->entries)[index - 1].publication.slot == entry.publication.slot))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool ValidMetadataValue(const SaveStorageRequest &request, const SaveStorageValue &value) noexcept {
            const auto *metadata = std::get_if<ImmutableSaveSlotMetadata>(&value);
            return metadata && metadata->entry && metadata->entry->publication.slot == request.source.slot &&
                   ValidateSaveSlotPublicationMetadata(metadata->entry->publication).HasValue() &&
                   ValidateSaveSlotDisplayMetadata(metadata->entry->display).HasValue();
        }

        [[nodiscard]] bool ValidArchiveValue(const SaveStorageValue &value, const SaveStorageLimits &limits) noexcept {
            const auto *archive = std::get_if<ImmutableSaveArchive>(&value);
            return archive && archive->bytes && !archive->bytes->empty() && archive->bytes->size() <= limits.maximumArchiveBytes;
        }

        [[nodiscard]] bool ValidValue(const SaveStorageRequest &request, const SaveStorageValue &value,
                                      const SaveStorageLimits &limits) noexcept {
            switch (request.kind) {
                case SaveStorageOperationKind::List:
                    return ValidListValue(value, limits);
                case SaveStorageOperationKind::ReadMetadata:
                    return ValidMetadataValue(request, value);
                case SaveStorageOperationKind::ReadArchive:
                    return ValidArchiveValue(value, limits);
                case SaveStorageOperationKind::Exists:
                    return std::holds_alternative<bool>(value);
                case SaveStorageOperationKind::Write:
                case SaveStorageOperationKind::Copy:
                case SaveStorageOperationKind::Rename:
                case SaveStorageOperationKind::Delete:
                    return std::holds_alternative<std::monostate>(value);
            }
            return false;
        }

        void Fail(SaveOperationController &controller, Error error, const bool mutation) {
            const auto outcome = mutation ? SaveOperationCommitOutcome::Unknown : SaveOperationCommitOutcome::NotCommitted;
            (void)controller.Fail(std::move(error), outcome);
        }

        [[nodiscard]] Result<SaveStorageValue> ExecuteProvider(ISaveStorageProvider &provider, const SaveStorageRequest &request,
                                                               const CancellationToken &cancellation) {
            try {
                return provider.Execute(request, cancellation);
            } catch (const std::bad_alloc &) {
                return Result<SaveStorageValue>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
            } catch (...) {
                return Result<SaveStorageValue>::Failure(MakeError(SaveErrors::StorageResultInvalid));
            }
        }

        [[nodiscard]] Result<void> Execute(const std::shared_ptr<SaveStorageDetail::SharedOperation> &state,
                                           const std::shared_ptr<ISaveStorageProvider> &provider, SaveStorageRequest request,
                                           const SaveStorageLimits limits, const std::shared_ptr<SaveOperationController> &controller,
                                           const CancellationToken &cancellation) {
            const bool mutation = IsMutation(request.kind);
            const SaveOperationStage stage = request.kind == SaveStorageOperationKind::Delete ? SaveOperationStage::Deleting
                                             : mutation                                       ? SaveOperationStage::WritingTemporary
                                                                                              : SaveOperationStage::RefreshingCatalog;
            if (controller->PublishProgress(stage, {1, 1}) != SaveOperationTransitionResult::Applied)
                return Result<void>::Success();
            if (mutation && controller->BeginCommit() != SaveCommitGateResult::Entered)
                return Result<void>::Success();
            const CancellationToken providerCancellation = mutation ? CancellationToken{} : cancellation;
            Result<SaveStorageValue> executed = ExecuteProvider(*provider, request, providerCancellation);
            if (executed.HasError()) {
                Fail(*controller, executed.ErrorValue(), mutation);
                return Result<void>::Success();
            }
            if (!ValidValue(request, executed.Value(), limits)) {
                Fail(*controller, MakeError(SaveErrors::StorageResultInvalid), mutation);
                return Result<void>::Success();
            }
            {
                std::lock_guard lock(state->mutex);
                state->value.emplace(std::move(executed).Value());
            }
            (void)controller->Complete(mutation ? SaveOperationCommitOutcome::Committed : SaveOperationCommitOutcome::NotCommitted);
            return Result<void>::Success();
        }
    }  // namespace

    bool SaveStorageCapabilities::Supports(const SaveStorageOperationKind kind) const noexcept {
        if (!IsKnown(kind))
            return false;
        return (bits & (std::uint16_t{1} << static_cast<std::uint8_t>(kind))) != 0;
    }

    SaveStorageOperation::SaveStorageOperation(std::shared_ptr<SaveStorageDetail::SharedOperation> state) noexcept
        : state_(std::move(state)) {}

    bool SaveStorageOperation::IsValid() const noexcept {
        return static_cast<bool>(state_);
    }

    std::optional<SaveOperationSnapshot> SaveStorageOperation::Snapshot() const {
        return state_ ? state_->operation.Snapshot() : std::nullopt;
    }

    std::optional<SaveStorageValue> SaveStorageOperation::Value() const {
        if (!state_)
            return std::nullopt;
        const auto snapshot = state_->operation.Snapshot();
        if (!snapshot || snapshot->state != SaveOperationState::Completed)
            return std::nullopt;
        std::lock_guard lock(state_->mutex);
        return state_->value;
    }

    SaveCancellationRequestResult SaveStorageOperation::RequestCancellation() const noexcept {
        if (!state_)
            return SaveCancellationRequestResult::InvalidHandle;
        const SaveCancellationRequestResult disposition = state_->operation.RequestCancellation();
        if (disposition == SaveCancellationRequestResult::Requested) {
            state_->cancellation.RequestCancellation();
            JobId job{};
            JobSystem *jobs{};
            {
                std::lock_guard lock(state_->mutex);
                job = state_->job;
                jobs = state_->jobs;
            }
            if (jobs && job != 0)
                (void)jobs->RequestCancel(job);
        }
        return disposition;
    }

    SaveStorageAdapter::SaveStorageAdapter(JobSystem &jobs, std::shared_ptr<ISaveStorageProvider> provider, const SaveStorageLimits limits)
        : jobs_(&jobs), provider_(std::move(provider)), limits_(limits) {}

    Result<SaveStorageOperation> SaveStorageAdapter::Submit(const OperationId operation, SaveStorageRequest request,
                                                            CancellationToken cancellation,
                                                            const std::optional<std::chrono::steady_clock::time_point> deadline) const {
        if (!jobs_ || !provider_ || !ValidLimits(limits_) || !ValidRequest(request, limits_))
            return Result<SaveStorageOperation>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
        if (!provider_->Capabilities().Supports(request.kind))
            return Result<SaveStorageOperation>::Failure(MakeError(SaveErrors::StorageCapabilityUnsupported));
        try {
            auto controller = CreateSaveOperation({.operation = operation,
                                                   .kind = LifecycleKind(request.kind),
                                                   .maximumCompletionCallbacks = 1,
                                                   .deadline = deadline,
                                                   .parentCancellation = cancellation});
            if (controller.HasError())
                return Result<SaveStorageOperation>::Failure(controller.ErrorValue());
            auto producer = std::make_shared<SaveOperationController>(std::move(controller).Value());
            auto state = std::make_shared<SaveStorageDetail::SharedOperation>();
            state->operation = producer->Handle();
            state->cancellation = CancellationSource(cancellation);
            state->jobs = jobs_;
            auto submitted = jobs_->SubmitResult({.parentCancellation = state->cancellation.Token(), .operationId = operation},
                                                 [state, provider = provider_, request = std::move(request), limits = limits_,
                                                  producer](const CancellationToken &token) mutable {
                return Execute(state, provider, std::move(request), limits, producer, token);
            });
            if (submitted.HasError())
                return Result<SaveStorageOperation>::Failure(submitted.ErrorValue());
            {
                std::lock_guard lock(state->mutex);
                state->job = submitted.Value().Id();
            }
            return Result<SaveStorageOperation>::Success(SaveStorageOperation{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Result<SaveStorageOperation>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
