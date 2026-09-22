#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiAssetLoadingInternals.h"

#include <algorithm>
#include <new>
#include <string_view>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsRuntimeTerminal(const UiRuntimeAssetLoadState state) noexcept {
            using enum UiRuntimeAssetLoadState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        [[nodiscard]] bool IsAssetTerminal(const Assets::AssetLoadState state) noexcept {
            using enum Assets::AssetLoadState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        [[nodiscard]] bool IsError(const Error &error, const std::string_view code) noexcept {
            return error.code.Value() == code;
        }

        [[nodiscard]] Error TranslateLoadErrorInternal(const Error &error) {
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

        [[nodiscard]] bool IsTerminal(const UiRuntimeAssetLoadState state) noexcept {
            return IsRuntimeTerminal(state);
        }

        [[nodiscard]] bool IsTerminal(const Assets::AssetLoadState state) noexcept {
            return IsAssetTerminal(state);
        }

        [[nodiscard]] Error TranslateLoadError(const Error &error) {
            return TranslateLoadErrorInternal(error);
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

    namespace {
        void CancelHandlesInternal(UiRuntimeAssetLoadHandle::Request &request) noexcept {
            if (request.rootLoad)
                static_cast<void>(request.rootLoad->RequestCancel());
            for (auto &entry : request.dependencies)
                if (entry.load)
                    static_cast<void>(entry.load->RequestCancel());
        }

        void CompleteFailureInternal(UiRuntimeAssetLoadHandle::Request &request, Error error, const bool cancelled = false) noexcept {
            if (!cancelled)
                CancelHandlesInternal(request);
            try {
                request.result = Result<UiRuntimeAssetLoadResult>::Failure(std::move(error));
            } catch (...) {
                request.result.reset();
            }
            request.state.store(cancelled ? UiRuntimeAssetLoadState::Cancelled : UiRuntimeAssetLoadState::Failed);
        }

        void CancelHandles(UiRuntimeAssetLoadHandle::Request &request) noexcept {
            CancelHandlesInternal(request);
        }

        void CompleteFailure(UiRuntimeAssetLoadHandle::Request &request, Error error, const bool cancelled = false) {
            CompleteFailureInternal(request, std::move(error), cancelled);
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
            CancelHandlesInternal(request);
            CompleteFailureInternal(request, MakeError(UiErrors::AssetLoadCancelled), true);
        }

        [[nodiscard]] Result<CookedUiDocument> DecodeRootDocument(UiRuntimeAssetLoadHandle::Request &request,
                                                                  UiRuntimeAssetLoadService::State &state,
                                                                  Assets::AssetLoadResult loadResult) {
            if (loadResult.sourceRegistryRevision != request.snapshot.Revision())
                return Failure<CookedUiDocument>(UiErrors::AssetRegistryStale);
            Assets::AssetCookLimits artifactLimits;
            artifactLimits.maximumArtifactBytes = state.limits.maximumArtifactBytes;
            const auto decoded = Assets::DecodeCookedArtifact(loadResult.bytes, artifactLimits);
            if (decoded.HasError())
                return Result<CookedUiDocument>::Failure(TranslateArtifactError(decoded.ErrorValue()));
            Assets::AssetCookArtifact artifact = std::move(decoded).Value();
            const auto *record = request.snapshot.Find(request.request.canvas.asset);
            if (record == nullptr)
                return Failure<CookedUiDocument>(UiErrors::AssetMissing);
            if (const auto valid = ValidateArtifact(artifact, request.request.canvas.asset, record->type, request.request.target);
                valid.HasError())
                return Result<CookedUiDocument>::Failure(valid.ErrorValue());
            if (artifact.payload.empty())
                return Failure<CookedUiDocument>(UiErrors::CookedPayloadMalformed);
            if (artifact.payload.size() > state.limits.cookedDocument.maximumPayloadBytes)
                return Failure<CookedUiDocument>(UiErrors::AssetBudgetExceeded);
            auto document = CookedUiDocument::Decode(artifact.payload, state.limits.cookedDocument);
            if (document.HasError())
                return Result<CookedUiDocument>::Failure(document.ErrorValue());
            if (document.Value().Id() != request.request.canvas.document ||
                document.Value().SourceRevision() < request.request.canvas.minimumRevision ||
                !HasCanvas(document.Value(), request.request.canvas.canvas))
                return Failure<CookedUiDocument>(UiErrors::CanvasReferenceInvalid);
            return document;
        }

        void ProcessRoot(UiRuntimeAssetLoadHandle::Request &request, UiRuntimeAssetLoadService::State &state) {
            if (!request.rootLoad || !IsTerminal(request.rootLoad->State()))
                return;

            Result<Assets::AssetLoadResult> loaded = request.rootLoad->TakeResult();
            request.rootLoad.reset();
            if (request.activeLoads > 0)
                --request.activeLoads;
            if (loaded.HasError()) {
                const Error error = loaded.ErrorValue();
                if (IsError(error, "asset.load.cancelled")) {
                    CompleteCancelled(request);
                    return;
                }
                CompleteFailure(request, TranslateLoadError(error));
                return;
            }

            auto document = DecodeRootDocument(request, state, std::move(loaded).Value());
            if (document.HasError()) {
                CompleteFailure(request, document.ErrorValue());
                return;
            }
            request.residentBytes = document.Value().Payload().size();
            request.document = std::move(document).Value();
            request.dependencies.reserve(request.document->Dependencies().size());
            for (const UiAssetDependency &dependency : request.document->Dependencies())
                request.dependencies.push_back({dependency});
            request.state.store(UiRuntimeAssetLoadState::LoadingDependencies);
        }

        [[nodiscard]] bool HandleDependencyError(UiRuntimeAssetLoadHandle::Request &request,
                                                 UiRuntimeAssetLoadHandle::Request::DependencyEntry &entry, const Error &error) {
            if (!entry.dependency.required && IsError(error, "asset.provider.not_found")) {
                entry.skipped = true;
                return true;
            }
            if (IsError(error, "asset.load.cancelled"))
                CompleteCancelled(request);
            else
                CompleteFailure(request, TranslateLoadError(error));
            return false;
        }

        [[nodiscard]] bool StoreDependencyPayload(UiRuntimeAssetLoadHandle::Request &request, UiRuntimeAssetLoadService::State &state,
                                                  UiRuntimeAssetLoadHandle::Request::DependencyEntry &entry,
                                                  Assets::AssetLoadResult loadResult) {
            if (loadResult.sourceRegistryRevision != request.snapshot.Revision()) {
                CompleteFailure(request, RegistryStaleError());
                return false;
            }
            const auto *record = request.snapshot.Find(entry.dependency.asset);
            if (record == nullptr) {
                if (!entry.dependency.required) {
                    entry.skipped = true;
                    return true;
                }
                CompleteFailure(request, MakeError(UiErrors::AssetMissing));
                return false;
            }
            Assets::AssetCookLimits artifactLimits;
            artifactLimits.maximumArtifactBytes = state.limits.maximumArtifactBytes;
            const auto decoded = Assets::DecodeCookedArtifact(loadResult.bytes, artifactLimits);
            if (decoded.HasError()) {
                CompleteFailure(request, TranslateArtifactError(decoded.ErrorValue()));
                return false;
            }
            Assets::AssetCookArtifact artifact = std::move(decoded).Value();
            if (const auto valid = ValidateArtifact(artifact, entry.dependency.asset, record->type, request.request.target);
                valid.HasError()) {
                CompleteFailure(request, valid.ErrorValue());
                return false;
            }
            if (artifact.payload.empty()) {
                if (!entry.dependency.required) {
                    entry.skipped = true;
                    return true;
                }
                CompleteFailure(request, MakeError(UiErrors::AssetPayloadEmpty));
                return false;
            }
            if (artifact.payload.size() > state.limits.maximumResidentBytes - request.residentBytes) {
                CompleteFailure(request, MakeError(UiErrors::AssetBudgetExceeded));
                return false;
            }
            request.residentBytes += artifact.payload.size();
            entry.payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(artifact.payload));
            return true;
        }

        void ProcessDependencyCompletion(UiRuntimeAssetLoadHandle::Request &request, UiRuntimeAssetLoadService::State &state,
                                         UiRuntimeAssetLoadHandle::Request::DependencyEntry &entry) {
            if (!entry.load || !IsTerminal(entry.load->State()))
                return;
            Result<Assets::AssetLoadResult> loaded = entry.load->TakeResult();
            entry.load.reset();
            if (request.activeLoads > 0)
                --request.activeLoads;
            if (loaded.HasError()) {
                static_cast<void>(HandleDependencyError(request, entry, loaded.ErrorValue()));
                return;
            }
            static_cast<void>(StoreDependencyPayload(request, state, entry, std::move(loaded).Value()));
        }

        void ProcessDependencyCompletions(UiRuntimeAssetLoadHandle::Request &request, UiRuntimeAssetLoadService::State &state) {
            for (auto &entry : request.dependencies) {
                ProcessDependencyCompletion(request, state, entry);
                if (IsTerminal(request.state.load()))
                    return;
            }
        }

        void SubmitDependencyLoads(UiRuntimeAssetLoadHandle::Request &request, UiRuntimeAssetLoadService::State &state) {
            for (auto &entry : request.dependencies) {
                if (entry.skipped || entry.payload || entry.load)
                    continue;
                const auto *record = request.snapshot.Find(entry.dependency.asset);
                if (record == nullptr) {
                    if (entry.dependency.required) {
                        CompleteFailure(request, MakeError(UiErrors::AssetMissing));
                        return;
                    }
                    entry.skipped = true;
                    continue;
                }
                if (record->type != entry.dependency.expectedType) {
                    CompleteFailure(request, MakeError(UiErrors::AssetTypeMismatch));
                    return;
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
                    return;
                }
                Assets::AssetLoadHandle dependencyLoad = std::move(submitted).Value();
                std::shared_ptr<Assets::AssetLoadHandle> retainedLoad;
                try {
                    retainedLoad = std::make_shared<Assets::AssetLoadHandle>();
                } catch (const std::bad_alloc &) {
                    static_cast<void>(dependencyLoad.RequestCancel());
                    CompleteFailure(request, MakeError(UiErrors::AssetBudgetExceeded));
                    return;
                }
                *retainedLoad = std::move(dependencyLoad);
                entry.load = std::move(retainedLoad);
                ++request.activeLoads;
            }
        }

        void FinalizeRequest(UiRuntimeAssetLoadHandle::Request &request, UiRuntimeAssetLoadService::State &state) {
            if (!request.document || request.activeLoads != 0 || HasPendingDependencies(request) || HasUnsubmittedDependencies(request))
                return;
            if (state.registry.Snapshot().Revision() != request.snapshot.Revision()) {
                CompleteFailure(request, RegistryStaleError());
                return;
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
        }

        void AdvanceRequestInternal(const std::shared_ptr<UiRuntimeAssetLoadHandle::Request> &request,
                                    UiRuntimeAssetLoadService::State &state) {
            std::scoped_lock lock{request->mutex};
            if (IsTerminal(request->state.load()))
                return;
            if (request->parentCancellation.IsCancellationRequested()) {
                CompleteCancelled(*request);
                return;
            }
            if (request->state.load() == UiRuntimeAssetLoadState::Queued)
                request->state.store(UiRuntimeAssetLoadState::LoadingDocument);
            ProcessRoot(*request, state);
            if (IsTerminal(request->state.load()))
                return;
            ProcessDependencyCompletions(*request, state);
            if (IsTerminal(request->state.load()))
                return;
            SubmitDependencyLoads(*request, state);
            if (IsTerminal(request->state.load()))
                return;
            FinalizeRequest(*request, state);
        }

        void CompleteFailureAfterException(UiRuntimeAssetLoadHandle::Request &request, const ErrorCodeDescriptor &descriptor) noexcept {
            try {
                CompleteFailure(request, MakeError(descriptor));
            } catch (...) {
                CancelHandlesInternal(request);
                request.state.store(UiRuntimeAssetLoadState::Failed);
            }
        }

        void DrainRequestInternal(const std::shared_ptr<UiRuntimeAssetLoadHandle::Request> &request) noexcept {
            std::scoped_lock lock{request->mutex};
            if (request->rootLoad)
                static_cast<void>(request->rootLoad->Wait());
            for (const auto &entry : request->dependencies)
                if (entry.load)
                    static_cast<void>(entry.load->Wait());
        }
    }  // namespace

    namespace UiAssetLoadingDetail {
        [[nodiscard]] bool IsTerminal(const UiRuntimeAssetLoadState state) noexcept {
            return IsRuntimeTerminal(state);
        }

        [[nodiscard]] Error TranslateLoadError(const Error &error) {
            return TranslateLoadErrorInternal(error);
        }

        void CancelHandles(UiRuntimeAssetLoadHandle::Request &request) noexcept {
            CancelHandlesInternal(request);
        }

        void CompleteFailure(UiRuntimeAssetLoadHandle::Request &request, Error error, const bool cancelled) {
            CompleteFailureInternal(request, std::move(error), cancelled);
        }

        void AdvanceRequest(const std::shared_ptr<UiRuntimeAssetLoadHandle::Request> &request,
                            UiRuntimeAssetLoadService::State &state) noexcept {
            try {
                AdvanceRequestInternal(request, state);
            } catch (const std::bad_alloc &) {
                std::scoped_lock lock{request->mutex};
                if (!IsTerminal(request->state.load()))
                    CompleteFailureAfterException(*request, UiErrors::AssetBudgetExceeded);
            } catch (...) {
                std::scoped_lock lock{request->mutex};
                if (!IsTerminal(request->state.load()))
                    CompleteFailureAfterException(*request, UiErrors::AssetLoadShutdown);
            }
        }

        void DrainRequest(const std::shared_ptr<UiRuntimeAssetLoadHandle::Request> &request) noexcept {
            DrainRequestInternal(request);
        }
    }  // namespace UiAssetLoadingDetail
}  // namespace Horo::Runtime::Ui
