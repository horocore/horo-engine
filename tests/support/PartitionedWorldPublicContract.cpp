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
