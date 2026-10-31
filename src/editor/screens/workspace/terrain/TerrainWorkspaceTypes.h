#pragma once
/** @file TerrainWorkspaceTypes.h @brief Private copied terrain document projections and complete revision-fenced intents. */
#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "TerrainSculptProducer.h"

#include <array>

namespace Horo::Editor {
    /** @brief Exact private workspace publication; UI retains only const host access and sends typed commands. */
    struct TerrainDocumentView final {
        TerrainEditFence fence;
        Assets::AssetId workspace;
        DocumentInstanceId instance;
        bool dirty{}, canUndo{}, canRedo{}, conflict{}, closed{};
    };
    enum class TerrainToolAction : std::uint8_t {
        Stroke,
        Undo,
        Redo,
        Save,
        Close,
        Discard,
        ReportFailure
    };

    /** @brief Complete revision-fenced command; no borrowed UI storage or mutable authority crosses the frame. */
    struct TerrainToolCommand final {
        TerrainToolAction action{TerrainToolAction::Stroke};
        TerrainDocumentView document;
        TerrainSculptBrush brush;
        std::array<TerrainSculptStamp, 64> stamps{};
        std::size_t stampCount{};
        std::optional<Error> failure;
    };
}  // namespace Horo::Editor
