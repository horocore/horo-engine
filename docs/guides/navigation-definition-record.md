# NavigationDefinition source record

This development contract is introduced by HORO-1253 for HoroProjectVersion
0.2.0. It is not a signed release or permission to reinterpret older source.
`HoroEngine::NavigationApi` owns NavigationDefinition.h and
NavigationDefinitionSerialization.h; the existing ownership registry stages
these headers and generates their public consumers. Application capture, Editor
and automation must use this one semantic codec.

The registered core authored record type is `0x4e41564445463031` (NAVDEF01),
payload version 1.0, required and nonopaque. Its record ID is an independent
owner-issued nonzero authoring identity. The asset sidecar remains the sole
definition AssetId authority. The surrounding HNAV 1.1 format, checksum, unknown
optional preservation and required-unknown rejection are unchanged. Consumer
support is exact 1.0, not a guessed range or fallback to synthetic test types.

## Canonical payload

All integers use little-endian fixed widths. Floats are IEEE binary32 with zero
normalized to positive zero. Every field below is mandatory; no omitted field
receives a default during decoding. No padding, runtime handle or generated data
is serialized.

1. Coordinate system uint32 (existing enum), meters-per-unit float32,
   tile-size-in-cells uint32, complete-closure maximum-tiles uint32, definition
   source scope uint32 (0 Scene, 1 Project).
2. Profile count uint32. Each row: profile ID uint64, UTF-8 name byte count uint32
   and bytes, then radius, height, slope, step height, cell size, cell height and
   minimum region size as seven float32 values in that order.
3. Area count uint32. Each row: area ID uint64, source-kind uint32, source ID
   uint64, traversal cost float32, flags uint64.
4. Filter count uint32. Each row: filter ID uint64, source-kind uint32, source ID
   uint64, included flags uint64, excluded flags uint64, override count uint32.
   Each override: area ID uint64 and traversal cost float32.

Tables use ascending stable IDs, including overrides. Duplicate identities or
invalid references reject through the existing profile/area registry validators.
Decode then canonical re-encode rejects alternate orderings, negative zero and
trailing bytes. All counts are checked against remaining bytes before allocation.

Qualified bounds are 64 profiles, 256 areas, 256 filters, 256 overrides per
filter, 256 UTF-8 bytes per profile name (no embedded NUL), 4096 cells per tile
axis and 65536 tiles per complete closure. Profiles/areas/filters are nonempty;
coordinate and scope enums are closed. Existing finite grounded geometry and
cost validation still applies. The payload cannot exceed the existing 1 MiB
record ceiling. Host service budgets may further restrict these source limits;
they never widen them or invent missing authored policy.

## Migration and consumers

Old unknown bytes are not NAVDEF01. Migration preserves optional unknown records
inert and rejects required unsupported semantics with explicit fidelity evidence.
It never manufactures profiles or treats generated cache contents as definition
truth. New definitions are written only under the new registered project
contract. The frozen 0.1.0 contract is not changed to describe this record.

Scene authoring still owns surface IDs, sidecar references, grounded profile
selection, object-subtree/local-bounds scope, regions, modifiers and ordered links.
Definition source scope is distinct from the selected-surface automation filter.
Publication must retain the complete definition tile closure, including unaffected
surfaces; incremental selection is not permission to drop other surfaces.

The existing headless source-extraction configuration built HoroNavigationApiTests;
all five focused definition cases passed. The attempted combined build then
failed at two incorrectly requested standalone consumer target names: those names
are not declared, so no consumer pass is claimed. The established ownership
generator emits one source per header into HoroNavigationApiPublicHeaderConsumer;
both new definition headers are present in that real target. A fresh manager
grant built that exact target successfully, including both new header translation
units; the slot was released. No reconfigure, unchanged Scene tests or broad
matrix were repeated in the corrected consumer-only pass.
