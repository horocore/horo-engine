#include "Horo/Editor/WorldStreamingPreview.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;
        using Geometry = std::shared_ptr<const WorldStreamingPreviewGeometry>;

        /** @brief Validates one coherent native registry/planner/diagnostic publication. */
        Result<void> ValidateNativeCapture(const WS::WorldPartitionRegistrySnapshot &registry, const WS::StreamingSourcePlanView &plan,
                                           const WS::WorldStreamingDiagnosticSnapshot &diagnostics) {
            if (!registry.IsValid() || plan.revision == 0)
                return Result<void>::Failure(MakeError(WorldDocumentErrors::Lifecycle));
            if (plan.partition != registry.Binding() || diagnostics.Owner() != registry.Binding().owner)
                return Result<void>::Failure(MakeError(WorldDocumentErrors::Stale));
            if (diagnostics.Lifecycle() == WS::WorldStreamingRuntimeCompositionState::Closed)
                return Result<void>::Failure(MakeError(WorldDocumentErrors::Lifecycle));
            return Result<void>::Success();
        }

        /** @brief Converts all eight exact corners using the same global-origin conversion as source tools. */
        Result<WorldStreamingPreviewBox> Box(const WorldStreamingPreviewCell &cell, const std::size_t index,
                                             const Math::WorldCoordinate64 origin) {
            const auto low = cell.bounds.minimum.Millimeters();
            const auto high = cell.bounds.maximum.Millimeters();
            WorldStreamingPreviewBox box{.cellIndex = index};
            for (std::size_t corner = 0; corner < box.corners.size(); ++corner) {
                const auto point =
                    Math::WorldCoordinate64::FromMillimeters((corner & 1) ? high[0] : low[0], (corner & 2) ? high[1] : low[1],
                                                             (corner & 4) ? high[2] : low[2]);
                const auto local = WorldSourceViewportPoint(point, origin);
                if (local.HasError())
                    return Result<WorldStreamingPreviewBox>::Failure(local.ErrorValue());
                box.corners[corner] = local.Value();
            }
            return Result<WorldStreamingPreviewBox>::Success(box);
        }

        /** @brief Prepares only local boxes under a finite budget and discloses out-of-window culling. */
        Result<void> FillGeometry(WorldStreamingPreviewGeometry &geometry, const std::size_t maximumBoxes,
                                  const std::stop_token cancellation) {
            geometry.boxes.reserve(std::min(maximumBoxes, geometry.preview->Cells().size()));
            for (std::size_t index = 0; index < geometry.preview->Cells().size(); ++index) {
                if (cancellation.stop_requested())
                    return Result<void>::Failure(MakeError(WorldDocumentErrors::Cancelled));
                const auto box = Box(geometry.preview->Cells()[index], index, geometry.globalOrigin);
                if (box.HasError()) {
                    ++geometry.outsideLocalWindow;
                    continue;
                }
                if (geometry.boxes.size() == maximumBoxes)
                    return Result<void>::Failure(MakeError(WorldDocumentErrors::Capacity));
                geometry.boxes.push_back(box.Value());
            }
            if (cancellation.stop_requested())
                return Result<void>::Failure(MakeError(WorldDocumentErrors::Cancelled));
            return Result<void>::Success();
        }

        /** @brief Tests cached presentation against all source and view publications, without querying any owner. */
        bool CurrentGeometry(const Geometry &geometry, const WorldViewportBinding &view, const WorldStreamingPreviewFence &fence) {
            return geometry && geometry->preview->Fence() == fence && geometry->globalOrigin == view.globalOrigin &&
                   geometry->preview->Source() == view.snapshot;
        }

        /** @brief Detects native publication rollback even after failed refresh cleared displayed geometry. */
        bool Regressed(const std::optional<WorldStreamingPreviewFence> &previous, const WorldStreamingPreviewFence &next) {
            return previous && (next.diagnostics.Value() < previous->diagnostics.Value() || next.planRevision < previous->planRevision ||
                                next.registry.revision.Value() < previous->registry.revision.Value());
        }

        /** @brief Rejects replayed view revisions and changed origins/incarnations that reused a publication identity. */
        bool RegressedView(const std::optional<std::pair<std::uint64_t, Math::WorldCoordinate64>> &previous,
                           const std::optional<WorldStreamingPreviewFence> &latest, const WorldViewportBinding &view) {
            if (!previous || !latest)
                return false;
            if (view.viewRevision < previous->first)
                return true;
            if (view.viewRevision != previous->first)
                return false;
            return view.globalOrigin != previous->second || view.snapshot->fence.owner != latest->document.owner ||
                   view.snapshot->fence.generation != latest->document.generation;
        }
    }  // namespace

    /** @copydoc BuildWorldStreamingPreviewGeometry */
    Result<std::shared_ptr<const WorldStreamingPreviewGeometry>> BuildWorldStreamingPreviewGeometry(
        std::shared_ptr<const WorldStreamingPreview> preview, const WorldViewportBinding &view, const std::size_t maximumBoxes,
        const std::stop_token cancellation) {
        if (!preview || !view.snapshot || view.viewRevision == 0)
            return Result<Geometry>::Failure(MakeError(WorldDocumentErrors::Invalid));
        if (preview->Source() != view.snapshot || preview->Fence().viewRevision != view.viewRevision)
            return Result<Geometry>::Failure(MakeError(WorldDocumentErrors::Stale));
        if (maximumBoxes == 0 || maximumBoxes > 256)
            return Result<Geometry>::Failure(MakeError(WorldDocumentErrors::Capacity));
        auto geometry = std::make_shared<WorldStreamingPreviewGeometry>();
        geometry->preview = std::move(preview);
        geometry->globalOrigin = view.globalOrigin;
        geometry->viewRevision = view.viewRevision;
        const auto filled = FillGeometry(*geometry, maximumBoxes, cancellation);
        if (filled.HasError())
            return Result<Geometry>::Failure(filled.ErrorValue());
        return Result<Geometry>::Success(std::move(geometry));
    }

    /** @copydoc WorldStreamingPreviewHost::IssueMountIdentity */
    Result<WS::StreamingRuntimeOwnerToken> WorldStreamingPreviewHost::IssueMountIdentity(const WS::WorldPartitionId partition) {
        using Output = WS::StreamingRuntimeOwnerToken;
        if (std::this_thread::get_id() != thread_)
            return Result<Output>::Failure(MakeError(WorldDocumentErrors::WrongThread));
        if (!partition.IsValid())
            return Result<Output>::Failure(MakeError(WorldDocumentErrors::Invalid));
        if (identities_.size() >= 512)
            return Result<Output>::Failure(MakeError(WorldDocumentErrors::Capacity));
        std::uint64_t epoch{};
        for (const auto &identity : identities_)
            if (identity.partition == partition)
                epoch = std::max(epoch, identity.epoch.Value());
        while (nextOwner_ != 0 && std::ranges::any_of(identities_, [&](const auto &value) {
            return value.owner.Value() == nextOwner_;
        }))
            ++nextOwner_;
        if (nextOwner_ == 0 || epoch == std::numeric_limits<std::uint64_t>::max())
            return Result<Output>::Failure(MakeError(WorldDocumentErrors::Exhausted));
        const Output owner{partition, WS::PartitionEpoch::Create(epoch + 1).Value(),
                           WS::StreamingRuntimeOwnerId::Create(nextOwner_).Value()};
        identities_.push_back(owner);
        ++nextOwner_;
        return Result<Output>::Success(owner);
    }

    /** @copydoc WorldStreamingPreviewHost::BindRuntime */
    Result<void> WorldStreamingPreviewHost::BindRuntime(const WS::WorldPartitionRegistry &registry,
                                                        const WS::StreamingSourcePlanner &planner,
                                                        const WS::WorldStreamingDiagnosticSnapshot &diagnostics) {
        if (std::this_thread::get_id() != thread_)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::WrongThread));
        const auto snapshot = registry.Snapshot();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        const auto plan = planner.Plan();
        if (plan.HasError())
            return Result<void>::Failure(plan.ErrorValue());
        const auto captured = ValidateNativeCapture(snapshot.Value(), plan.Value(), diagnostics);
        if (captured.HasError())
            return captured;
        if (registry_ && owner_ != diagnostics.Owner())
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Conflict));
        if (std::ranges::find(identities_, diagnostics.Owner()) == identities_.end()) {
            if (identities_.size() >= 512)
                return Result<void>::Failure(MakeError(WorldDocumentErrors::Capacity));
            identities_.push_back(diagnostics.Owner());
        }
        registry_ = &registry;
        planner_ = &planner;
        diagnostics_ = &diagnostics;
        owner_ = diagnostics.Owner();
        cached_.reset();
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingPreviewHost::UnbindRuntime */
    Result<void> WorldStreamingPreviewHost::UnbindRuntime(const WS::StreamingRuntimeOwnerToken owner) {
        if (std::this_thread::get_id() != thread_)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::WrongThread));
        if (!registry_ || owner != owner_)
            return Result<void>::Failure(MakeError(WorldDocumentErrors::Stale));
        registry_ = nullptr;
        planner_ = nullptr;
        diagnostics_ = nullptr;
        owner_ = {};
        cached_.reset();
        latest_.reset();
        latestView_.reset();
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingPreviewHost::ClearPresentation */
    void WorldStreamingPreviewHost::ClearPresentation() noexcept {
        cached_.reset();
        latestView_.reset();
    }

    /** @copydoc WorldStreamingPreviewHost::Refresh */
    Result<std::shared_ptr<const WorldStreamingPreviewGeometry>> WorldStreamingPreviewHost::Refresh(const WorldViewportBinding &view) {
        if (std::this_thread::get_id() != thread_)
            return Result<Geometry>::Failure(MakeError(WorldDocumentErrors::WrongThread));
        if (!registry_ || !view.snapshot)
            return CaptureFailure(MakeError(WorldDocumentErrors::Lifecycle));
        const auto registry = registry_->Snapshot();
        if (registry.HasError())
            return CaptureFailure(registry.ErrorValue());
        const auto plan = planner_->Plan();
        if (plan.HasError())
            return CaptureFailure(plan.ErrorValue());
        const auto captured = ValidateNativeCapture(registry.Value(), plan.Value(), *diagnostics_);
        if (captured.HasError())
            return CaptureFailure(captured.ErrorValue());
        return Project(view, registry.Value(), plan.Value());
    }

    /** @copydoc WorldStreamingPreviewHost::CaptureFailure */
    Result<std::shared_ptr<const WorldStreamingPreviewGeometry>> WorldStreamingPreviewHost::CaptureFailure(const Error &error) {
        cached_.reset();
        return Result<Geometry>::Failure(error);
    }

    /** @copydoc WorldStreamingPreviewHost::Project */
    Result<std::shared_ptr<const WorldStreamingPreviewGeometry>> WorldStreamingPreviewHost::Project(
        const WorldViewportBinding &view, const WS::WorldPartitionRegistrySnapshot &registry, const WS::StreamingSourcePlanView &plan) {
        const WorldStreamingPreviewFence fence{view.snapshot->fence, registry.Binding(), diagnostics_->Revision(), plan.revision,
                                               view.viewRevision};
        if (CurrentGeometry(cached_, view, fence))
            return Result<Geometry>::Success(cached_);
        if (Regressed(latest_, fence) || RegressedView(latestView_, latest_, view))
            return CaptureFailure(MakeError(WorldDocumentErrors::Stale));
        auto preview = WorldStreamingPreview::Create(view.snapshot, registry, plan, *diagnostics_, fence);
        if (preview.HasError())
            return CaptureFailure(preview.ErrorValue());
        auto geometry = BuildWorldStreamingPreviewGeometry(std::make_shared<const WorldStreamingPreview>(std::move(preview).Value()), view);
        if (geometry.HasError())
            return CaptureFailure(geometry.ErrorValue());
        latest_ = fence;
        latestView_ = std::pair{view.viewRevision, view.globalOrigin};
        cached_ = geometry.Value();
        return geometry;
    }
}  // namespace Horo::Editor
