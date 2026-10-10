#include "Horo/TerrainRenderIntegration/TerrainTileResourceOwner.h"

#include "Horo/TerrainRender/TerrainMaterialBinding.h"

#include <cassert>
#include <limits>
#include <stdexcept>

namespace Horo::TerrainRender {
    namespace {
        /** @brief Recognizes exact failed creations whose automatic retirement belongs to the canonical frontend. */
        template <typename Handle>
        bool FailedCreationRetired(Render::RenderFrontend &frontend, Render::ResourceCreation<Handle> &creation) {
            const auto state = frontend.ResourceState(creation.handle);
            if (state.HasValue() && state.Value() != Render::RenderResourceState::Failed)
                return false;
            const auto operation = frontend.ResourceOperationState(creation.operation);
            if (operation.HasError() || operation.Value() != Render::RenderResourceOperationState::Failed)
                return false;
            creation = {};
            return true;
        }

        /** @brief Enforces monotonic semantic revisions and strictly newer request generations across replacement. */
        bool Successor(const TerrainRenderSnapshotHeader &next, const TerrainRenderSnapshotHeader &old) noexcept {
            return next.generation > old.generation && next.terrain.dataset == old.terrain.dataset && next.world == old.world &&
                   next.origin.identity == old.origin.identity && next.origin.revision >= old.origin.revision &&
                   next.origin.generation >= old.origin.generation && next.revisions.content >= old.revisions.content &&
                   next.revisions.residency >= old.revisions.residency && next.revisions.mutation >= old.revisions.mutation &&
                   next.revisions.capability >= old.revisions.capability && next.configuration >= old.configuration;
        }

