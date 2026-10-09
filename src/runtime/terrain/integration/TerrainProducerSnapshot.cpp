#include "TerrainProducerSnapshotInternal.h"

#include <algorithm>

namespace Horo::Terrain {
    namespace TerrainProducerErrors {
        namespace {
            const ErrorDomainId Domain{"horo.terrain.producer"};
        }

        const ErrorCodeDescriptor Invalid{Domain, ErrorCode{"terrain.integration.snapshot_incompatible"}, ErrorSeverity::Error,
                                          "Terrain producer snapshot is incompatible.", "Check exact source membership and provenance."};
        const ErrorCodeDescriptor Unavailable{Domain, ErrorCode{"terrain.integration.consumer_unavailable"}, ErrorSeverity::Error,
                                              "Terrain consumer capability is unavailable.", "Compose the exact required capability."};
        const ErrorCodeDescriptor Limit{Domain, ErrorCode{"terrain.integration.snapshot_limit"}, ErrorSeverity::Error,
                                        "Terrain producer snapshot exceeds its reservation.", "Reserve a bounded complete selection."};
        const ErrorCodeDescriptor Cancelled{Domain, ErrorCode{"terrain.integration.snapshot_cancelled"}, ErrorSeverity::Warning,
                                            "Terrain producer snapshot was cancelled.", "Capture a new admitted attempt."};
        const ErrorCodeDescriptor Stale{Domain, ErrorCode{"terrain.integration.snapshot_stale"}, ErrorSeverity::Error,
                                        "Terrain producer snapshot evidence is stale.", "Capture the current complete generation."};
        const ErrorCodeDescriptor Closed{Domain, ErrorCode{"terrain.integration.snapshot_closed"}, ErrorSeverity::Error,
                                         "Terrain producer admission is closed.", "Create a new host-owned session."};
    }  // namespace TerrainProducerErrors

    struct TerrainProducerSnapshot::State final {
        TerrainProducerSnapshotHeader header;
        std::vector<TerrainProducerMesh> meshes;
        std::vector<TerrainProducerFoliage> foliage;
        std::uint64_t ownedBytes{};
    };

    namespace ProducerDetail {
        /** @copydoc Failure */
        Error Failure(const TerrainProducerSnapshotRequest &request, const ErrorCodeDescriptor &code) {
            return MakeError(code,
                             request.header.consumer == TerrainProducerConsumer::Navigation ? "navigation producer" : "collision producer");
        }

        /** @copydoc Charge */
        bool Charge(std::uint64_t &used, const std::uint64_t amount, const std::uint64_t maximum) noexcept {
            if (used > maximum || amount > maximum - used)
                return false;
            used += amount;
            return true;
        }

        /** @copydoc Budget::Work */
        bool Budget::Work(const std::uint64_t count) {
            return Charge(work, count, limits.maximumWorkItems);
        }

        /** @copydoc Budget::Bytes */
        bool Budget::Bytes(const std::uint64_t count) {
            return Charge(bytes, count, limits.maximumOwnedBytes);
        }

