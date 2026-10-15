#include "WorldDependencyArtifactModel.h"

#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "WorldDependencyArtifactWire.h"

#include <algorithm>
#include <limits>
#include <tuple>

namespace Horo::WorldStreaming::Detail {
    namespace {
        /** @brief Rejects a dependency projection without exposing partial derived membership. */
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        /** @brief Requires an explicit ceiling for every retained table and derivation work class. */
        bool ValidLimits(const WorldDependencyArtifactLimits &limits) {
            return limits.maximumArtifactBytes && limits.maximumDecodedAllocationBytes && limits.maximumObjects && limits.maximumCells &&
                   limits.maximumCoverageCells && limits.maximumPlacementCells && limits.maximumBundleMembers &&
                   limits.maximumSoftReferences && limits.maximumHardCellEdges && limits.maximumDerivationSteps;
        }

        /** @brief Resolves exact page-scoped identity in a previously ordered object table. */
        std::size_t FindObject(const DecodedWorldDependencies &model, const WorldAuthoringObjectAddress &address) {
            const auto found = std::ranges::lower_bound(model.objects, address, {}, [](const auto &object) {
                return object.object.address;
            });
            return found != model.objects.end() && found->object.address == address
                       ? static_cast<std::size_t>(found - model.objects.begin())
                       : model.objects.size();
        }

        /** @brief Requires exact canonical, unique cell keys already admitted by the matching index. */
        Result<void> ValidateCells(const std::span<const StreamingCellId> cells, const DecodedWorldIndex &index,
                                   const CancellationToken &cancellation) {
            for (std::size_t position = 0; position < cells.size(); ++position) {
                if (cancellation.IsCancellationRequested())
                    return Reject<void>(WorldArtifactErrors::Cancelled);
                if ((position && !StreamingCellCanonicalLess{}(cells[position - 1], cells[position])) ||
                    !std::ranges::binary_search(index.cells, cells[position], StreamingCellCanonicalLess{},
                                                &CookedWorldCellManifestEntry::cell))
                    return Reject<void>(WorldArtifactErrors::Invalid);
            }
            return Result<void>::Success();
        }

        /** @brief Checks actual placement without silently dropping authored coverage or nonspatial content. */
        bool ValidPlacement(const WorldDependencyObjectProjection &object) {
            using enum WorldSpanningObjectPlacement;
            switch (object.placement) {
                case Direct:
                case SplitPerCell:
                    return object.coverage == object.cells;
                case SingleCellOwner:
                    return object.cells.size() == 1 &&
                           std::ranges::binary_search(object.coverage, object.cells.front(), StreamingCellCanonicalLess{});
                case NonSpatial:
                    return object.cells.empty();
            }
            return false;
        }

        /** @brief Checks canonical object identities, bounded placement counts and exact cell membership in order. */
        Result<void> ValidateObjectProjections(const DecodedWorldDependencies &model, const DecodedWorldIndex &index,
                                               const WorldDependencyArtifactLimits &limits, const CancellationToken &cancellation) {
            std::size_t coverage{};
            std::size_t placements{};
            for (std::size_t position = 0; position < model.objects.size(); ++position) {
                if (cancellation.IsCancellationRequested())
                    return Reject<void>(WorldArtifactErrors::Cancelled);
                const auto &object = model.objects[position];
                if (!object.object.IsValid() || object.coverage.empty() ||
                    (position && !(model.objects[position - 1].object.address < object.object.address)))
                    return Reject<void>(WorldArtifactErrors::Invalid);
                if (object.coverage.size() > limits.maximumCoverageCells - coverage ||
                    object.cells.size() > limits.maximumPlacementCells - placements)
                    return Reject<void>(WorldArtifactErrors::CapacityExceeded);
                coverage += object.coverage.size();
                placements += object.cells.size();
                if (const auto coverageValid = ValidateCells(object.coverage, index, cancellation); coverageValid.HasError())
                    return coverageValid;
                if (const auto placementValid = ValidateCells(object.cells, index, cancellation); placementValid.HasError())
                    return placementValid;
                if (!ValidPlacement(object))
                    return Reject<void>(WorldArtifactErrors::Invalid);
            }
            return Result<void>::Success();
        }

