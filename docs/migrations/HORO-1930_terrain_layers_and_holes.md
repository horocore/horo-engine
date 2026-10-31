# HORO-1930 / #1974: terrain layer painting and holes

The new authoring source container has its own schema 1. It does not change the
engine project version, RAW-F32 input, HTIL, HTMF, HTSG or HTPM formats. The prefix
starts with a reserved NaN representation, so a valid finite RAW-F32 height raster
cannot be mistaken for this container. Existing height assets retain their
original behavior until an explicit clean-source conversion.

The container owns canonical heights, all stored normalized weight channels,
binary holes, coordinates, semantic source revision and the exact ordered layer
binding. Layer-set source has a separate bounded schema 1 using the existing
Terrain material layer identities and actual material AssetIds. Up to four active
paint targets select from the full stored order; inactive layers are never dropped.
Layer-set asset identity, semantic revision and captured digest are durable data.
Workspace sessions, host capability publications, registry epochs and cook grants
are not source authority and are not persisted in the authoring container.

`Horo/Terrain/TerrainAuthoringSourceCodec.h` belongs exclusively to
`HoroTerrainImport`. Consumers link `HoroEngine::TerrainImport`; the source codec
tests and a standalone public consumer compile through this declared boundary.
Draft private consumers migrate to this header when the source codec is promoted.
The existing terrain document remains the only source mutation/history owner.

This raster codec does not yet encode authored foliage placements. A nonempty
placement source must be refused by the raster-only save adapter rather than
silently discarded. Foliage authoring needs its own explicit schema evolution
and immutable definition/placement capture; procedural scatter output is not an
authored-placement persistence substitute.
