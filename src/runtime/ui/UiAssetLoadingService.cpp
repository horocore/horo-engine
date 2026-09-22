#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiAssetLoadingInternals.h"

#include <algorithm>
#include <new>
#include <thread>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }
    }  // namespace

    /** @copydoc UiRuntimeAssetLoadResult::CreateInstance */
    Result<UiRuntimeInstance> UiRuntimeAssetLoadResult::CreateInstance(const RuntimeUiInstanceId instance) && {
        return UiRuntimeInstance::Create(std::move(document), instance, std::move(assets));
    }

    /** @copydoc UiRuntimeAssetLoadHandle::State */
    UiRuntimeAssetLoadState UiRuntimeAssetLoadHandle::State() const noexcept {
        return request_ ? request_->state.load() : UiRuntimeAssetLoadState::Failed;
    }

    /** @copydoc UiRuntimeAssetLoadHandle::RequestCancel */
    Result<void> UiRuntimeAssetLoadHandle::RequestCancel() {
        if (!request_)
            return Failure<void>(UiErrors::AssetLoadShutdown);
        std::scoped_lock lock{request_->mutex};
        if (UiAssetLoadingDetail::IsTerminal(request_->state.load()))
            return Result<void>::Success();
        UiAssetLoadingDetail::CancelHandles(*request_);
        UiAssetLoadingDetail::CompleteFailure(*request_, MakeError(UiErrors::AssetLoadCancelled), true);
        return Result<void>::Success();
    }

    /** @copydoc UiRuntimeAssetLoadHandle::Wait */
    Result<void> UiRuntimeAssetLoadHandle::Wait() const {
        if (!request_)
            return Failure<void>(UiErrors::AssetLoadShutdown);
        const auto owner = request_->owner.lock();
        if (!owner)
            return UiAssetLoadingDetail::IsTerminal(request_->state.load()) ? Result<void>::Success()
                                                                            : Failure<void>(UiErrors::AssetLoadShutdown);
        while (!UiAssetLoadingDetail::IsTerminal(request_->state.load())) {
            UiAssetLoadingDetail::AdvanceRequest(request_, *owner);
            if (UiAssetLoadingDetail::IsTerminal(request_->state.load()))
                break;
            std::shared_ptr<Assets::AssetLoadHandle> waitFor;
            {
                std::scoped_lock lock{request_->mutex};
                if (request_->rootLoad)
                    waitFor = request_->rootLoad;
                else
                    for (auto &entry : request_->dependencies)
                        if (entry.load) {
                            waitFor = entry.load;
                            break;
                        }
            }
            if (waitFor) {
                if (const Result<void> waited = waitFor->Wait(); waited.HasError()) {
                    std::scoped_lock lock{request_->mutex};
                    if (!UiAssetLoadingDetail::IsTerminal(request_->state.load()))
                        UiAssetLoadingDetail::CompleteFailure(*request_, UiAssetLoadingDetail::TranslateLoadError(waited.ErrorValue()));
                    return waited;
                }
            } else {
                std::this_thread::yield();
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc UiRuntimeAssetLoadHandle::TakeResult */
    Result<UiRuntimeAssetLoadResult> UiRuntimeAssetLoadHandle::TakeResult() {
        if (!request_)
            return Failure<UiRuntimeAssetLoadResult>(UiErrors::AssetLoadShutdown);
        if (const auto owner = request_->owner.lock())
            UiAssetLoadingDetail::AdvanceRequest(request_, *owner);
        std::scoped_lock lock{request_->mutex};
        if (!UiAssetLoadingDetail::IsTerminal(request_->state.load()))
            return Failure<UiRuntimeAssetLoadResult>(UiErrors::AssetLoadNotReady);
        if (request_->consumed)
            return Failure<UiRuntimeAssetLoadResult>(UiErrors::AssetLoadConsumed);
        request_->consumed = true;
        if (!request_->result)
            return Failure<UiRuntimeAssetLoadResult>(
                request_->state.load() == UiRuntimeAssetLoadState::Failed ? UiErrors::AssetBudgetExceeded : UiErrors::AssetLoadCancelled);
        return std::move(*request_->result);
    }

    /** @copydoc UiRuntimeAssetLoadService::UiRuntimeAssetLoadService */
    UiRuntimeAssetLoadService::UiRuntimeAssetLoadService(Assets::AssetRegistry &registry, Assets::AssetLoadService &loads,
                                                         const UiRuntimeAssetLoadLimits limits)
        : state_(std::make_shared<State>(registry, loads, limits)) {}

    UiRuntimeAssetLoadService::~UiRuntimeAssetLoadService() {
        Shutdown();
        state_.reset();
    }

    /** @copydoc UiRuntimeAssetLoadService::LoadAsync */
    Result<UiRuntimeAssetLoadHandle> UiRuntimeAssetLoadService::LoadAsync(UiRuntimeAssetLoadRequest request,
                                                                          const CancellationToken &parentCancellation) {
        if (!state_)
            return Failure<UiRuntimeAssetLoadHandle>(UiErrors::AssetLoadShutdown);
        return LoadAsync(state_->registry.Snapshot(), std::move(request), parentCancellation);
    }

    /** @copydoc UiRuntimeAssetLoadService::LoadAsync */
    Result<UiRuntimeAssetLoadHandle> UiRuntimeAssetLoadService::LoadAsync(const Assets::AssetRegistrySnapshot &snapshot,
                                                                          UiRuntimeAssetLoadRequest request,
                                                                          const CancellationToken &parentCancellation) {
        if (!state_ || !state_->limits.IsValid())
            return Failure<UiRuntimeAssetLoadHandle>(UiErrors::AssetBudgetExceeded);
        if (parentCancellation.IsCancellationRequested())
            return Failure<UiRuntimeAssetLoadHandle>(UiErrors::AssetLoadCancelled);
        if (const auto valid = ValidateUiCanvasAssetReference(request.canvas); valid.HasError())
            return Result<UiRuntimeAssetLoadHandle>::Failure(valid.ErrorValue());
        if (request.target && !request.target->IsValid())
            return Failure<UiRuntimeAssetLoadHandle>(UiErrors::AssetTargetMismatch);
        const auto *record = snapshot.Find(request.canvas.asset);
        if (record == nullptr)
            return Failure<UiRuntimeAssetLoadHandle>(UiErrors::AssetMissing);
        if (request.expectedAssetType.Value().empty())
            request.expectedAssetType = record->type;
        if (record->type != request.expectedAssetType)
            return Failure<UiRuntimeAssetLoadHandle>(UiErrors::AssetTypeMismatch);

        std::scoped_lock lock{state_->mutex};
        if (!state_->accepting)
            return Failure<UiRuntimeAssetLoadHandle>(UiErrors::AssetLoadShutdown);
        std::erase_if(state_->requests, [](const auto &request) {
            return UiAssetLoadingDetail::IsTerminal(request->state.load());
        });
        if (state_->requests.size() >= state_->limits.maximumOutstanding)
            return Failure<UiRuntimeAssetLoadHandle>(UiErrors::AssetLoadQueueFull);
        auto submitted = state_->loads.LoadAsync(snapshot, request.canvas.asset, parentCancellation);
        if (submitted.HasError())
            return Result<UiRuntimeAssetLoadHandle>::Failure(UiAssetLoadingDetail::TranslateLoadError(submitted.ErrorValue()));

        Assets::AssetLoadHandle rootLoad = std::move(submitted).Value();
        std::shared_ptr<UiRuntimeAssetLoadHandle::Request> operation;
        try {
            operation = std::make_shared<UiRuntimeAssetLoadHandle::Request>();
            operation->owner = state_;
            operation->request = std::move(request);
            operation->snapshot = snapshot;
            operation->parentCancellation = parentCancellation;
            operation->rootLoad = std::make_shared<Assets::AssetLoadHandle>(std::move(rootLoad));
            operation->activeLoads = 1;
            operation->state.store(UiRuntimeAssetLoadState::LoadingDocument);
            state_->requests.push_back(operation);
            return Result<UiRuntimeAssetLoadHandle>::Success(UiRuntimeAssetLoadHandle{std::move(operation)});
        } catch (const std::bad_alloc &) {
            if (operation)
                UiAssetLoadingDetail::CancelHandles(*operation);
            static_cast<void>(rootLoad.RequestCancel());
            return Failure<UiRuntimeAssetLoadHandle>(UiErrors::AssetBudgetExceeded);
        }
    }

    /** @copydoc UiRuntimeAssetLoadService::Advance */
    Result<void> UiRuntimeAssetLoadService::Advance() {
        if (!state_)
            return Result<void>::Success();
        std::vector<std::shared_ptr<UiRuntimeAssetLoadHandle::Request>> requests;
        try {
            std::scoped_lock lock{state_->mutex};
            requests = state_->requests;
        } catch (const std::bad_alloc &) {
            return Failure<void>(UiErrors::AssetBudgetExceeded);
        }
        for (const auto &request : requests)
            UiAssetLoadingDetail::AdvanceRequest(request, *state_);
        {
            std::scoped_lock lock{state_->mutex};
            std::erase_if(state_->requests, [](const auto &request) {
                return UiAssetLoadingDetail::IsTerminal(request->state.load());
            });
        }
        return Result<void>::Success();
    }

    /** @copydoc UiRuntimeAssetLoadService::Shutdown */
    void UiRuntimeAssetLoadService::Shutdown() noexcept {
        if (!state_)
            return;
        std::scoped_lock serviceLock{state_->mutex};
        if (!state_->accepting && state_->requests.empty())
            return;
        state_->accepting = false;
        for (const auto &request : state_->requests) {
            std::scoped_lock lock{request->mutex};
            if (!UiAssetLoadingDetail::IsTerminal(request->state.load())) {
                UiAssetLoadingDetail::CancelHandles(*request);
                UiAssetLoadingDetail::CompleteFailure(*request, MakeError(UiErrors::AssetLoadCancelled), true);
            }
        }
        for (const auto &request : state_->requests)
            UiAssetLoadingDetail::DrainRequest(request);
        state_->requests.clear();
    }
}  // namespace Horo::Runtime::Ui
