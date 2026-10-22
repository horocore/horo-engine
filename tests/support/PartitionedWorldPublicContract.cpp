#include "Horo/Editor/PartitionedWorldWorkspace.h"
#include "Horo/Editor/WorldStreamingPreview.h"
#include "Horo/Editor/WorldStreamingPreviewRuntime.h"

#include <type_traits>
#include <utility>

using Horo::Editor::PartitionedWorldDocument;
using Horo::Editor::PartitionedWorldSnapshot;
using Horo::Editor::PartitionedWorldSnapshotLease;
using Horo::Editor::PartitionedWorldWorkspace;
using Horo::Editor::WorldDocumentFence;
using Horo::Editor::WorldStreamingPreview;
using Horo::Editor::WorldStreamingPreviewCell;

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

static_assert(std::is_same_v<decltype(std::declval<const WorldStreamingPreview &>().Cells()), std::span<const WorldStreamingPreviewCell>>);
static_assert(std::is_same_v<decltype(std::declval<PartitionedWorldWorkspace &>().EditSource(Horo::Editor::WorldSourceEdit{})),
                             Horo::Result<PartitionedWorldSnapshotLease>>);
static_assert(std::is_same_v<decltype(Horo::Editor::WorldSourceViewportPoint({}, {})), Horo::Result<Horo::Math::Vec3>>);
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

static_assert(
    std::is_same_v<decltype(std::declval<const PartitionedWorldWorkspace &>().Browse(
                       std::declval<const Horo::Editor::WorldBrowserQuery &>(), std::declval<Horo::Editor::WorldDocumentFence>())),
                   Horo::Result<Horo::Editor::WorldBrowserSnapshot>>);

static_assert(!std::is_copy_constructible_v<Horo::Editor::WorldStreamingPreviewHost>);
static_assert(std::is_same_v<decltype(std::declval<Horo::Editor::WorldStreamingPreviewHost &>().Refresh(
                                 std::declval<const Horo::Editor::WorldViewportBinding &>())),
                             Horo::Result<std::shared_ptr<const Horo::Editor::WorldStreamingPreviewGeometry>>>);
static_assert(std::is_same_v<decltype(Horo::Editor::WorldSourceGizmoPoints(Horo::Editor::PartitionedWorldSource{})),
                             Horo::Result<Horo::Editor::WorldSourceGizmoPointSet>>);

static_assert(!std::is_copy_constructible_v<Horo::Editor::WorldStreamingPreviewRuntime>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::WorldStreaming::WorldStreamingDiagnosticSnapshot &>().BudgetPolicy()),
                             const Horo::WorldStreaming::StreamingBudgetPolicy *>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::WorldStreaming::WorldStreamingDiagnosticSnapshot &>().BudgetSample()),
                             const Horo::WorldStreaming::StreamingBudgetSample *>);
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

using WorldBrowserMethod = Horo::Result<Horo::Editor::WorldBrowserSnapshot> (PartitionedWorldWorkspace::*)(
    const Horo::Editor::WorldBrowserQuery &, WorldDocumentFence, std::stop_token) const;
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::Browse), WorldBrowserMethod>);
using SourceReplayMethod = Horo::Result<PartitionedWorldSnapshotLease> (PartitionedWorldWorkspace::*)(const WorldDocumentFence &, bool,
                                                                                                      std::stop_token);
using SourceViewportPointMethod = Horo::Result<Horo::Math::Vec3> (*)(const Horo::Math::WorldCoordinate64 &,
                                                                     const Horo::Math::WorldCoordinate64 &);
using SourceViewportGlobalMethod = Horo::Result<Horo::Math::WorldCoordinate64> (*)(const Horo::Math::WorldCoordinate64 &, Horo::Math::Vec3);
using SourceGizmoMoveMethod = Horo::Result<Horo::Editor::PartitionedWorldSource> (*)(const Horo::Editor::PartitionedWorldSource &,
                                                                                     std::size_t, const Horo::Math::WorldCoordinate64 &);
static_assert(std::is_same_v<decltype(&PartitionedWorldWorkspace::ReplaySources), SourceReplayMethod>);
static_assert(std::is_same_v<decltype(&Horo::Editor::WorldSourceViewportPoint), SourceViewportPointMethod>);
static_assert(std::is_same_v<decltype(&Horo::Editor::WorldSourceViewportGlobal), SourceViewportGlobalMethod>);
static_assert(std::is_same_v<decltype(&Horo::Editor::MoveWorldSourceGizmo), SourceGizmoMoveMethod>);

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
