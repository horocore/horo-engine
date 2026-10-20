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

static_assert(std::is_same_v<decltype(std::declval<PartitionedWorldWorkspace &>().EditSource(Horo::Editor::WorldSourceEdit{})),
                             Horo::Result<PartitionedWorldSnapshotLease>>);
static_assert(std::is_same_v<decltype(Horo::Editor::WorldSourceViewportPoint({}, {})), Horo::Result<Horo::Math::Vec3>>);
