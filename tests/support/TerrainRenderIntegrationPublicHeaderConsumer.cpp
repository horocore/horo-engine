#include "Horo/TerrainRenderIntegration/TerrainRenderSnapshot.h"
#include "Horo/TerrainRenderIntegration/TerrainTileResourceOwner.h"

#include <type_traits>
static_assert(std::is_same_v<decltype(Horo::TerrainRender::TerrainRenderSnapshotHeader::origin), Horo::WorldStreaming::OriginFrameBinding>);
using CaptureResult = Horo::Result<Horo::TerrainRender::TerrainRenderExtractionSnapshot>;
using CaptureContract = CaptureResult (*)(const Horo::TerrainRender::TerrainRenderSnapshotRequest &, const Horo::CancellationToken &);
static_assert(std::is_same_v<decltype(&Horo::TerrainRender::CaptureTerrainRenderSnapshot), CaptureContract>);
static_assert(!std::is_copy_constructible_v<Horo::TerrainRender::TerrainTileResourceOwner>);
static_assert(std::is_copy_constructible_v<Horo::TerrainRender::TerrainRenderExtractionSnapshot>);

using OperationObservation =
    Horo::Result<Horo::Render::RenderResourceOperationState> (Horo::Render::RenderFrontend::*)(Horo::Render::ResourceOperationId) const;
static_assert(std::is_same_v<decltype(&Horo::Render::RenderFrontend::ResourceOperationState), OperationObservation>);
