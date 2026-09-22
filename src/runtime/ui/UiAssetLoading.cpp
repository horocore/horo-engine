#include "Horo/Runtime/Ui/UiAssetLoading.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <new>
#include <string_view>
#include <thread>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsTerminal(const UiRuntimeAssetLoadState state) noexcept {
            using enum UiRuntimeAssetLoadState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        [[nodiscard]] bool IsTerminal(const Assets::AssetLoadState state) noexcept {
            using enum Assets::AssetLoadState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        [[nodiscard]] bool IsError(const Error &error, const std::string_view code) noexcept {
            return error.code.Value() == code;
        }

        [[nodiscard]] Error TranslateLoadError(const Error &error) {
            if (IsError(error, "asset.load.cancelled"))
                return MakeError(UiErrors::AssetLoadCancelled);
            if (IsError(error, "asset.load.shutdown"))
                return MakeError(UiErrors::AssetLoadShutdown);
            if (IsError(error, "asset.load.queue_full") || IsError(error, "job.queue_full"))
                return MakeError(UiErrors::AssetLoadQueueFull);
            if (IsError(error, "asset.provider.not_found"))
                return MakeError(UiErrors::AssetMissing);
            return error;
        }

        [[nodiscard]] Error TranslateArtifactError(const Error &error) {
            if (IsError(error, "asset.cook.unsupported_format"))
                return MakeError(UiErrors::CookedFormatUnsupported);
            if (IsError(error, "asset.cook.malformed_artifact") || IsError(error, "asset.cook.hash_mismatch"))
                return MakeError(UiErrors::CookedPayloadMalformed);
            return error;
        }

        [[nodiscard]] Result<void> ValidateArtifact(const Assets::AssetCookArtifact &artifact, const Assets::AssetId expectedId,
                                                    const Assets::AssetTypeId &expectedType,
                                                    const std::optional<AssetCookTargetId> &expectedTarget) {
            if (artifact.id != expectedId)
                return Failure<void>(UiErrors::AssetIdentityMismatch);
            if (artifact.type != expectedType)
                return Failure<void>(UiErrors::AssetTypeMismatch);
            if (expectedTarget && artifact.target != *expectedTarget)
                return Failure<void>(UiErrors::AssetTargetMismatch);
            return Result<void>::Success();
        }

        [[nodiscard]] bool HasCanvas(const CookedUiDocument &document, const UiCanvasId id) noexcept {
            return std::ranges::find(document.Canvases(), id, &UiCanvasDescriptor::id) != document.Canvases().end();
        }

        [[nodiscard]] Error RegistryStaleError() {
            return MakeError(UiErrors::AssetRegistryStale);
        }
    }  // namespace

    struct UiRuntimeAssetLoadHandle::Request {
        struct DependencyEntry final {
            UiAssetDependency dependency;
            std::shared_ptr<Assets::AssetLoadHandle> load;
            std::shared_ptr<const std::vector<std::uint8_t>> payload;
            bool skipped{};
        };

        std::weak_ptr<UiRuntimeAssetLoadService::State> owner;
        UiRuntimeAssetLoadRequest request;
        Assets::AssetRegistrySnapshot snapshot;
        CancellationToken parentCancellation;
        std::shared_ptr<Assets::AssetLoadHandle> rootLoad;
        std::optional<CookedUiDocument> document;
        std::vector<DependencyEntry> dependencies;
        std::size_t activeLoads{};
        std::size_t residentBytes{};
        std::atomic<UiRuntimeAssetLoadState> state{UiRuntimeAssetLoadState::Queued};
        std::mutex mutex;
        std::optional<Result<UiRuntimeAssetLoadResult>> result;
        bool consumed{};
    };

    struct UiRuntimeAssetLoadService::State {
        State(Assets::AssetRegistry &assetRegistry, Assets::AssetLoadService &assetLoadService, UiRuntimeAssetLoadLimits loadLimits)
            : registry(assetRegistry), loads(assetLoadService), limits(loadLimits) {}

        Assets::AssetRegistry &registry;
        Assets::AssetLoadService &loads;
        UiRuntimeAssetLoadLimits limits;
        std::mutex mutex;
        std::vector<std::shared_ptr<UiRuntimeAssetLoadHandle::Request>> requests;
        bool accepting{true};
    };

    namespace {
        void CancelHandles(UiRuntimeAssetLoadHandle::Request &request) noexcept {
            if (request.rootLoad)
                static_cast<void>(request.rootLoad->RequestCancel());
            for (auto &entry : request.dependencies)
                if (entry.load)
                    static_cast<void>(entry.load->RequestCancel());
        }

        void CompleteFailure(UiRuntimeAssetLoadHandle::Request &request, Error error, const bool cancelled = false) {
            if (!cancelled)
                CancelHandles(request);
            request.result = Result<UiRuntimeAssetLoadResult>::Failure(std::move(error));
            request.state.store(cancelled ? UiRuntimeAssetLoadState::Cancelled : UiRuntimeAssetLoadState::Failed);
        }

        [[nodiscard]] bool HasPendingDependencies(const UiRuntimeAssetLoadHandle::Request &request) noexcept {
            return std::ranges::any_of(request.dependencies, [](const auto &entry) {
                return !entry.skipped && !entry.payload && entry.load;
            });
        }

        [[nodiscard]] bool HasUnsubmittedDependencies(const UiRuntimeAssetLoadHandle::Request &request) noexcept {
            return std::ranges::any_of(request.dependencies, [](const auto &entry) {
                return !entry.skipped && !entry.payload && !entry.load;
            });
        }

        void CompleteCancelled(UiRuntimeAssetLoadHandle::Request &request) {
            CancelHandles(request);
            CompleteFailure(request, MakeError(UiErrors::AssetLoadCancelled), true);
        }

        [[nodiscard]] Result<void> ProcessRoot(UiRuntimeAssetLoadHandle::Request &request, UiRuntimeAssetLoadService::State &state) {
            if (!request.rootLoad || !IsTerminal(request.rootLoad->State()))
                return Result<void>::Success();

            Result<Assets::AssetLoadResult> loaded = request.rootLoad->TakeResult();
            request.rootLoad.reset();
            if (request.activeLoads > 0)
                --request.activeLoads;
            if (loaded.HasError()) {
                const Error error = loaded.ErrorValue();
                if (IsError(error, "asset.load.cancelled")) {
                    CompleteCancelled(request);
                    return Result<void>::Success();
                }
                CompleteFailure(request, TranslateLoadError(error));
                return Result<void>::Success();
            }

            Assets::AssetLoadResult loadResult = std::move(loaded).Value();
            if (loadResult.sourceRegistryRevision != request.snapshot.Revision()) {
                CompleteFailure(request, RegistryStaleError());
                return Result<void>::Success();
            }
            Assets::AssetCookLimits artifactLimits;
            artifactLimits.maximumArtifactBytes = state.limits.maximumArtifactBytes;
            const auto decoded = Assets::DecodeCookedArtifact(loadResult.bytes, artifactLimits);
            if (decoded.HasError()) {
                CompleteFailure(request, TranslateArtifactError(decoded.ErrorValue()));
                return Result<void>::Success();
            }
            Assets::AssetCookArtifact artifact = std::move(decoded).Value();
            const auto *record = request.snapshot.Find(request.request.canvas.asset);
            if (record == nullptr) {
                CompleteFailure(request, MakeError(UiErrors::AssetMissing));
                return Result<void>::Success();
            }
            if (const auto valid = ValidateArtifact(artifact, request.request.canvas.asset, record->type, request.request.target);
                valid.HasError()) {
                CompleteFailure(request, valid.ErrorValue());
                return Result<void>::Success();
            }
            if (artifact.payload.empty()) {
                CompleteFailure(request, MakeError(UiErrors::CookedPayloadMalformed));
                return Result<void>::Success();
            }
            if (artifact.payload.size() > state.limits.cookedDocument.maximumPayloadBytes) {
                CompleteFailure(request, MakeError(UiErrors::AssetBudgetExceeded));
                return Result<void>::Success();
            }
            auto document = CookedUiDocument::Decode(artifact.payload, state.limits.cookedDocument);
            if (document.HasError()) {
                CompleteFailure(request, document.ErrorValue());
                return Result<void>::Success();
            }
            if (document.Value().Id() != request.request.canvas.document ||
                document.Value().SourceRevision() < request.request.canvas.minimumRevision ||
                !HasCanvas(document.Value(), request.request.canvas.canvas)) {
                CompleteFailure(request, MakeError(UiErrors::CanvasReferenceInvalid));
                return Result<void>::Success();
            }
            request.residentBytes = document.Value().Payload().size();
            request.document = std::move(document).Value();
            request.dependencies.reserve(request.document->Dependencies().size());
            for (const UiAssetDependency &dependency : request.document->Dependencies())
                request.dependencies.push_back({dependency});
            request.state.store(UiRuntimeAssetLoadState::LoadingDependencies);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ProcessDependencyCompletions(UiRuntimeAssetLoadHandle::Request &request,
                                                                UiRuntimeAssetLoadService::State &state) {
            for (auto &entry : request.dependencies) {
                if (!entry.load || !IsTerminal(entry.load->State()))
                    continue;
                Result<Assets::AssetLoadResult> loaded = entry.load->TakeResult();
                entry.load.reset();
                if (request.activeLoads > 0)
                    --request.activeLoads;
                if (loaded.HasError()) {
                    const Error error = loaded.ErrorValue();
                    if (!entry.dependency.required && IsError(error, "asset.provider.not_found")) {
                        entry.skipped = true;
                        continue;
                    }
                    if (IsError(error, "asset.load.cancelled")) {
                        CompleteCancelled(request);
                        return Result<void>::Success();
                    }
                    CompleteFailure(request, TranslateLoadError(error));
                    return Result<void>::Success();
                }

                Assets::AssetLoadResult loadResult = std::move(loaded).Value();
                if (loadResult.sourceRegistryRevision != request.snapshot.Revision()) {
                    CompleteFailure(request, RegistryStaleError());
                    return Result<void>::Success();
                }
                const auto *record = request.snapshot.Find(entry.dependency.asset);
                if (record == nullptr) {
                    if (!entry.dependency.required) {
                        entry.skipped = true;
                        continue;
                    }
                    CompleteFailure(request, MakeError(UiErrors::AssetMissing));
                    return Result<void>::Success();
                }
                Assets::AssetCookLimits artifactLimits;
                artifactLimits.maximumArtifactBytes = state.limits.maximumArtifactBytes;
                const auto decoded = Assets::DecodeCookedArtifact(loadResult.bytes, artifactLimits);
                if (decoded.HasError()) {
                    CompleteFailure(request, TranslateArtifactError(decoded.ErrorValue()));
                    return Result<void>::Success();
                }
                Assets::AssetCookArtifact artifact = std::move(decoded).Value();
                if (const auto valid = ValidateArtifact(artifact, entry.dependency.asset, record->type, request.request.target);
                    valid.HasError()) {
                    CompleteFailure(request, valid.ErrorValue());
                    return Result<void>::Success();
                }
                if (artifact.payload.empty()) {
                    if (!entry.dependency.required) {
                        entry.skipped = true;
                        continue;
                    }
                    CompleteFailure(request, MakeError(UiErrors::AssetPayloadEmpty));
                    return Result<void>::Success();
                }
                if (artifact.payload.size() > state.limits.maximumResidentBytes - request.residentBytes) {
                    CompleteFailure(request, MakeError(UiErrors::AssetBudgetExceeded));
                    return Result<void>::Success();
                }
                request.residentBytes += artifact.payload.size();
                entry.payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(artifact.payload));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> SubmitDependencyLoads(UiRuntimeAssetLoadHandle::Request &request,
                                                         UiRuntimeAssetLoadService::State &state) {
            for (auto &entry : request.dependencies) {
                if (entry.skipped || entry.payload || entry.load)
                    continue;
                const auto *record = request.snapshot.Find(entry.dependency.asset);
                if (record == nullptr) {
                    if (entry.dependency.required) {
                        CompleteFailure(request, MakeError(UiErrors::AssetMissing));
                        return Result<void>::Success();
                    }
                    entry.skipped = true;
                    continue;
                }
                if (record->type != entry.dependency.expectedType) {
                    CompleteFailure(request, MakeError(UiErrors::AssetTypeMismatch));
                    return Result<void>::Success();
                }
                if (request.activeLoads >= state.limits.maximumConcurrentLoads)
                    break;
                auto submitted = state.loads.LoadAsync(request.snapshot, entry.dependency.asset, request.parentCancellation);
                if (submitted.HasError()) {
                    const Error error = submitted.ErrorValue();
                    if (!entry.dependency.required && IsError(error, "asset.provider.not_found")) {
                        entry.skipped = true;
                        continue;
                    }
                    CompleteFailure(request, TranslateLoadError(error));
                    return Result<void>::Success();
                }
                Assets::AssetLoadHandle dependencyLoad = std::move(submitted).Value();
                try {
                    entry.load = std::make_shared<Assets::AssetLoadHandle>(std::move(dependencyLoad));
                } catch (const std::bad_alloc &) {
                    static_cast<void>(dependencyLoad.RequestCancel());
                    return Failure<void>(UiErrors::AssetBudgetExceeded);
                }
                ++request.activeLoads;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> FinalizeRequest(UiRuntimeAssetLoadHandle::Request &request, UiRuntimeAssetLoadService::State &state) {
            if (!request.document || request.activeLoads != 0 || HasPendingDependencies(request) || HasUnsubmittedDependencies(request))
                return Result<void>::Success();
            if (state.registry.Snapshot().Revision() != request.snapshot.Revision()) {
                CompleteFailure(request, RegistryStaleError());
                return Result<void>::Success();
            }
            std::vector<UiRuntimeAsset> assets;
            assets.reserve(request.dependencies.size());
            for (auto &entry : request.dependencies)
                if (entry.payload)
                    assets.push_back({entry.dependency, std::move(entry.payload)});
            request.result = Result<UiRuntimeAssetLoadResult>::Success(
                {request.request.canvas.asset, request.snapshot.Revision(), std::move(*request.document), std::move(assets)});
            request.document.reset();
            request.state.store(UiRuntimeAssetLoadState::Succeeded);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AdvanceRequest(const std::shared_ptr<UiRuntimeAssetLoadHandle::Request> &request,
                                                  UiRuntimeAssetLoadService::State &state) {
            std::scoped_lock lock{request->mutex};
            if (IsTerminal(request->state.load()))
                return Result<void>::Success();
            if (request->parentCancellation.IsCancellationRequested()) {
                CompleteCancelled(*request);
                return Result<void>::Success();
            }
            if (request->state.load() == UiRuntimeAssetLoadState::Queued)
                request->state.store(UiRuntimeAssetLoadState::LoadingDocument);
            if (const auto root = ProcessRoot(*request, state); root.HasError())
                return root;
            if (IsTerminal(request->state.load()))
                return Result<void>::Success();
            if (const auto completed = ProcessDependencyCompletions(*request, state); completed.HasError())
                return completed;
            if (IsTerminal(request->state.load()))
                return Result<void>::Success();
            if (const auto submitted = SubmitDependencyLoads(*request, state); submitted.HasError())
                return submitted;
            if (IsTerminal(request->state.load()))
                return Result<void>::Success();
            return FinalizeRequest(*request, state);
        }

        void DrainRequest(const std::shared_ptr<UiRuntimeAssetLoadHandle::Request> &request) noexcept {
            std::scoped_lock lock{request->mutex};
            if (request->rootLoad)
                static_cast<void>(request->rootLoad->Wait());
            for (const auto &entry : request->dependencies)
                if (entry.load)
                    static_cast<void>(entry.load->Wait());
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
        if (IsTerminal(request_->state.load()))
            return Result<void>::Success();
        CancelHandles(*request_);
        CompleteFailure(*request_, MakeError(UiErrors::AssetLoadCancelled), true);
        return Result<void>::Success();
    }

    /** @copydoc UiRuntimeAssetLoadHandle::Wait */
    Result<void> UiRuntimeAssetLoadHandle::Wait() const {
        if (!request_)
            return Failure<void>(UiErrors::AssetLoadShutdown);
        const auto owner = request_->owner.lock();
        if (!owner)
            return IsTerminal(request_->state.load()) ? Result<void>::Success() : Failure<void>(UiErrors::AssetLoadShutdown);
        while (!IsTerminal(request_->state.load())) {
            try {
                static_cast<void>(AdvanceRequest(request_, *owner));
            } catch (const std::bad_alloc &) {
                std::scoped_lock lock{request_->mutex};
                if (!IsTerminal(request_->state.load()))
                    CompleteFailure(*request_, MakeError(UiErrors::AssetBudgetExceeded));
            }
            if (IsTerminal(request_->state.load()))
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
                if (const Result<void> waited = waitFor->Wait(); waited.HasError())
                    return waited;
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
        if (const auto owner = request_->owner.lock()) {
            try {
                static_cast<void>(AdvanceRequest(request_, *owner));
            } catch (const std::bad_alloc &) {
                std::scoped_lock requestLock{request_->mutex};
                if (!IsTerminal(request_->state.load()))
                    CompleteFailure(*request_, MakeError(UiErrors::AssetBudgetExceeded));
            }
        }
        std::scoped_lock lock{request_->mutex};
        if (!IsTerminal(request_->state.load()))
            return Failure<UiRuntimeAssetLoadResult>(UiErrors::AssetLoadNotReady);
        if (request_->consumed)
            return Failure<UiRuntimeAssetLoadResult>(UiErrors::AssetLoadConsumed);
        request_->consumed = true;
        if (!request_->result)
            return Failure<UiRuntimeAssetLoadResult>(UiErrors::AssetLoadCancelled);
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
            return IsTerminal(request->state.load());
        });
        if (state_->requests.size() >= state_->limits.maximumOutstanding)
            return Failure<UiRuntimeAssetLoadHandle>(UiErrors::AssetLoadQueueFull);
        auto submitted = state_->loads.LoadAsync(snapshot, request.canvas.asset, parentCancellation);
        if (submitted.HasError())
            return Result<UiRuntimeAssetLoadHandle>::Failure(TranslateLoadError(submitted.ErrorValue()));

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
                CancelHandles(*operation);
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
        try {
            for (const auto &request : requests)
                static_cast<void>(AdvanceRequest(request, *state_));
        } catch (const std::bad_alloc &) {
            for (const auto &request : requests) {
                std::scoped_lock lock{request->mutex};
                if (!IsTerminal(request->state.load()))
                    CompleteFailure(*request, MakeError(UiErrors::AssetBudgetExceeded));
            }
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
            if (!IsTerminal(request->state.load()))
                CompleteCancelled(*request);
        }
        for (const auto &request : state_->requests)
            DrainRequest(request);
        state_->requests.clear();
    }
}  // namespace Horo::Runtime::Ui