        /** @brief Queues a complete generation's owned uploads while retaining every accepted handle on failure. */
        Result<void> QueueBuffers(Render::RenderFrontend &frontend, TerrainTileResourceGeneration &generation,
                                  const CancellationToken &cancellation) {
            for (std::size_t i = 0; i < generation.tiles.size(); ++i) {
                const auto &candidate = generation.source.Tiles()[i];
                auto &resources = generation.tiles[i];
                resources.tile = candidate.tile;
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Terrain::TerrainErrors::CompositionCancelled));
                if (candidate.mesh.indices.empty())
                    continue;
                const auto vertexBytes = std::as_bytes(std::span{candidate.mesh.vertices});
                auto vertices = frontend.CreateBuffer({vertexBytes.size(),
                                                       Render::RenderBufferUsage::Vertex | Render::RenderBufferUsage::CopyDestination},
                                                      vertexBytes);
                if (vertices.HasError())
                    return Result<void>::Failure(vertices.ErrorValue());
                resources.vertices = vertices.Value();
                const auto indexBytes = std::as_bytes(std::span{candidate.mesh.indices});
                auto indices = frontend.CreateBuffer({indexBytes.size(),
                                                      Render::RenderBufferUsage::Index | Render::RenderBufferUsage::CopyDestination},
                                                     indexBytes);
                if (indices.HasError())
                    return Result<void>::Failure(indices.ErrorValue());
                resources.indices = indices.Value();
            }
            return Result<void>::Success();
        }

        /** @brief Observes readiness while retaining exact backend diagnostics for auto-retired failed creations. */
        template <typename Handle>
        Result<bool> ObserveCreation(Render::RenderFrontend &frontend, const Render::ResourceCreation<Handle> &creation) {
            const auto state = frontend.ResourceState(creation.handle);
            if (state.HasError() || state.Value() == Render::RenderResourceState::Failed) {
                const auto operation = frontend.ResourceOperationResult(creation.operation);
                if (operation.HasError())
                    return Result<bool>::Failure(operation.ErrorValue());
                return Result<bool>::Failure(state.HasError() ? state.ErrorValue() : MakeError(Terrain::TerrainErrors::DescriptorInvalid));
            }
            return Result<bool>::Success(state.Value() == Render::RenderResourceState::Ready);
        }

        /** @brief Advances one tile from ready buffers to a mesh without publishing any generation. */
        Result<bool> AdvanceTile(Render::RenderFrontend &frontend, TerrainTileRenderResources &resource, const Render::MeshData &mesh) {
            if (mesh.indices.empty())
                return Result<bool>::Success(true);
            const auto vertices = ObserveCreation(frontend, resource.vertices);
            const auto indices = ObserveCreation(frontend, resource.indices);
            if (vertices.HasError() || indices.HasError())
                return Result<bool>::Failure(vertices.HasError() ? vertices.ErrorValue() : indices.ErrorValue());
            if (!vertices.Value() || !indices.Value())
                return Result<bool>::Success(false);
            if (!resource.mesh.handle.IsValid()) {
                auto created = frontend.CreateMesh({resource.vertices.handle, resource.indices.handle, sizeof(Render::MeshVertex),
                                                    static_cast<std::uint32_t>(mesh.vertices.size()), Render::RenderIndexFormat::UInt32,
                                                    static_cast<std::uint32_t>(mesh.indices.size()),
                                                    Render::RenderPrimitiveTopology::Triangles, mesh.localBounds});
                if (created.HasError())
                    return Result<bool>::Failure(created.ErrorValue());
                resource.mesh = created.Value();
                return Result<bool>::Success(false);
            }
            return ObserveCreation(frontend, resource.mesh);
        }
    }  // namespace

    /** @copydoc TerrainTileResourceOwner::TerrainTileResourceOwner */
    TerrainTileResourceOwner::TerrainTileResourceOwner(Render::RenderFrontend &frontend, const std::uint32_t maximumRetiring)
        : frontend_(frontend), owner_(std::this_thread::get_id()), maximumRetiring_(maximumRetiring) {
        if (maximumRetiring == 0 || maximumRetiring > 16)
            throw std::invalid_argument("Terrain resource retirement ceiling must be one to sixteen.");
        retiring_.reserve(static_cast<std::size_t>(maximumRetiring) + 2);
    }

    /** @copydoc TerrainTileResourceOwner::~TerrainTileResourceOwner */
    TerrainTileResourceOwner::~TerrainTileResourceOwner() {
        assert(IsDrained());
    }

    /** @brief Guards every mutating/frontend call with the explicitly composed render lane. */
    Result<void> TerrainTileResourceOwner::CheckOwner() const {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeError(TerrainMaterialBindingErrors::WrongThread));
        return Result<void>::Success();
    }

    /** @brief Transfers detached queued allocations to preallocated retirement metadata without losing ownership. */
    void TerrainTileResourceOwner::DiscardPending() noexcept {
        if (pending_)
            retiring_.push_back(std::move(pending_));
    }

    /** @copydoc TerrainTileResourceOwner::Stage */
    Result<void> TerrainTileResourceOwner::Stage(TerrainRenderExtractionSnapshot source, const TerrainRenderSnapshotHeader &current,
                                                 const std::uint64_t expectedPublication, const CancellationToken &cancellation) {
        if (const auto lane = CheckOwner(); lane.HasError())
            return lane;
        if (closed_)
            return Result<void>::Failure(MakeError(Terrain::TerrainErrors::LifecycleUnavailable));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(Terrain::TerrainErrors::CompositionCancelled));
        if (const auto valid = source.ValidateCurrent(current); valid.HasError())
            return valid;
        if (pending_ || retiring_.size() >= maximumRetiring_ || source.Tiles().size() > 256)
            return Result<void>::Failure(MakeError(Terrain::TerrainErrors::CapacityExceeded));
        if (expectedPublication != (current_ ? lastPublication_ : 0) || (current_ && !Successor(current, current_->source.Header())))
            return Result<void>::Failure(MakeError(Terrain::TerrainErrors::RevisionStale));
        if (lastPublication_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Terrain::TerrainErrors::GenerationExhausted));
        auto detached =
            std::make_shared<TerrainTileResourceGeneration>(TerrainTileResourceGeneration{std::move(source), {}, lastPublication_ + 1});
        detached->tiles.resize(detached->source.Tiles().size());
        pending_ = std::move(detached);
        const auto queued = QueueBuffers(frontend_, *pending_, cancellation);
        if (queued.HasError())
            DiscardPending();
        return queued;
    }

    /** @copydoc TerrainTileResourceOwner::Advance */
    Result<bool> TerrainTileResourceOwner::Advance(const TerrainRenderSnapshotHeader &current, const CancellationToken &cancellation) {
        using Progress = Result<bool>;
        if (const auto lane = CheckOwner(); lane.HasError())
            return Progress::Failure(lane.ErrorValue());
        if (closed_)
            return Progress::Failure(MakeError(Terrain::TerrainErrors::LifecycleUnavailable));
        if (!pending_)
            return Progress::Failure(MakeError(Terrain::TerrainErrors::DescriptorInvalid));
        if (cancellation.IsCancellationRequested()) {
            DiscardPending();
            return Progress::Failure(MakeError(Terrain::TerrainErrors::CompositionCancelled));
        }
        if (const auto valid = pending_->source.ValidateCurrent(current); valid.HasError()) {
            DiscardPending();
            return Progress::Failure(valid.ErrorValue());
        }
        bool complete = true;
        for (std::size_t i = 0; i < pending_->tiles.size(); ++i) {
            const auto ready = AdvanceTile(frontend_, pending_->tiles[i], pending_->source.Tiles()[i].mesh);
            if (ready.HasError()) {
                DiscardPending();
                return Progress::Failure(ready.ErrorValue());
            }
            complete = complete && ready.Value();
        }
        if (!complete)
            return Progress::Success(false);
        if (cancellation.IsCancellationRequested()) {
            DiscardPending();
            return Progress::Failure(MakeError(Terrain::TerrainErrors::CompositionCancelled));
        }
        if (current_)
            retiring_.push_back(std::move(current_));
        current_ = std::move(pending_);
        ++lastPublication_;
        return Progress::Success(true);
    }

    /** @copydoc TerrainTileResourceOwner::Snapshot */
    Result<std::shared_ptr<const TerrainTileResourceGeneration>> TerrainTileResourceOwner::Snapshot() const {
        using Read = Result<std::shared_ptr<const TerrainTileResourceGeneration>>;
        if (const auto lane = CheckOwner(); lane.HasError())
            return Read::Failure(lane.ErrorValue());
        if (closed_)
            return Read::Failure(MakeError(Terrain::TerrainErrors::LifecycleUnavailable));
        return Read::Success(current_);
    }

    /** @copydoc TerrainTileResourceOwner::BeginShutdown */
    Result<void> TerrainTileResourceOwner::BeginShutdown() {
        if (const auto lane = CheckOwner(); lane.HasError())
            return lane;
        if (closed_)
            return Result<void>::Success();
        closed_ = true;
        DiscardPending();
        if (current_)
            retiring_.push_back(std::move(current_));
        return Result<void>::Success();
    }

    /** @brief Releases mesh dependencies before buffers; accepted submission pins remain frontend-owned. */
    Result<void> TerrainTileResourceOwner::Release(TerrainTileResourceGeneration &generation) {
        for (auto &tile : generation.tiles) {
            if (tile.mesh.handle.IsValid() && !FailedCreationRetired(frontend_, tile.mesh)) {
                const auto released = frontend_.ReleaseMesh(tile.mesh.handle);
                if (released.HasError())
                    return released;
                tile.mesh = {};
            }
            for (auto *buffer : {&tile.vertices, &tile.indices}) {
                if (!buffer->handle.IsValid() || FailedCreationRetired(frontend_, *buffer))
                    continue;
                const auto released = frontend_.ReleaseBuffer(buffer->handle);
                if (released.HasError())
                    return released;
                *buffer = {};
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc TerrainTileResourceOwner::DrainRetired */
    Result<std::size_t> TerrainTileResourceOwner::DrainRetired() {
        if (const auto lane = CheckOwner(); lane.HasError())
            return Result<std::size_t>::Failure(lane.ErrorValue());
        std::size_t released = 0;
        for (auto i = retiring_.begin(); i != retiring_.end();) {
            if (i->use_count() != 1) {
                ++i;
                continue;
            }
            if (const auto retired = Release(**i); retired.HasError())
                return Result<std::size_t>::Failure(retired.ErrorValue());
            i = retiring_.erase(i);
            ++released;
        }
        return Result<std::size_t>::Success(released);
    }

    /** @copydoc TerrainTileResourceOwner::IsDrained */
    bool TerrainTileResourceOwner::IsDrained() const noexcept {
        return closed_ && !current_ && !pending_ && retiring_.empty();
    }
}  // namespace Horo::TerrainRender
