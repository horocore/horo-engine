#pragma once

#include "Horo/Terrain/TerrainProducerSnapshot.h"

#include <algorithm>
#include <bit>
#include <functional>

namespace Horo::Terrain::ProducerDetail {
    /** @brief Adds bounded consumer identity to a typed failure without dumping payloads. */
    Error Failure(const TerrainProducerSnapshotRequest &request, const ErrorCodeDescriptor &code);

    /** @brief Overflow-safe admission; updates the counter only when the complete addition fits. */
    bool Charge(std::uint64_t &used, std::uint64_t amount, std::uint64_t maximum) noexcept;

    /** @brief Invocation-local accounting shared by membership and projection; owns no inputs or publication authority. */
    struct Budget final {
        const TerrainProducerSnapshotLimits &limits;
        std::uint64_t bytes{}, work{}, vertices{}, triangles{}, instances{};

        /** @brief Charges examined work; zero-sized selections remain bounded. */
        bool Work(std::uint64_t count);
        /** @brief Charges neutral storage before allocating it. */
        bool Bytes(std::uint64_t count);
        /** @brief Checks cancellation before charging work, preserving cancellation-over-limit precedence.
         * @param request Borrowed invocation for error context.
         * @param cancellation Host-owned cancellation observer.
         * @param count Number of examined items; one by default.
         * @return Success or exact contextual cancelled/limit failure.
         */
        Result<void> Step(const TerrainProducerSnapshotRequest &request, const CancellationToken &cancellation, std::uint64_t count = 1);
    };

    /** @brief Validates lifecycle, request, capability and exact manifest/root provenance before dereferencing selected inputs. */
    Result<void> ValidateCaptureRequest(const TerrainProducerSnapshotRequest &request, const CancellationToken &cancellation);
    /** @brief Verifies authoritative manifest bytes within the invocation's work ceiling. */
    Result<void> VerifyManifest(const TerrainProducerSnapshotRequest &request, Budget &budget, const CancellationToken &cancellation);
    /** @brief Verifies selected payload hashes in cancellable at-most-4-KiB chunks without a byte copy. */
    Result<void> VerifyPayload(const TerrainProducerSnapshotRequest &request, std::span<const std::uint8_t> payload,
                               const Sha256Digest &digest, Budget &budget, const CancellationToken &cancellation);

    /** @brief Traverses one cook-issued collection, charging every examined member and retaining only invocation-local borrows.
     * @param members Exact immutable cook-issued collection.
     * @param request Invocation for error identity.
     * @param budget Shared admission counters.
     * @param cancellation Host observer.
     * @param matches Pure exact identity predicate.
     * @return Borrowed exact member or contextual cancellation/limit/invalid failure.
     */
    template <typename Member, typename Predicate>
    Result<const Member *> FindMember(std::span<const Member> members, const TerrainProducerSnapshotRequest &request, Budget &budget,
                                      const CancellationToken &cancellation, Predicate matches) {
        for (const auto &member : members) {
            auto step = budget.Step(request, cancellation);
            if (!step.HasValue())
                return Result<const Member *>::Failure(step.ErrorValue());
            if (matches(member))
                return Result<const Member *>::Success(&member);
        }
        return Result<const Member *>::Failure(Failure(request, TerrainProducerErrors::Invalid));
    }

    /** @brief Shared bounded canonical-selection algorithm for the two concrete cook member types.
     * @param request Invocation owning every borrow until capture completes.
     * @param selections Exact host-admitted selections.
     * @param budget Shared counters; index storage is reserved by capture before this call.
     * @param cancellation Host observer, checked through resolution and before/after bounded sorting.
     * @param resolve Exact manifest-bound resolver, never native conversion or publication.
     * @param order Canonical ordering key.
     * @param identity Duplicate identity key.
     * @return Canonical invocation-local index or contextual failure; allocates only its precharged index.
     */
    template <typename Selection, typename Member, typename Order, typename Identity>
    Result<std::vector<const Member *>> SelectMembers(const TerrainProducerSnapshotRequest &request, std::span<const Selection> selections,
                                                      Budget &budget, const CancellationToken &cancellation,
                                                      Result<const Member *> (*resolve)(const TerrainProducerSnapshotRequest &,
                                                                                        const Selection &, Budget &,
                                                                                        const CancellationToken &),
                                                      Order order, Identity identity) {
        using Selected = Result<std::vector<const Member *>>;
        std::vector<const Member *> selected;
        selected.reserve(selections.size());
        for (const auto &selection : selections) {
            auto member = resolve(request, selection, budget, cancellation);
            if (!member.HasValue())
                return Selected::Failure(member.ErrorValue());
            selected.push_back(member.Value());
        }
        auto admitted = budget.Step(request, cancellation, selected.size() * (std::bit_width(selected.size()) + 1));
        if (!admitted.HasValue())
            return Selected::Failure(admitted.ErrorValue());
        std::ranges::sort(selected, {}, order);
        for (std::size_t index = 1; index < selected.size(); ++index)
            if (std::invoke(identity, *selected[index - 1]) == std::invoke(identity, *selected[index]))
                return Selected::Failure(Failure(request, TerrainProducerErrors::Invalid));
        if (cancellation.IsCancellationRequested())
            return Selected::Failure(Failure(request, TerrainProducerErrors::Cancelled));
        return Selected::Success(std::move(selected));
    }

    /** @brief Returns exact role-specific manifest tile members in canonical LOD/Z/X order. */
    Result<std::vector<const TerrainSourceArtifact *>> SelectMeshes(const TerrainProducerSnapshotRequest &request, Budget &budget,
                                                                    const CancellationToken &cancellation);
    /** @brief Returns exact manifest cluster members in canonical tile/type/cluster order. */
    Result<std::vector<const CookedFoliageCluster *>> SelectClusters(const TerrainProducerSnapshotRequest &request, Budget &budget,
                                                                     const CancellationToken &cancellation);
    /** @brief Admits, verifies and copies every selected canonical mesh without returning partial state. */
    Result<void> CopyMeshes(const TerrainProducerSnapshotRequest &request, std::span<const TerrainSourceArtifact *const> meshes,
                            std::vector<TerrainProducerMesh> &output, Budget &budget, const CancellationToken &cancellation);
    /** @brief Validates every selected foliage instance and reserves output storage before projection. */
    Result<std::uint64_t> AdmitFoliage(const TerrainProducerSnapshotRequest &request, std::span<const CookedFoliageCluster *const> clusters,
                                       Budget &budget, const CancellationToken &cancellation);
    /** @brief Copies exact authored primitives and integer transforms without consumer-native realization. */
    Result<void> CopyFoliage(const TerrainProducerSnapshotRequest &request, std::span<const CookedFoliageCluster *const> clusters,
                             std::vector<TerrainProducerFoliage> &output, Budget &budget, const CancellationToken &cancellation);
}  // namespace Horo::Terrain::ProducerDetail