        /** @brief Admits all aggregate counts before graph scratch allocation and semantic traversal. */
        Result<void> ValidateObjects(const DecodedWorldDependencies &model, const DecodedWorldIndex &index,
                                     const WorldDependencyArtifactLimits &limits, const CancellationToken &cancellation) {
            if (!ValidLimits(limits) || model.objects.empty())
                return Reject<void>(WorldArtifactErrors::Invalid);
            if (model.partition != index.descriptor.Partition() || model.indexArtifactHash != index.artifactHash)
                return Reject<void>(WorldArtifactErrors::Stale);
            if (index.artifactHash == Sha256Digest{} || index.cells.empty())
                return Reject<void>(WorldArtifactErrors::Invalid);
            if (model.objects.size() > limits.maximumObjects || model.softReferences.size() > limits.maximumSoftReferences ||
                index.cells.size() > limits.maximumCells)
                return Reject<void>(WorldArtifactErrors::CapacityExceeded);
            if (index.cells.size() != index.descriptor.Cells().size() ||
                !std::ranges::is_sorted(index.cells, StreamingCellCanonicalLess{}, &CookedWorldCellManifestEntry::cell))
                return Reject<void>(WorldArtifactErrors::Invalid);
            for (std::size_t position = 0; position < index.cells.size(); ++position)
                if (index.cells[position].cell != index.descriptor.Cells()[position].id)
                    return Reject<void>(WorldArtifactErrors::Invalid);
            return ValidateObjectProjections(model, index, limits, cancellation);
        }

        /** @brief Validates disjoint canonical hard components against exact known object revisions. */
        Result<std::vector<std::size_t>> ValidateBundles(const DecodedWorldDependencies &model, const WorldDependencyArtifactLimits &limits,
                                                         const CancellationToken &cancellation) {
            std::vector owners(model.objects.size(), model.bundles.size());
            std::size_t members{};
            for (std::size_t bundle = 0; bundle < model.bundles.size(); ++bundle) {
                if (cancellation.IsCancellationRequested())
                    return Reject<std::vector<std::size_t>>(WorldArtifactErrors::Cancelled);
                const auto &entries = model.bundles[bundle];
                if (entries.size() < 2 || (bundle && !(model.bundles[bundle - 1].front() < entries.front())) ||
                    !std::ranges::is_sorted(entries) || std::ranges::adjacent_find(entries) != entries.end())
                    return Reject<std::vector<std::size_t>>(WorldArtifactErrors::Invalid);
                if (entries.size() > limits.maximumBundleMembers - members)
                    return Reject<std::vector<std::size_t>>(WorldArtifactErrors::CapacityExceeded);
                members += entries.size();
                for (const auto &entry : entries) {
                    if (cancellation.IsCancellationRequested())
                        return Reject<std::vector<std::size_t>>(WorldArtifactErrors::Cancelled);
                    const auto found = FindObject(model, entry.address);
                    if (found == model.objects.size() || owners[found] != model.bundles.size())
                        return Reject<std::vector<std::size_t>>(WorldArtifactErrors::Invalid);
                    if (model.objects[found].object != entry)
                        return Reject<std::vector<std::size_t>>(WorldArtifactErrors::Stale);
                    owners[found] = bundle;
                }
            }
            return Result<std::vector<std::size_t>>::Success(std::move(owners));
        }

