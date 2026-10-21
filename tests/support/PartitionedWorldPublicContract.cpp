#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <type_traits>
#include <utility>

using Horo::Editor::PartitionedWorldDocument;
using Horo::Editor::PartitionedWorldSnapshot;
using Horo::Editor::PartitionedWorldSnapshotLease;
using Horo::Editor::PartitionedWorldWorkspace;
using Horo::Editor::WorldDocumentFence;

static_assert(std::is_same_v<PartitionedWorldSnapshotLease::element_type, const PartitionedWorldSnapshot>);
static_assert(!std::is_copy_constructible_v<PartitionedWorldWorkspace>);
static_assert(!std::is_move_constructible_v<PartitionedWorldWorkspace>);
static_assert(std::is_copy_constructible_v<PartitionedWorldDocument>);
static_assert(std::is_same_v<decltype(std::declval<PartitionedWorldWorkspace &>().Commit(std::declval<const PartitionedWorldDocument &>(),
                                                                                         WorldDocumentFence{})),
                             Horo::Result<PartitionedWorldSnapshotLease>>);
static_assert(std::is_same_v<decltype(std::declval<PartitionedWorldWorkspace &>().SetWorkingSet(std::span<const Horo::Assets::AssetId>{},
                                                                                                WorldDocumentFence{})),
                             Horo::Result<PartitionedWorldSnapshotLease>>);
static_assert(!std::is_same_v<Horo::Editor::WorldDocumentRevision, Horo::WorldStreaming::WorldAuthoringRevision>);
static_assert(!std::is_same_v<Horo::Editor::WorldDocumentGeneration, Horo::WorldStreaming::StreamingGeneration>);

static_assert(std::is_same_v<Horo::Editor::WorldObjectInspectionLease::element_type, const Horo::Editor::WorldObjectSelectionInspection>);
static_assert(std::is_same_v<decltype(std::declval<PartitionedWorldWorkspace &>().SetSelection(
                                 std::span<const Horo::WorldStreaming::WorldAuthoringObjectAddress>{}, std::nullopt, WorldDocumentFence{})),
                             Horo::Result<PartitionedWorldSnapshotLease>>);
static_assert(std::is_same_v<decltype(std::declval<const PartitionedWorldWorkspace &>().InspectSelectedObjects(WorldDocumentFence{})),
                             Horo::Result<Horo::Editor::WorldObjectInspectionLease>>);

static_assert(std::is_same_v<decltype(Horo::WorldStreaming::WorldSpatialAssignment::CreateForAuthoring(
                                 std::declval<const Horo::WorldStreaming::WorldSpatialAssignmentAuthoringDomain &>(),
                                 std::span<const Horo::WorldStreaming::WorldSpatialAssignmentCandidate>{},
                                 Horo::WorldStreaming::WorldSpatialAssignmentLimits{})),
                             Horo::Result<Horo::WorldStreaming::WorldSpatialAssignment>>);

using WorldSelectionMethod = Horo::Result<PartitionedWorldSnapshotLease> (PartitionedWorldWorkspace::*)(
    std::span<const Horo::WorldStreaming::WorldAuthoringObjectAddress>,
    const std::optional<Horo::WorldStreaming::WorldAuthoringObjectAddress> &, const WorldDocumentFence &, std::stop_token);
using WorldInspectionMethod = Horo::Result<Horo::Editor::WorldObjectInspectionLease> (PartitionedWorldWorkspace::*)(
    const WorldDocumentFence &, Horo::Editor::WorldObjectInspectionLimits, std::stop_token) const;
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::SetSelection), WorldSelectionMethod>);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::InspectSelectedObjects), WorldInspectionMethod>);
