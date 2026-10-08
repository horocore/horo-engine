#include "Horo/Editor/TerrainAuthoringDocument.h"

#include <type_traits>
static_assert(!std::is_copy_constructible_v<Horo::Editor::TerrainAuthoringDocument>);
static_assert(!std::is_same_v<Horo::Editor::TerrainDocumentStateId, Horo::Editor::TerrainDocumentRevision>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Editor::TerrainAuthoringDocument &>().Source()),
                             const Horo::Terrain::TerrainCanonicalSource &>);
