#pragma once
/** @file TerrainToolSelection.h @brief Owner-lane transient terrain palette and selection, exposed read-only during Draw. */
#include "FoliagePaletteModel.h"
#include "FoliageSelectionModel.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"

namespace Horo::Editor {
    /** @brief One document's disposable tool state. The controller publishes const shared borrows; no source authority is retained. */
    struct TerrainToolSelection final {
        DocumentInstanceId document;
        FoliagePaletteModel palette;
        FoliageSelectionModel selection;
    };
}  // namespace Horo::Editor
