#include "FoliageClusterCookInternal.h"
#include "Horo/Terrain/FoliageClusterCook.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace Horo::Terrain {
    namespace FoliageClusterCookErrors {
        namespace {
            const ErrorDomainId Domain{"horo.terrain.foliage.cluster"};
        }

        const ErrorCodeDescriptor InvalidInput{Domain, ErrorCode{"terrain.foliage.cluster_invalid"}, ErrorSeverity::Error,
                                               "Foliage cluster evidence is invalid.",
                                               "Check exact placement, geometry and target provenance."};
        const ErrorCodeDescriptor LimitExceeded{Domain, ErrorCode{"terrain.foliage.cluster_limit_exceeded"}, ErrorSeverity::Error,
                                                "Foliage clusters exceed a finite limit.",
                                                "Reduce required content or select an explicitly compatible profile."};
        const ErrorCodeDescriptor Cancelled{Domain, ErrorCode{"terrain.foliage.cluster_cancelled"}, ErrorSeverity::Warning,
                                            "Foliage cluster work was cancelled.", "Retry against current source evidence."};
        const ErrorCodeDescriptor Stale{Domain, ErrorCode{"terrain.foliage.cluster_stale"}, ErrorSeverity::Error,
                                        "Foliage cluster replacement is stale.", "Prepare the exact successor of the current generation."};
        const ErrorCodeDescriptor Closed{Domain, ErrorCode{"terrain.foliage.cluster_closed"}, ErrorSeverity::Error,
                                         "Foliage cluster admission is closed.", "Create a new owner for the next session."};
    }  // namespace FoliageClusterCookErrors

    namespace {
        /** @brief Requires the exact non-wrapping same-dataset successor, not merely a newer revision. */
        [[nodiscard]] bool IsSuccessor(const CookedFoliageClusterSet &current, const CookedFoliageClusterSet &candidate) {
            return candidate.Dataset() == current.Dataset() &&
                   current.ContentRevision().Value() != std::numeric_limits<std::uint64_t>::max() &&
                   candidate.ContentRevision().Value() == current.ContentRevision().Value() + 1;
        }

        /** @brief Matches explicit insertion/replacement expectations before dereferencing optional revision evidence. */
        [[nodiscard]] bool MatchesPublication(const CookedFoliageClusterSet *current, const CookedFoliageClusterSet &candidate,
                                              const std::optional<TerrainContentRevision> expected) {
            if (!current)
                return !expected.has_value();
            return expected.has_value() && current->ContentRevision() == *expected && IsSuccessor(*current, candidate);
        }
    }  // namespace

    /** @copydoc VerifyFoliageClusterPayload */
    Result<void> VerifyFoliageClusterPayload(const CookedFoliageCluster &expected, const std::span<const std::uint8_t> payload,
                                             const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(FoliageClusterCookErrors::Cancelled));
        if (payload.size() != expected.payload.size())
            return Result<void>::Failure(MakeError(FoliageClusterCookErrors::InvalidInput));
        Sha256Builder hash;
        for (std::size_t offset = 0; offset < payload.size();) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(FoliageClusterCookErrors::Cancelled));
            const auto count = std::min<std::size_t>(4'096, payload.size() - offset);
            static_cast<void>(hash.Update(std::as_bytes(payload.subspan(offset, count))));
            offset += count;
        }
        if (hash.Finalize() != expected.digest)
            return Result<void>::Failure(MakeError(FoliageClusterCookErrors::InvalidInput));
        return Result<void>::Success();
    }

    /** @brief Revalidates a bounded cook-issued generation, including moved-from values, before replacement/publication. */
    bool CookedFoliageClusterSet::IsWellFormed(const CancellationToken &cancellation) const noexcept {
        if (!dataset_.IsValid() || !content_.IsValid() || clusters_.size() != footprint_.activeFoliageClusters)
            return false;
        const auto profile = FoliageClusterCookInternal::ProfileFingerprint(profile_);
        std::uint64_t instances{};
        for (std::size_t index = 0; index < clusters_.size(); ++index) {
            const auto &cluster = clusters_[index];
            if (!FoliageClusterCookInternal::ValidateCluster(cluster, dataset_, profile_.configuration.Data().capability, profile,
                                                             cancellation))
                return false;
            if (index != 0 && std::tuple{clusters_[index - 1].tile, clusters_[index - 1].type, clusters_[index - 1].id} >=
                                  std::tuple{cluster.tile, cluster.type, cluster.id})
                return false;
            instances += cluster.instances.size();
        }
        return instances == footprint_.activeFoliageInstances &&
               manifestDigest_ == FoliageClusterCookInternal::ManifestFingerprint(dataset_, content_, fingerprint_, clusters_);
    }

    /** @copydoc FoliageClusterCookOwner::Publish */
    Result<void> FoliageClusterCookOwner::Publish(CookedFoliageClusterSet candidate,
                                                  const std::optional<TerrainContentRevision> expectedCurrent,
                                                  const CancellationToken &cancellation) {
        if (closed_)
            return Result<void>::Failure(MakeError(FoliageClusterCookErrors::Closed));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(FoliageClusterCookErrors::Cancelled));
        if (!MatchesPublication(Current(), candidate, expectedCurrent))
            return Result<void>::Failure(MakeError(FoliageClusterCookErrors::Stale));
        if (!candidate.IsWellFormed(cancellation))
            return Result<void>::Failure(MakeError(cancellation.IsCancellationRequested() ? FoliageClusterCookErrors::Cancelled
                                                                                          : FoliageClusterCookErrors::InvalidInput));
        if (current_ && current_->Footprint().residentFoliageBytes > candidate.Profile().configuration.Data().limits.maximumRetiringBytes)
            return Result<void>::Failure(MakeError(FoliageClusterCookErrors::LimitExceeded));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(FoliageClusterCookErrors::Cancelled));
        current_ = std::move(candidate);
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain
