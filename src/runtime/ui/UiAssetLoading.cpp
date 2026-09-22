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

        void CompleteFailureInternal(UiRuntimeAssetLoadHandle::Request &request, Error error, const bool cancelled = false) {
            if (!cancelled)
                CancelHandlesInternal(request);
            request.result = Result<UiRuntimeAssetLoadResult>::Failure(std::move(error));
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

            auto document = DecodeRootDocument(request, state, std::move(loaded).Value());
            if (document.HasError()) {
                CompleteFailure(request, document.ErrorValue());
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
                    if (!HandleDependencyError(request, entry, loaded.ErrorValue()))
                        return Result<void>::Success();
                    continue;
                }
                if (!StoreDependencyPayload(request, state, entry, std::move(loaded).Value()))
                    return Result<void>::Success();
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

        [[nodiscard]] Result<void> AdvanceRequestInternal(const std::shared_ptr<UiRuntimeAssetLoadHandle::Request> &request,
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

        [[nodiscard]] Result<void> AdvanceRequest(const std::shared_ptr<UiRuntimeAssetLoadHandle::Request> &request,
                                                  UiRuntimeAssetLoadService::State &state) {
            return AdvanceRequestInternal(request, state);
        }

        void DrainRequest(const std::shared_ptr<UiRuntimeAssetLoadHandle::Request> &request) noexcept {
            DrainRequestInternal(request);
        }
    }  // namespace UiAssetLoadingDetail
}  // namespace Horo::Runtime::Ui
