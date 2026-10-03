# Terrain Source Import Contract

TRF-002.2 owns source format interpretation and canonical authoring normalization.
`HoroTerrainImport` is a tooling/assets contribution. It reads borrowed bytes under
captured byte, sample, layer and work ceilings and returns a detached source. The
caller owns tracked source bytes and `AssetId`; the candidate retains the typed
canonical asset ID, dataset ID, source revision and captured capability revision.
The terrain document owns the only mutable canonical source revision. Import performs no file I/O, asset registration,
cook, runtime activation or automatic fallback.

## Supported source formats

| Channel | Built-in encodings | Exact interpretation |
|---|---|---|
| Height | tightly packed RAW U16, RAW F32, grayscale PNG 16-bit | One scalar per sample. RAW byte order and row direction are explicit; PNG samples are decoded to canonical little-endian values. Finite height scale and offset produce float32 meters under a caller-selected maximum absolute precision error. |
| Weight | tightly packed RAW U8/U16, grayscale PNG 8/16-bit | One raster per authored layer. Each pixel must have positive total weight. Integer largest-remainder normalization makes the layer sum exactly 65535, with ties assigned by layer index. |
| Hole | tightly packed RAW U8, grayscale PNG 8-bit | Exact zero means solid; exact one means hole. Other values fail. |

These encodings use the existing vendored `stb_image` PNG decoder and require no
new platform image dependency. PNG input must be non-interlaced single-channel
grayscale with a declared 2D shape; palette, RGB/RGBA, grayscale-alpha, sub-8-bit,
and interlaced PNG fail with a typed unsupported-format result. RAW has no header,
so callers must supply width, height, scalar format, byte order and row order.
EXR, GeoTIFF, Houdini HeightField and other containers require an explicitly
registered decoder contribution or a pre-conversion to one supported encoding.
The import API returns `UnsupportedFormat` when no contribution is supplied; it
never interprets a filename or substitutes a different format.

`ITerrainRasterDecoder` is the optional host-pinned contribution seam. It probes
an exact format ID and borrowed bytes for the output scalar format and byte count.
The host validates that claim and allocates bounded output before invoking the
decoder with a writable span and cancellation token. The same shape, role, numeric
and budget validation runs after contribution decode. Host composition may adapt
the contribution to its importer catalog; this module creates no second catalog
or publication authority.

Local coordinates and projected meter coordinates with explicit `EPSG:<digits>`
provenance are supported. The importer preserves that CRS and finite origin,
spacing, scale and offset. Geographic degrees require upstream projection and
return `InvalidCoordinates`; import does not silently infer a CRS, reproject, flip
rows or guess nodata. Non-finite values, unsupported precision, mismatched raster
shapes, malformed byte lengths and exceeded budgets return distinct actionable
typed errors before publication.

`TerrainSourceDocument::Publish` accepts the complete candidate only when its
dataset and canonical asset identities match, its capability revision remains
current, its source revision strictly advances and the caller supplies
the exact prior revision. First insertion requires no expected revision. Failure,
cancellation, supersession and close leave the last source intact. Cooked content
and runtime revisions remain separate from `TerrainSourceRevision`.
