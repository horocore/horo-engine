#pragma once
/** @file TerrainWorkspaceTypes.h @brief Private copied terrain document projections and complete revision-fenced intents. */
#include "FoliagePaintProducer.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "TerrainPaintProducer.h"
#include "TerrainSculptProducer.h"

#include <array>

namespace Horo::Editor {
    /** @brief Exact private workspace publication; UI retains only const host access and sends typed commands. */
    struct TerrainDocumentView final {
        TerrainEditFence fence;
        Assets::AssetId workspace;
        DocumentInstanceId instance;
        bool dirty{}, canUndo{}, canRedo{}, conflict{}, closed{};
        bool containerSource{}, hasLayerBinding{};
    };
    /** @brief Explicit selected authoring mode; runtime and source capabilities are never inferred from a mode. */
    enum class TerrainToolMode : std::uint8_t {
        Sculpt,
        Weights,
        CarveHole,
        RestoreHole,
        FoliagePaint,
        FoliageErase,
        FoliageSelect
    };
    enum class TerrainToolAction : std::uint8_t {
        Stroke,
        Undo,
        Redo,
        Save,
        Close,
        Discard,
        ReportFailure,
        ChooseFoliage,
        RefreshFoliage,
        ConvertSource,
        Cook,
        CancelCook
    };

    /** @brief Complete revision-fenced command; no borrowed UI storage or mutable authority crosses the frame. */
    struct TerrainToolCommand final {
        TerrainToolAction action{TerrainToolAction::Stroke};
        TerrainDocumentView document;
        TerrainSculptBrush brush;
        TerrainToolMode mode{TerrainToolMode::Sculpt};
        TerrainPaintBrush paint;
        TerrainPaintFence paintFence;
        FoliagePaintBrush foliage;
        std::optional<Terrain::FoliageAuthoringDefinitionReference> foliageType;
        std::optional<Terrain::TerrainAuthoringLayerBinding> conversionLayers;
        std::uint64_t cookEpoch{}; /**< Owner-issued adoption epoch; stale cancellation cannot affect a later retry. */
        std::array<TerrainSculptStamp, 64> stamps{};
        std::size_t stampCount{};
        std::optional<Error> failure;
    };
}  // namespace Horo::Editor