        /** @brief Keeps unresolved soft targets explicit while rejecting stale or hard-component-conflicting references. */
        Result<void> ValidateSoftReferences(const DecodedWorldDependencies &model, const std::span<const std::size_t> owners,
                                            const CancellationToken &cancellation) {
            for (std::size_t position = 0; position < model.softReferences.size(); ++position) {
                if (cancellation.IsCancellationRequested())
                    return Reject<void>(WorldArtifactErrors::Cancelled);
                const auto &edge = model.softReferences[position];
                if (edge.kind != WorldDependencyKind::Soft || !edge.source.IsValid() || !edge.target.IsValid() ||
                    edge.source.address == edge.target.address)
                    return Reject<void>(WorldArtifactErrors::Invalid);
                if (position) {
                    const auto &previous = model.softReferences[position - 1];
                    if (!(std::tie(previous.source.address, previous.target.address) < std::tie(edge.source.address, edge.target.address)))
                        return Reject<void>(WorldArtifactErrors::Invalid);
                }
                const auto source = FindObject(model, edge.source.address);
                const auto target = FindObject(model, edge.target.address);
                if (source == model.objects.size())
                    return Reject<void>(WorldArtifactErrors::Invalid);
                if (model.objects[source].object != edge.source ||
                    (target != model.objects.size() && model.objects[target].object != edge.target))
                    return Reject<void>(WorldArtifactErrors::Stale);
                if (target != model.objects.size() && owners[source] != model.bundles.size() && owners[source] == owners[target])
                    return Reject<void>(WorldArtifactErrors::Invalid);
            }
            return Result<void>::Success();
        }

        /** @brief Owns bounded derived cell rows and accounts every attempted edge, including duplicate work. */
        class CellClosure final {
        public:
            explicit CellClosure(const DecodedWorldIndex &index, const WorldDependencyArtifactLimits &limits,
                                 const std::size_t edgeCapacity, const CancellationToken &cancellation)
                : limits_(limits), cancellation_(cancellation), edgeCapacity_(edgeCapacity) {
                cells_.reserve(index.cells.size());
                for (const auto &entry : index.cells)
                    cells_.emplace_back(entry.cell, std::vector<StreamingCellId>{});
            }

            /** @brief Adds one exact admitted target without duplicate storage or unchecked work growth. */
            Result<void> Add(const StreamingCellId &source, const StreamingCellId &target) {
                if (cancellation_.IsCancellationRequested())
                    return Reject<void>(WorldArtifactErrors::Cancelled);
                if (steps_ == limits_.maximumDerivationSteps)
                    return Reject<void>(WorldArtifactErrors::CapacityExceeded);
                ++steps_;
                if (source == target)
                    return Result<void>::Success();
                auto found = std::ranges::lower_bound(cells_, source, StreamingCellCanonicalLess{}, &WorldDependencyCellProjection::cell);
                if (found == cells_.end() || found->cell != source)
                    return Reject<void>(WorldArtifactErrors::Invalid);
                auto &targets = found->hardTargets;
                const auto position = std::ranges::lower_bound(targets, target, StreamingCellCanonicalLess{});
                if (position != targets.end() && *position == target)
                    return Result<void>::Success();
                if (edges_ == edgeCapacity_)
                    return Reject<void>(WorldArtifactErrors::CapacityExceeded);
                targets.insert(position, target);
                ++edges_;
                return Result<void>::Success();
            }

            /** @brief Transfers the fully validated rows after all bounded derivation has completed. */
            std::vector<WorldDependencyCellProjection> &&TakeCells() && noexcept {
                return std::move(cells_);
            }

        private:
            std::vector<WorldDependencyCellProjection> cells_;
            const WorldDependencyArtifactLimits &limits_;
            const CancellationToken &cancellation_;
            const std::size_t edgeCapacity_;
            std::uint64_t steps_{};
            std::uint32_t edges_{};
        };

