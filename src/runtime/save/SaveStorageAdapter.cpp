#include "Horo/Runtime/Save/SaveStorageAdapter.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveTelemetry.h"
#include "Horo/Runtime/Save/SaveThumbnailArchive.h"

#include <algorithm>
#include <mutex>
#include <new>
#include <utility>

namespace Horo::Runtime {
    class SaveStorageDetail::SharedOperation final {
    public:
        void SetOperation(SaveOperationHandle operation) {
            operation_ = std::move(operation);
        }

        [[nodiscard]] std::optional<SaveOperationSnapshot> Snapshot() const {
            return operation_.Snapshot();
        }

        [[nodiscard]] SaveCancellationRequestResult RequestCancellation() const noexcept {
            return operation_.RequestCancellation();
        }

        void SetCancellation(CancellationSource cancellation) {
            cancellation_ = std::move(cancellation);
        }

        [[nodiscard]] CancellationToken Cancellation() const noexcept {
            return cancellation_.Token();
        }

        void RequestParentCancellation() const noexcept {
            cancellation_.RequestCancellation();
        }

        void StoreValue(SaveStorageValue value) {
            std::lock_guard lock(mutex_);
            value_.emplace(std::move(value));
        }

        [[nodiscard]] std::optional<SaveStorageValue> Value() const {
            std::lock_guard lock(mutex_);
            return value_;
        }

        void SetScheduler(JobSystem &jobs) noexcept {
            jobs_ = &jobs;
        }

        void SetJob(const JobId job) {
            std::lock_guard lock(mutex_);
            job_ = job;
        }

        void RequestJobCancellation() const {
            std::lock_guard lock(mutex_);
            if (jobs_ && job_ != 0)
                (void)jobs_->RequestCancel(job_);
        }

    private:
        mutable std::mutex mutex_;
        SaveOperationHandle operation_;
        CancellationSource cancellation_;
        std::optional<SaveStorageValue> value_;
        JobSystem *jobs_{};
        JobId job_{};
    };

    namespace {
        [[nodiscard]] bool IsKnown(const SaveStorageOperationKind kind) noexcept {
            return static_cast<std::uint8_t>(kind) < static_cast<std::uint8_t>(SaveStorageOperationKind::Count);
        }

        [[nodiscard]] bool IsMutation(const SaveStorageOperationKind kind) noexcept {
            using enum SaveStorageOperationKind;
            return kind == Write || kind == Copy || kind == Rename || kind == Delete;
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
                !std::ranges::is_sorted(*list->entries, EntryLess))
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
            using enum SaveStorageOperationKind;
            switch (request.kind) {
                case List:
                    return ValidListValue(value, limits);
                case ReadMetadata:
                    return ValidMetadataValue(request, value);
                case ReadArchive:
                    return ValidArchiveValue(value, limits);
                case Exists:
                    return std::holds_alternative<bool>(value);
                case Write:
                case Copy:
                case Rename:
                case Delete:
                    return std::holds_alternative<std::monostate>(value);
                case Count:
                    break;
            }
            return false;
        }

