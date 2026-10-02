#include "Horo/WorldStreaming/StreamingCellAssetRequest.h"

#include "Horo/Assets/AssetProvider.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "WorldStreamingInternal.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        [[nodiscard]] bool IsKnown(const StreamingCellAssetRequestLifecycle value) noexcept {
            return value < StreamingCellAssetRequestLifecycle::Count;
        }

        [[nodiscard]] const WorldPartitionCellDescriptor *FindCell(const CookedWorldIndexManifest &manifest, const StreamingCellId &cell) {
            const auto cells = manifest.Descriptor().Cells();
            const auto found = std::ranges::lower_bound(cells, cell, StreamingCellCanonicalLess{}, &WorldPartitionCellDescriptor::id);
            return found != cells.end() && found->id == cell ? std::to_address(found) : nullptr;
        }

        [[nodiscard]] std::optional<std::size_t> FindManifestCellIndex(const CookedWorldIndexManifest &manifest,
                                                                       const StreamingCellId &cell) {
            const auto cells = manifest.Cells();
            const auto found = std::ranges::lower_bound(cells, cell, StreamingCellCanonicalLess{}, &CookedWorldCellManifestEntry::cell);
            if (found == cells.end() || found->cell != cell)
                return std::nullopt;
            return static_cast<std::size_t>(found - cells.begin());
        }

        [[nodiscard]] bool Matches(const CookedWorldCellManifestEntry &manifest,
                                   const StreamingCellCandidateManifestRecord &candidate) noexcept {
            return manifest.cell == candidate.cell && manifest.uncompressedSize == candidate.uncompressedSize &&
                   manifest.compressedSize == candidate.compressedSize && manifest.payloadCrc32 == candidate.payloadCrc32 &&
                   manifest.artifactHash == candidate.artifactHash;
        }

        [[nodiscard]] bool IsTerminal(const Assets::AssetLoadState state) noexcept {
            using enum Assets::AssetLoadState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        [[nodiscard]] Result<void> ValidateAdmissionContext(const CookedWorldIndexManifest &manifest,
                                                            const StreamingCellCandidate &candidate,
                                                            const StreamingCellAssetRequestContext &context) {
            if (!IsKnown(context.lifecycle) || !context.request.IsValid() || !context.operation.IsValid() || context.maximumRequests == 0)
                return Internal::Failure<void>(WorldStreamingErrors::CellAssetRequestInvalid);
            if (context.lifecycle != StreamingCellAssetRequestLifecycle::Active)
                return Internal::Failure<void>(WorldStreamingErrors::CellAssetRequestLifecycleUnavailable);
            if (context.operation != candidate.Operation() || context.operation.fence.partition != manifest.Descriptor().Partition())
                return Internal::Failure<void>(WorldStreamingErrors::CellAssetRequestStale);
            const auto manifestCell = FindManifestCellIndex(manifest, context.operation.fence.cell);
            if (const auto *descriptorCell = FindCell(manifest, context.operation.fence.cell);
                !manifestCell || !descriptorCell || !Matches(manifest.Cells()[*manifestCell], candidate.ManifestEntry()) ||
                descriptorCell->package.chunkAsset != candidate.ChunkAsset() ||
                !std::ranges::equal(manifest.HardDependencies(*manifestCell), candidate.HardDependencies()))
                return Internal::Failure<void>(WorldStreamingErrors::CellAssetRequestStale);
            if (candidate.HardDependencies().size() >= context.maximumRequests)
                return Internal::Failure<void>(WorldStreamingErrors::CellAssetRequestCapacityExceeded);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<Assets::AssetId>> ResolveRequestAssets(const CookedWorldIndexManifest &manifest,
                                                                                const StreamingCellCandidate &candidate,
                                                                                const Assets::AssetRegistrySnapshot &registry) {
            std::vector<Assets::AssetId> assets;
            assets.reserve(candidate.HardDependencies().size() + 1U);
            assets.push_back(candidate.ChunkAsset());
            for (const auto &dependency : candidate.HardDependencies()) {
                const auto *cell = FindCell(manifest, dependency);
                if (!cell)
                    return Internal::Failure<std::vector<Assets::AssetId>>(WorldStreamingErrors::CellAssetRequestUnavailable);
                assets.push_back(cell->package.chunkAsset);
            }
            for (const auto &asset : assets)
                if (!registry.Find(asset))
                    return Internal::Failure<std::vector<Assets::AssetId>>(WorldStreamingErrors::CellAssetRequestUnavailable);
            return Result<std::vector<Assets::AssetId>>::Success(std::move(assets));
        }
    }  // namespace

    struct StreamingCellAssetRequest::StateData final {
        StreamingCellAssetRequestId request;
        StreamingCellOperationHandle operation;
        Assets::AssetRegistryRevision registryRevision;
        CancellationSource cancellation;
        std::vector<Assets::AssetId> assets;
        std::vector<Assets::AssetLoadHandle> handles;
        mutable std::mutex mutex;
        bool cancellationRequested{};
        bool consumed{};

        [[nodiscard]] StreamingCellAssetRequestState RefreshState() {
            using enum Assets::AssetLoadState;
            bool anyLoading{};
            bool anyFailed{};
            bool anyCancelled{};
            for (const auto &handle : handles) {
                const auto state = handle.State();
                anyLoading = anyLoading || !IsTerminal(state);
                anyFailed = anyFailed || state == Failed;
                anyCancelled = anyCancelled || state == Cancelled;
            }
            if (anyLoading) {
                if ((anyFailed || anyCancelled) && !cancellationRequested)
                    CancelChildren();
                return cancellationRequested ? StreamingCellAssetRequestState::Cancelling : StreamingCellAssetRequestState::Loading;
            }
            if (anyFailed)
                return StreamingCellAssetRequestState::Failed;
            if (anyCancelled || cancellationRequested)
                return StreamingCellAssetRequestState::Cancelled;
            return StreamingCellAssetRequestState::Ready;
        }

        void CancelChildren() {
            cancellationRequested = true;
            cancellation.RequestCancellation();
            for (auto &handle : handles)
                static_cast<void>(handle.RequestCancel());
        }

        void CancelRoot() noexcept {
            cancellationRequested = true;
            cancellation.RequestCancellation();
        }
    };

    StreamingCellAssetRequest::StreamingCellAssetRequest(std::shared_ptr<StateData> state) noexcept : state_(std::move(state)) {}

    /** @copydoc StreamingCellAssetRequest::~StreamingCellAssetRequest */
    StreamingCellAssetRequest::~StreamingCellAssetRequest() {
        if (state_)
            state_->CancelRoot();
    }

    /** @copydoc StreamingCellAssetRequest::operator= */
    StreamingCellAssetRequest &StreamingCellAssetRequest::operator=(StreamingCellAssetRequest &&other) noexcept {
        if (this == &other)
            return *this;
        if (state_)
            state_->CancelRoot();
        state_ = std::move(other.state_);
        return *this;
    }

    /** @copydoc StreamingCellAssetRequest::State */
    StreamingCellAssetRequestState StreamingCellAssetRequest::State() const {
        if (!state_)
            return StreamingCellAssetRequestState::Failed;
        std::scoped_lock lock{state_->mutex};
        return state_->RefreshState();
    }

    /** @copydoc StreamingCellAssetRequest::RequestCancel */
    Result<void> StreamingCellAssetRequest::RequestCancel() {
        if (!state_)
            return Internal::Failure<void>(WorldStreamingErrors::CellAssetRequestLifecycleUnavailable);
        std::scoped_lock lock{state_->mutex};
        if (state_->consumed)
            return Internal::Failure<void>(WorldStreamingErrors::CellAssetRequestConsumed);
        state_->CancelChildren();
        return Result<void>::Success();
    }

    /** @copydoc StreamingCellAssetRequest::TakeResult */
    Result<StreamingCellAssetBatch> StreamingCellAssetRequest::TakeResult() {
        if (!state_)
            return Internal::Failure<StreamingCellAssetBatch>(WorldStreamingErrors::CellAssetRequestLifecycleUnavailable);
        std::scoped_lock lock{state_->mutex};
        if (const auto state = state_->RefreshState();
            state == StreamingCellAssetRequestState::Loading || state == StreamingCellAssetRequestState::Cancelling)
            return Internal::Failure<StreamingCellAssetBatch>(WorldStreamingErrors::CellAssetRequestNotReady);
        if (state_->consumed)
            return Internal::Failure<StreamingCellAssetBatch>(WorldStreamingErrors::CellAssetRequestConsumed);
        state_->consumed = true;

        StreamingCellAssetBatch batch{state_->request, state_->operation, state_->registryRevision, {}};
        batch.assets.reserve(state_->handles.size());
        for (std::size_t index{}; index < state_->handles.size(); ++index) {
            auto loaded = state_->handles[index].TakeResult();
            if (loaded.HasError())
                return Result<StreamingCellAssetBatch>::Failure(loaded.ErrorValue());
            auto value = std::move(loaded).Value();
            batch.assets.emplace_back(state_->assets[index], std::move(value.bytes));
        }
        if (state_->cancellationRequested)
            return Internal::Failure<StreamingCellAssetBatch>(WorldStreamingErrors::CellAssetRequestCancelled);
        return Result<StreamingCellAssetBatch>::Success(std::move(batch));
    }

    /** @copydoc RequestStreamingCellAssets */
    Result<StreamingCellAssetRequest> RequestStreamingCellAssets(Assets::AssetLoadService &service,
                                                                 const Assets::AssetRegistrySnapshot &registry,
                                                                 const CookedWorldIndexManifest &manifest,
                                                                 const StreamingCellCandidate &candidate,
                                                                 const StreamingCellAssetRequestContext &context) {
        if (const auto valid = ValidateAdmissionContext(manifest, candidate, context); valid.HasError())
            return Result<StreamingCellAssetRequest>::Failure(valid.ErrorValue());
        auto resolvedAssets = ResolveRequestAssets(manifest, candidate, registry);
        if (resolvedAssets.HasError())
            return Result<StreamingCellAssetRequest>::Failure(resolvedAssets.ErrorValue());

        auto state = std::make_shared<StreamingCellAssetRequest::StateData>();
        state->request = context.request;
        state->operation = context.operation;
        state->registryRevision = registry.Revision();
        state->assets = std::move(resolvedAssets).Value();
        state->handles.reserve(state->assets.size());

        const auto cancellation = state->cancellation.Token();
        for (const auto &asset : state->assets) {
            auto submitted = service.LoadAsync(registry, asset, cancellation);
            if (submitted.HasError()) {
                state->CancelChildren();
                return Result<StreamingCellAssetRequest>::Failure(submitted.ErrorValue());
            }
            state->handles.push_back(std::move(submitted).Value());
        }
        return Result<StreamingCellAssetRequest>::Success(StreamingCellAssetRequest{std::move(state)});
    }
}  // namespace Horo::WorldStreaming