        /** @brief Derives complete co-load edges for a component from its actual placed cell union. */
        Result<void> AddBundle(const DecodedWorldDependencies &model, const std::span<const WorldDependencyEndpoint> bundle,
                               CellClosure &closure, const CancellationToken &cancellation) {
            std::vector<StreamingCellId> cells;
            std::size_t count{};
            for (const auto &member : bundle) {
                if (cancellation.IsCancellationRequested())
                    return Reject<void>(WorldArtifactErrors::Cancelled);
                count += model.objects[FindObject(model, member.address)].cells.size();
            }
            cells.reserve(count);
            for (const auto &member : bundle) {
                if (cancellation.IsCancellationRequested())
                    return Reject<void>(WorldArtifactErrors::Cancelled);
                const auto &placed = model.objects[FindObject(model, member.address)].cells;
                cells.insert(cells.end(), placed.begin(), placed.end());
            }
            std::ranges::sort(cells, StreamingCellCanonicalLess{});
            const auto duplicate = std::ranges::unique(cells);
            cells.erase(duplicate.begin(), duplicate.end());
            for (const auto &source : cells)
                for (const auto &target : cells)
                    if (auto added = closure.Add(source, target); added.HasError())
                        return added;
            return Result<void>::Success();
        }

        /** @brief Derives one single-owner object's coverage edges without changing membership or error ordering. */
        Result<void> AddSingleOwner(const WorldDependencyObjectProjection &object, CellClosure &closure) {
            if (object.placement != WorldSpanningObjectPlacement::SingleCellOwner)
                return Result<void>::Success();
            for (const auto &covered : object.coverage)
                if (auto added = closure.Add(covered, object.cells.front()); added.HasError())
                    return added;
            return Result<void>::Success();
        }

        /** @brief Reserves hard-edge storage within both wire and peak native budgets before deriving a single edge. */
        Result<std::size_t> EdgeCapacity(const DecodedWorldDependencies &model, const DecodedWorldIndex &index,
                                         const WorldDependencyArtifactLimits &limits) {
            std::array<std::size_t, 8> counts{model.objects.size(), 0, 0, model.bundles.size(), 0, model.softReferences.size(),
                                              index.cells.size(),   0};
            for (const auto &object : model.objects) {
                counts[1] += object.coverage.size();
                counts[2] += object.cells.size();
            }
            for (const auto &bundle : model.bundles) {
                if (bundle.size() > limits.maximumBundleMembers - counts[4])
                    return Reject<std::size_t>(WorldArtifactErrors::CapacityExceeded);
                counts[4] += bundle.size();
            }
            auto admitted = AdmitDependencyLayout(counts, limits);
            if (admitted.HasError())
                return Result<std::size_t>::Failure(admitted.ErrorValue());
            const auto remaining = DependencyAllocationRemaining(counts, limits.maximumDecodedAllocationBytes);
            const auto wireMaximum = std::min<std::size_t>(limits.maximumArtifactBytes, std::numeric_limits<std::uint32_t>::max());
            const auto wireEdges = (wireMaximum - admitted.Value().total) / 16;
            const auto nativeEdges = *remaining / (2 * sizeof(StreamingCellId));
            return Result<std::size_t>::Success(std::min({wireEdges, nativeEdges, static_cast<std::size_t>(limits.maximumHardCellEdges)}));
        }
    }  // namespace

    /** @copydoc DeriveWorldDependencyCells */
    Result<std::vector<WorldDependencyCellProjection>> DeriveWorldDependencyCells(const DecodedWorldDependencies &model,
                                                                                  const DecodedWorldIndex &index,
                                                                                  const WorldDependencyArtifactLimits &limits,
                                                                                  const CancellationToken &cancellation) {
        if (const auto objects = ValidateObjects(model, index, limits, cancellation); objects.HasError())
            return Result<std::vector<WorldDependencyCellProjection>>::Failure(objects.ErrorValue());
        auto capacity = EdgeCapacity(model, index, limits);
        if (capacity.HasError())
            return Result<std::vector<WorldDependencyCellProjection>>::Failure(capacity.ErrorValue());
        auto owners = ValidateBundles(model, limits, cancellation);
        if (owners.HasError())
            return Result<std::vector<WorldDependencyCellProjection>>::Failure(owners.ErrorValue());
        if (const auto soft = ValidateSoftReferences(model, owners.Value(), cancellation); soft.HasError())
            return Result<std::vector<WorldDependencyCellProjection>>::Failure(soft.ErrorValue());
        CellClosure closure{index, limits, capacity.Value(), cancellation};
        for (const auto &object : model.objects)
            if (auto added = AddSingleOwner(object, closure); added.HasError())
                return Result<std::vector<WorldDependencyCellProjection>>::Failure(added.ErrorValue());
        for (const auto &bundle : model.bundles)
            if (auto added = AddBundle(model, bundle, closure, cancellation); added.HasError())
                return Result<std::vector<WorldDependencyCellProjection>>::Failure(added.ErrorValue());
        return Result<std::vector<WorldDependencyCellProjection>>::Success(std::move(closure).TakeCells());
    }