        /** @copydoc Budget::Step */
        Result<void> Budget::Step(const TerrainProducerSnapshotRequest &request, const CancellationToken &cancellation,
                                  const std::uint64_t count) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Cancelled));
            if (!Work(count))
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Limit));
            return Result<void>::Success();
        }

        /** @copydoc VerifyPayload */
        Result<void> VerifyPayload(const TerrainProducerSnapshotRequest &request, const std::span<const std::uint8_t> payload,
                                   const Sha256Digest &digest, Budget &budget, const CancellationToken &cancellation) {
            Sha256Builder hash;
            for (std::size_t offset = 0; offset < payload.size();) {
                auto step = budget.Step(request, cancellation);
                if (!step.HasValue())
                    return step;
                const auto count = std::min<std::size_t>(4096, payload.size() - offset);
                if (!hash.Update(std::as_bytes(payload.subspan(offset, count))))
                    return Result<void>::Failure(Failure(request, TerrainProducerErrors::Invalid));
                offset += count;
            }
            if (payload.empty() || digest == Sha256Digest{} || hash.Finalize() != digest)
                return Result<void>::Failure(Failure(request, TerrainProducerErrors::Invalid));
            return Result<void>::Success();
        }
    }  // namespace ProducerDetail

    using namespace ProducerDetail;

    /** @copydoc CaptureTerrainProducerSnapshot */
    Result<TerrainProducerSnapshot> CaptureTerrainProducerSnapshot(const TerrainProducerSnapshotRequest &request,
                                                                   const CancellationToken &cancellation) {
        auto validation = ValidateCaptureRequest(request, cancellation);
        if (!validation.HasValue())
            return Result<TerrainProducerSnapshot>::Failure(validation.ErrorValue());
        Budget budget{request.limits};
        validation = VerifyManifest(request, budget, cancellation);
        if (!validation.HasValue())
            return Result<TerrainProducerSnapshot>::Failure(validation.ErrorValue());
        const auto scratchBytes = (request.tiles.size() + request.clusters.size()) * sizeof(void *);
        if (!budget.Bytes(sizeof(TerrainProducerSnapshot::State) + scratchBytes + request.tiles.size() * sizeof(TerrainProducerMesh)))
            return Result<TerrainProducerSnapshot>::Failure(Failure(request, TerrainProducerErrors::Limit));
        auto meshes = SelectMeshes(request, budget, cancellation);
        if (!meshes.HasValue())
            return Result<TerrainProducerSnapshot>::Failure(meshes.ErrorValue());
        auto clusters = SelectClusters(request, budget, cancellation);
        if (!clusters.HasValue())
            return Result<TerrainProducerSnapshot>::Failure(clusters.ErrorValue());
        auto count = AdmitFoliage(request, clusters.Value(), budget, cancellation);
        if (!count.HasValue())
            return Result<TerrainProducerSnapshot>::Failure(count.ErrorValue());
        auto state = std::make_shared<TerrainProducerSnapshot::State>();
        state->header = request.header;
        state->meshes.reserve(meshes.Value().size());
        state->foliage.reserve(static_cast<std::size_t>(count.Value()));
        validation = CopyMeshes(request, meshes.Value(), state->meshes, budget, cancellation);
        if (!validation.HasValue())
            return Result<TerrainProducerSnapshot>::Failure(validation.ErrorValue());
        validation = CopyFoliage(request, clusters.Value(), state->foliage, budget, cancellation);
        if (!validation.HasValue())
            return Result<TerrainProducerSnapshot>::Failure(validation.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<TerrainProducerSnapshot>::Failure(Failure(request, TerrainProducerErrors::Cancelled));
        state->ownedBytes = budget.bytes - scratchBytes;
        return Result<TerrainProducerSnapshot>::Success(TerrainProducerSnapshot{std::move(state)});
    }

    /** @copydoc TerrainProducerSnapshot::IsValid */
    bool TerrainProducerSnapshot::IsValid() const noexcept {
        return static_cast<bool>(state_);
    }

    /** @copydoc TerrainProducerSnapshot::Header */
    const TerrainProducerSnapshotHeader &TerrainProducerSnapshot::Header() const noexcept {
        static const TerrainProducerSnapshotHeader invalid{};
        return state_ ? state_->header : invalid;
    }

    /** @copydoc TerrainProducerSnapshot::Meshes */
    std::span<const TerrainProducerMesh> TerrainProducerSnapshot::Meshes() const noexcept {
        return state_ ? std::span<const TerrainProducerMesh>{state_->meshes} : std::span<const TerrainProducerMesh>{};
    }

    /** @copydoc TerrainProducerSnapshot::Foliage */
    std::span<const TerrainProducerFoliage> TerrainProducerSnapshot::Foliage() const noexcept {
        return state_ ? std::span<const TerrainProducerFoliage>{state_->foliage} : std::span<const TerrainProducerFoliage>{};
    }

    /** @copydoc TerrainProducerSnapshot::OwnedBytes */
    std::uint64_t TerrainProducerSnapshot::OwnedBytes() const noexcept {
        return state_ ? state_->ownedBytes : 0;
    }

    /** @copydoc TerrainProducerSnapshot::ValidateCurrent */
    Result<void> TerrainProducerSnapshot::ValidateCurrent(const TerrainProducerSnapshotHeader &current) const {
        if (!IsValid() || Header() != current)
            return Result<void>::Failure(MakeError(TerrainProducerErrors::Stale));
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain
