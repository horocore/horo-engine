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

static_assert(std::is_same_v<decltype(std::declval<PartitionedWorldWorkspace &>().ExecuteLayerCommand(
                                 std::declval<const Horo::Editor::WorldLayerCommand &>(), WorldDocumentFence{})),
                             Horo::Result<PartitionedWorldSnapshotLease>>);
static_assert(std::is_same_v<decltype(std::declval<PartitionedWorldWorkspace &>().ReplayLayerCommand(false, WorldDocumentFence{})),
                             Horo::Result<PartitionedWorldSnapshotLease>>);
static_assert(std::is_same_v<decltype(std::declval<const PartitionedWorldWorkspace &>().VisibleLayers()),
                             Horo::Result<std::vector<Horo::WorldStreaming::StreamingLayerId>>>);
static_assert(std::is_same_v<decltype(std::declval<Horo::Editor::WorldPageContentLease>().Objects()),
                             std::span<const Horo::SceneSource::SceneObjectSnapshot>>);
static_assert(std::is_copy_constructible_v<Horo::Editor::WorldPageContentLease>);
static_assert(std::is_same_v<decltype(std::declval<PartitionedWorldWorkspace &>().LoadPage(
                                 Horo::WorldStreaming::WorldAuthoringPageDescriptor{}, std::string_view{}, WorldDocumentFence{})),
                             Horo::Result<PartitionedWorldSnapshotLease>>);
static_assert(
    std::is_same_v<decltype(std::declval<PartitionedWorldWorkspace &>().UnloadPage(Horo::Assets::AssetId{}, WorldDocumentFence{})),
                   Horo::Result<PartitionedWorldSnapshotLease>>);

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

static_assert(std::is_same_v<decltype(std::declval<PartitionedWorldWorkspace &>().CreateLayer(std::string{}, WorldDocumentFence{})),
                             Horo::Result<PartitionedWorldSnapshotLease>>);

using WorldSelectionMethod = Horo::Result<PartitionedWorldSnapshotLease> (PartitionedWorldWorkspace::*)(
    std::span<const Horo::WorldStreaming::WorldAuthoringObjectAddress>,
    const std::optional<Horo::WorldStreaming::WorldAuthoringObjectAddress> &, const WorldDocumentFence &, std::stop_token);
using WorldInspectionMethod = Horo::Result<Horo::Editor::WorldObjectInspectionLease> (PartitionedWorldWorkspace::*)(
    const WorldDocumentFence &, Horo::Editor::WorldObjectInspectionLimits, std::stop_token) const;
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::SetSelection), WorldSelectionMethod>);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::InspectSelectedObjects), WorldInspectionMethod>);

using ManualPageLoad = Horo::Result<PartitionedWorldSnapshotLease> (PartitionedWorldWorkspace::*)(
    const Horo::WorldStreaming::WorldAuthoringPageDescriptor &, std::string_view, const WorldDocumentFence &, std::stop_token);
using ManualPageUnload = Horo::Result<PartitionedWorldSnapshotLease> (PartitionedWorldWorkspace::*)(Horo::Assets::AssetId,
                                                                                                    const WorldDocumentFence &,
                                                                                                    std::stop_token);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::LoadPage), ManualPageLoad>);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::UnloadPage), ManualPageUnload>);

using LayerExecuteMethod = Horo::Result<PartitionedWorldSnapshotLease> (PartitionedWorldWorkspace::*)(
    const Horo::Editor::WorldLayerCommand &, const WorldDocumentFence &, std::stop_token);
using LayerCreateMethod = Horo::Result<PartitionedWorldSnapshotLease> (PartitionedWorldWorkspace::*)(std::string,
                                                                                                     const WorldDocumentFence &,
                                                                                                     std::stop_token);
using LayerReplayMethod = Horo::Result<PartitionedWorldSnapshotLease> (PartitionedWorldWorkspace::*)(bool, const WorldDocumentFence &,
                                                                                                     std::stop_token);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::ExecuteLayerCommand), LayerExecuteMethod>);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::CreateLayer), LayerCreateMethod>);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::ReplayLayerCommand), LayerReplayMethod>);
using CommitSignature = Horo::Result<PartitionedWorldSnapshotLease> (PartitionedWorldWorkspace::*)(const PartitionedWorldDocument &,
                                                                                                   const WorldDocumentFence &,
                                                                                                   std::stop_token);
using WorkingSetSignature = Horo::Result<PartitionedWorldSnapshotLease> (PartitionedWorldWorkspace::*)(
    std::span<const Horo::Assets::AssetId>, const WorldDocumentFence &, std::stop_token);
using ReplaceSignature = Horo::Result<PartitionedWorldSnapshotLease> (PartitionedWorldWorkspace::*)(const PartitionedWorldDocument &,
                                                                                                    const WorldDocumentFence &,
                                                                                                    Horo::Editor::WorldDocumentDisposition,
                                                                                                    std::stop_token);
using CloseSignature = Horo::Result<void> (PartitionedWorldWorkspace::*)(const WorldDocumentFence &,
                                                                         Horo::Editor::WorldDocumentDisposition);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::Commit), CommitSignature>);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::SetWorkingSet), WorkingSetSignature>);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::Replace), ReplaceSignature>);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::Close), CloseSignature>);
