#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <algorithm>
#include <memory>
#include <set>
#include <utility>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        /** @brief Looks up the exact page-scoped source identity, independently from open geometry. */
        const PartitionedWorldObject *FindObject(const PartitionedWorldDocument &document, const WS::WorldAuthoringObjectAddress address) {
            const auto page = std::ranges::find(document.pages, address.page, [](const auto &value) {
                return value.head.sourceAsset;
            });
            if (page == document.pages.end())
                return nullptr;
            const auto object = std::ranges::find(page->objects, address, [](const auto &value) {
                return value.descriptor.address;
            });
            return object == page->objects.end() ? nullptr : std::to_address(object);
        }

        /** @brief Prepares source coverage and exact policy explanation without mutating document or runtime state. */
        Result<WorldObjectInspection> InspectObject(const PartitionedWorldObject &object, const PartitionedWorldDocument &document,
                                                    const WorldObjectInspectionLimits limits) {
            WorldObjectInspection result{.source = object};
            if (object.descriptor.placement == WS::WorldSpatialObjectPlacementClass::AlwaysPresent) {
                result.placement = WS::WorldSpanningObjectPlacement::NonSpatial;
                return Result<WorldObjectInspection>::Success(std::move(result));
            }
            const WS::WorldSpatialAssignmentCandidate candidate{.address = object.descriptor.address,
                                                                .revision = object.descriptor.revision,
                                                                .bounds = object.descriptor.bounds,
                                                                .layer = object.layer,
                                                                .lod = object.assignmentLod};
            const WS::WorldSpatialAssignmentAuthoringDomain domain{document.settings.partition, document.settings.bounds,
                                                                   document.settings.grid, document.layers};
            auto coverage = WS::WorldSpatialAssignment::CreateForAuthoring(domain, std::span{&candidate, 1},
                                                                           {1, limits.maximumCellsPerObject, limits.maximumTotalCells});
            if (coverage.HasError()) {
                if (coverage.ErrorValue().code.Value() == WS::WorldStreamingErrors::SpatialAssignmentCapacityExceeded.code.Value())
                    return Result<WorldObjectInspection>::Failure(MakeError(WorldDocumentErrors::Capacity));
                result.diagnostic = coverage.ErrorValue();
                return Result<WorldObjectInspection>::Success(std::move(result));
            }
            const auto cells = coverage.Value().CellsForObject(0);
            result.automaticCells.assign(cells.begin(), cells.end());
            result.spanning = cells.size() > 1;
            result.requiresDirective = cells.size() > document.settings.maximumDirectCellsPerObject;
            const auto directives = object.spanning ? std::span{&*object.spanning, 1} : std::span<const WS::WorldSpanningObjectDirective>{};
            if (auto placement =
                    WS::WorldSpanningObjectPlan::Create(coverage.Value(), directives,
                                                        {1, document.settings.maximumDirectCellsPerObject, limits.maximumTotalCells});
                placement.HasError()) {
                if (placement.ErrorValue().code.Value() == WS::WorldStreamingErrors::SpanningObjectPlanCapacityExceeded.code.Value())
                    return Result<WorldObjectInspection>::Failure(MakeError(WorldDocumentErrors::Capacity));
                result.diagnostic = placement.ErrorValue();
            } else {
                const auto effective = placement.Value().CellsForObject(0);
                if (effective.size() > limits.maximumTotalCells - cells.size())
                    return Result<WorldObjectInspection>::Failure(MakeError(WorldDocumentErrors::Capacity));
                result.placementCells.assign(effective.begin(), effective.end());
                result.placement = placement.Value().Objects().front().placement;
            }
            return Result<WorldObjectInspection>::Success(std::move(result));
        }

        /** @brief Checks actual content membership when the selected source page has an editor payload. */
        bool HasLoadedObject(const PartitionedWorldSnapshot &snapshot, const WS::WorldAuthoringObjectAddress address) {
            const auto page = std::ranges::find(snapshot.loadedPages, address.page, [](const auto &loaded) {
                return loaded.head.sourceAsset;
            });
            return page == snapshot.loadedPages.end() || std::ranges::any_of(page->content.Objects(), [&](const auto &object) {
                return object.id.value == address.object;
            });
        }

        /** @brief Validates selection membership and primary against the same immutable source root. */
        Result<void> ValidateSelection(const PartitionedWorldSnapshot &snapshot,
                                       const std::span<const WS::WorldAuthoringObjectAddress> objects,
                                       const std::optional<WS::WorldAuthoringObjectAddress> &primary) {
            std::set<WS::WorldAuthoringObjectAddress> distinct;
            for (const auto &address : objects) {
                if (!address.IsValid())
                    return Result<void>::Failure(MakeError(WorldDocumentErrors::Invalid));
                if (!distinct.insert(address).second || !FindObject(snapshot.document, address) || !HasLoadedObject(snapshot, address))
                    return Result<void>::Failure(MakeError(WorldDocumentErrors::Conflict));
            }
            if (primary && !distinct.contains(*primary))
                return Result<void>::Failure(MakeError(WorldDocumentErrors::Conflict));
            return Result<void>::Success();
        }

        /** @brief Checks the consumer output reservation before any explanation storage is allocated. */
        bool ValidInspectionLimits(const WorldObjectInspectionLimits limits) {
            constexpr WorldObjectInspectionLimits hard;
            return limits.maximumCellsPerObject != 0 && limits.maximumTotalCells != 0 &&
                   limits.maximumCellsPerObject <= hard.maximumCellsPerObject && limits.maximumTotalCells <= hard.maximumTotalCells;
        }

        /** @brief Builds the complete bounded immutable result; a failed row budget never publishes a prefix. */
        Result<WorldObjectInspectionLease> PrepareInspection(const PartitionedWorldSnapshot &snapshot,
                                                             const WorldObjectInspectionLimits limits, const std::stop_token cancellation) {
            auto candidate = std::make_shared<WorldObjectSelectionInspection>();
            candidate->fence = snapshot.fence;
            candidate->primary = snapshot.primary;
            candidate->objects.reserve(snapshot.selectedObjects.size());
            std::size_t usedCells{};
            for (const auto &address : snapshot.selectedObjects) {
                if (cancellation.stop_requested())
                    return Result<WorldObjectInspectionLease>::Failure(MakeError(WorldDocumentErrors::Cancelled));
                const auto *source = FindObject(snapshot.document, address);
                if (!source)
                    return Result<WorldObjectInspectionLease>::Failure(MakeError(WorldDocumentErrors::Stale));
                auto remaining = limits;
                // A zero remaining budget still admits always-present metadata but never a spatial enumeration.
                if (usedCells == limits.maximumTotalCells && source->descriptor.placement == WS::WorldSpatialObjectPlacementClass::Spatial)
                    return Result<WorldObjectInspectionLease>::Failure(MakeError(WorldDocumentErrors::Capacity));
                remaining.maximumTotalCells -= static_cast<std::uint32_t>(usedCells);
                auto row = InspectObject(*source, snapshot.document, remaining);
                if (row.HasError())
                    return Result<WorldObjectInspectionLease>::Failure(row.ErrorValue());
                usedCells += row.Value().automaticCells.size() + row.Value().placementCells.size();
                candidate->objects.push_back(std::move(row).Value());
            }
            if (cancellation.stop_requested())
                return Result<WorldObjectInspectionLease>::Failure(MakeError(WorldDocumentErrors::Cancelled));
            return Result<WorldObjectInspectionLease>::Success(std::move(candidate));
        }
    }  // namespace

    /** @copydoc PartitionedWorldWorkspace::SetSelection */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::SetSelection(
        const std::span<const WS::WorldAuthoringObjectAddress> objects, const std::optional<WS::WorldAuthoringObjectAddress> &primary,
        const WorldDocumentFence &expected, const std::stop_token cancellation) {
        if (const auto valid = CheckFence(expected); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Result<PartitionedWorldSnapshotLease>::Failure(MakeError(WorldDocumentErrors::Cancelled));
        if (objects.size() > limits_.maximumSelectedObjects)
            return Result<PartitionedWorldSnapshotLease>::Failure(MakeError(WorldDocumentErrors::Capacity));
        if (const auto valid = ValidateSelection(*snapshot_, objects, primary); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (std::ranges::equal(objects, snapshot_->selectedObjects) && primary == snapshot_->primary)
            return Result<PartitionedWorldSnapshotLease>::Success(snapshot_);
        auto candidate = std::make_shared<PartitionedWorldSnapshot>(*snapshot_);
        candidate->selectedObjects.assign(objects.begin(), objects.end());
        candidate->primary = primary;
        return Publish(std::move(candidate), cancellation);
    }

    /** @copydoc PartitionedWorldWorkspace::InspectSelectedObjects */
    Result<WorldObjectInspectionLease> PartitionedWorldWorkspace::InspectSelectedObjects(const WorldDocumentFence &expected,
                                                                                         const WorldObjectInspectionLimits limits,
                                                                                         const std::stop_token cancellation) const {
        if (const auto valid = CheckFence(expected); valid.HasError())
            return Result<WorldObjectInspectionLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Result<WorldObjectInspectionLease>::Failure(MakeError(WorldDocumentErrors::Cancelled));
        if (!ValidInspectionLimits(limits))
            return Result<WorldObjectInspectionLease>::Failure(MakeError(WorldDocumentErrors::Capacity));
        return PrepareInspection(*snapshot_, limits, cancellation);
    }
}  // namespace Horo::Editor