        [[nodiscard]] SaveOperationStage StorageStage(const SaveStorageOperationKind kind) noexcept {
            using enum SaveOperationStage;
            if (kind == SaveStorageOperationKind::Delete)
                return Deleting;
            return IsMutation(kind) ? WritingTemporary : RefreshingCatalog;
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
            }
        }

        /** @brief Finalizes optional presentation bytes on the worker before the existing commit gate. */
        [[nodiscard]] bool PreparePresentation(SaveStorageRequest &request,
                                               const std::shared_ptr<const SavePresentationArchiveInput> &input,
                                               const SaveStorageLimits &limits, SaveOperationController &controller) {
            if (!input)
                return true;
            if (controller.PublishProgress(SaveOperationStage::FinalizingArchive, {0, 1}) != SaveOperationTransitionResult::Applied)
                return false;
            auto prepared = PrepareSavePresentationWrite(*input);
            if (prepared.HasError()) {
                Fail(controller, prepared.ErrorValue(), false);
                return false;
            }
            request.write = std::move(prepared).Value();
            if (!ValidWrite(request, limits)) {
                Fail(controller, MakeError(SaveErrors::StorageOperationInvalid), false);
                return false;
            }
            return true;
        }

        [[nodiscard]] Result<void> Execute(const std::shared_ptr<SaveStorageDetail::SharedOperation> &state,
                                           const std::shared_ptr<ISaveStorageProvider> &provider, const SaveStorageRequest &request,
                                           const SaveStorageLimits limits, const std::shared_ptr<SaveOperationController> &controller,
                                           const CancellationToken &cancellation, const OperationId operation) {
            const bool mutation = IsMutation(request.kind);
            SaveStageObservation observation{mutation ? SaveTelemetryStage::Commit : SaveTelemetryStage::Restore, operation};
            if (const SaveOperationStage stage = StorageStage(request.kind);
                controller->PublishProgress(stage, {1, 1}) != SaveOperationTransitionResult::Applied)
                return Result<void>::Success();
            if (mutation && controller->BeginCommit() != SaveCommitGateResult::Entered)
                return Result<void>::Success();
            // Crossing the mutation commit gate transfers durability responsibility to the provider;
            // cancellation can no longer interrupt publication without risking a partial destination.
            const CancellationToken providerCancellation = mutation ? CancellationToken{} : cancellation;
            Result<SaveStorageValue> executed = ExecuteProvider(*provider, request, providerCancellation);
            if (executed.HasError()) {
                observation.Fail(executed.ErrorValue());
                Fail(*controller, executed.ErrorValue(), mutation);
                return Result<void>::Success();
            }
            if (!ValidValue(request, executed.Value(), limits)) {
                observation.Fail(MakeError(SaveErrors::StorageResultInvalid));
                Fail(*controller, MakeError(SaveErrors::StorageResultInvalid), mutation);
                return Result<void>::Success();
            }
            SaveTelemetryEvidence evidence;
            if (const auto *archive = std::get_if<ImmutableSaveArchive>(&executed.Value()); archive && archive->bytes)
                evidence.bytes = archive->bytes->size();
            else if (request.write && request.write->archive.bytes)
                evidence.bytes = request.write->archive.bytes->size();
            state->StoreValue(std::move(executed).Value());
            (void)controller->Complete(mutation ? SaveOperationCommitOutcome::Committed : SaveOperationCommitOutcome::NotCommitted);
            observation.Complete(SaveTelemetryOutcome::Succeeded, evidence);
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
        return state_ ? state_->Snapshot() : std::nullopt;
    }

    std::optional<SaveStorageValue> SaveStorageOperation::Value() const {
        if (!state_)
            return std::nullopt;
        if (const auto snapshot = state_->Snapshot(); !snapshot || snapshot->state != SaveOperationState::Completed)
            return std::nullopt;
        return state_->Value();
    }

    SaveCancellationRequestResult SaveStorageOperation::RequestCancellation() const noexcept {
        if (!state_)
            return SaveCancellationRequestResult::InvalidHandle;
        const SaveCancellationRequestResult disposition = state_->RequestCancellation();
        if (disposition == SaveCancellationRequestResult::Requested) {
            state_->RequestParentCancellation();
            state_->RequestJobCancellation();
        }
        return disposition;
    }

    SaveStorageAdapter::SaveStorageAdapter(JobSystem &jobs, std::shared_ptr<ISaveStorageProvider> provider, const SaveStorageLimits limits)
        : jobs_(&jobs), provider_(std::move(provider)), limits_(limits) {}

    Result<SaveStorageOperation> SaveStorageAdapter::Submit(const OperationId operation, SaveStorageRequest request,
                                                            CancellationToken cancellation,
                                                            const std::optional<std::chrono::steady_clock::time_point> deadline) const {
        return ObserveSaveStage(SaveTelemetryStage::Queue, operation, [this, operation, &request, &cancellation, deadline] {
            return SubmitRequest(operation, std::move(request), cancellation, deadline);
        }, {});
    }

    /** @copydoc SaveStorageAdapter::SubmitPresentation */
    Result<SaveStorageOperation> SaveStorageAdapter::SubmitPresentation(
        const OperationId operation, SaveStorageAddress address, SavePresentationArchiveInput input, CancellationToken cancellation,
        const std::optional<std::chrono::steady_clock::time_point> deadline) const {
        try {
            const auto &name = address.namespaceAccess.expected;
            if (input.header.product != name.product || input.header.environment != name.environment)
                return Result<SaveStorageOperation>::Failure(MakeError(SaveErrors::NamespaceStale));
            if (const auto *owner = std::get_if<UserProfileOwner>(&name.owner);
                owner && (input.header.user != owner->user || input.header.profile != owner->profile))
                return Result<SaveStorageOperation>::Failure(MakeError(SaveErrors::NamespaceStale));
            auto publication = input.publication;
            publication.thumbnail = input.capture.artifact ? std::optional{input.capture.artifact->Request().thumbnail} : std::nullopt;
            if (const auto valid = ValidateSaveThumbnailPublication(publication, input.capture); valid.HasError())
                return Result<SaveStorageOperation>::Failure(valid.ErrorValue());
            input.limits.maximumArchiveBytes = std::min(input.limits.maximumArchiveBytes, limits_.maximumArchiveBytes);
            auto owned = std::make_shared<const SavePresentationArchiveInput>(std::move(input));
            return SubmitRequest(operation, {.kind = SaveStorageOperationKind::Write, .source = std::move(address)}, cancellation, deadline,
                                 std::move(owned));
        } catch (const std::bad_alloc &) {
            return Result<SaveStorageOperation>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc SaveStorageAdapter::SubmitRequest */
    Result<SaveStorageOperation> SaveStorageAdapter::SubmitRequest(const OperationId operation, SaveStorageRequest request,
                                                                   CancellationToken cancellation,
                                                                   const std::optional<std::chrono::steady_clock::time_point> deadline,
                                                                   std::shared_ptr<const SavePresentationArchiveInput> presentation) const {
        if (const bool validPresentation = presentation && request.kind == SaveStorageOperationKind::Write &&
                                           ValidAddress(request.source) && !request.write && !request.destination &&
                                           presentation->publication.slot == request.source.slot;
            !jobs_ || !provider_ || !ValidLimits(limits_) || (presentation ? !validPresentation : !ValidRequest(request, limits_)))
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
            state->SetOperation(producer->Handle());
            state->SetCancellation(CancellationSource(cancellation));
            state->SetScheduler(*jobs_);
            auto submitted =
                jobs_->SubmitResult({.parentCancellation = state->Cancellation(), .operationId = operation},
                                    [state, provider = provider_, request = std::move(request), limits = limits_, producer, operation,
                                     presentation = std::move(presentation)](const CancellationToken &token) mutable {
                if (!PreparePresentation(request, presentation, limits, *producer))
                    return Result<void>::Success();
                return Execute(state, provider, request, limits, producer, token, operation);
            });
            if (submitted.HasError())
                return Result<SaveStorageOperation>::Failure(submitted.ErrorValue());
            state->SetJob(submitted.Value().Id());
            return Result<SaveStorageOperation>::Success(SaveStorageOperation{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Result<SaveStorageOperation>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