    /** @copydoc CaptureWorldDependencyProjection */
    Result<DecodedWorldDependencies> CaptureWorldDependencyProjection(
        const DecodedWorldIndex &index, const WorldSpatialAssignment &assignments, const WorldSpanningObjectPlan &placements,
        const WorldDependencyPlan &dependencies, const WorldDependencyArtifactLimits &limits, const CancellationToken &cancellation) {
        if (!ValidLimits(limits))
            return Reject<DecodedWorldDependencies>(WorldArtifactErrors::Invalid);
        if (assignments.Partition() != index.descriptor.Partition() || placements.Partition() != assignments.Partition() ||
            dependencies.Partition() != assignments.Partition() || assignments.Objects().size() != placements.Objects().size())
            return Reject<DecodedWorldDependencies>(WorldArtifactErrors::Stale);
        if (assignments.Objects().size() > limits.maximumObjects || dependencies.SoftReferences().size() > limits.maximumSoftReferences)
            return Reject<DecodedWorldDependencies>(WorldArtifactErrors::CapacityExceeded);
        DecodedWorldDependencies model{.partition = assignments.Partition(), .indexArtifactHash = index.artifactHash};
        model.objects.reserve(assignments.Objects().size());
        model.bundles.reserve(dependencies.Bundles().size());
        std::size_t totalCoverage{};
        std::size_t totalPlacements{};
        for (std::size_t position = 0; position < assignments.Objects().size(); ++position) {
            if (cancellation.IsCancellationRequested())
                return Reject<DecodedWorldDependencies>(WorldArtifactErrors::Cancelled);
            const auto &object = assignments.Objects()[position];
            const auto &placed = placements.Objects()[position];
            if (object.address != placed.address || object.revision != placed.revision)
                return Reject<DecodedWorldDependencies>(WorldArtifactErrors::Stale);
            const auto coverage = assignments.CellsForObject(position);
            const auto cells = placements.CellsForObject(position);
            if (coverage.size() > limits.maximumCoverageCells - totalCoverage ||
                cells.size() > limits.maximumPlacementCells - totalPlacements)
                return Reject<DecodedWorldDependencies>(WorldArtifactErrors::CapacityExceeded);
            totalCoverage += coverage.size();
            totalPlacements += cells.size();
            model.objects.push_back(
                {{object.address, object.revision}, placed.placement, {coverage.begin(), coverage.end()}, {cells.begin(), cells.end()}});
        }
        std::size_t members{};
        for (std::size_t position = 0; position < dependencies.Bundles().size(); ++position) {
            if (cancellation.IsCancellationRequested())
                return Reject<DecodedWorldDependencies>(WorldArtifactErrors::Cancelled);
            const auto bundle = dependencies.MembersForBundle(position);
            if (bundle.size() > limits.maximumBundleMembers - members)
                return Reject<DecodedWorldDependencies>(WorldArtifactErrors::CapacityExceeded);
            members += bundle.size();
            model.bundles.emplace_back(bundle.begin(), bundle.end());
        }
        model.softReferences.assign(dependencies.SoftReferences().begin(), dependencies.SoftReferences().end());
        auto derived = DeriveWorldDependencyCells(model, index, limits, cancellation);
        if (derived.HasError())
            return Result<DecodedWorldDependencies>::Failure(derived.ErrorValue());
        model.cells = std::move(derived).Value();
        return Result<DecodedWorldDependencies>::Success(std::move(model));
    }
}  // namespace Horo::WorldStreaming::Detail
