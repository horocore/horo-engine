# Prefab asset browser thumbnails

The browser recognizes a prefab only from a registered, exact `AssetTypeId` of
`core.prefab`. Names and legacy filename hints do not grant prefab authority.
Unregistered files stay visible with an identity-metadata diagnostic. Search and
exact type filtering use the existing immutable directory projection. Activation
requests embedded source opening; the HORO-1055 persistent document route owns
identity, dirty state, history, recovery and conflict handling.

`PrefabAssetPreview` uses the existing host `AssetPreviewService`, rather than a
second job system or render backend. A worker captures the real prefab closure
through the explicit host source capability and its cooperative project lease.
CPU rasterization releases that lease; completion recaptures and compares the
exact closure before success, allowing source saves during rendering.
The canonical resolver expands nesting/variants, and the canonical Scene codec
projects portable built-in primitive geometry. Complete transformed indexed
triangles are rasterized with a fixed isometric camera and depth buffer. The
result is uploaded by the existing selected GUI renderer on its owner thread.
No native graphics object or live document crosses the worker boundary. Sources
without supported renderable geometry retain the generic card and an explicit
preview-unavailable diagnostic; their authored data stays intact.

The browser retains at most 32 pending observations and serializes prefab source
captures within the workspace to avoid competing for its project lease. The
existing preview service separately bounds outstanding work, source bytes and
cached image bytes. Each refresh replaces the directory generation; requests are
keyed by source bytes, pinned registry revision and directory generation, so a
nested source edit cannot reuse an earlier directory's thumbnail. Completion is
fenced by that generation. Source save/reload retires existing images and requests
a new generation, including source replacements with uncertain durability and
partial multi-document saves. Providers verify the complete captured source closure
before success. Cooperative project-lock contention retries without presenting
an asset as malformed.

Rendering admits 16 through 256 pixels per dimension, at most 20000 triangles,
and 2000000 candidate pixel samples per thumbnail. Cancellation is checked during
capture, object expansion, triangle collection and every raster scanline. Source,
codec and migration diagnostics remain typed and appear in localized secondary
copy and diagnostic tooltips. Over-budget work produces an explicit diagnostic.

Navigation cancels retired requests. Controller destruction explicitly shuts down
and joins the preview service before document and host capability retirement.
Card texture ownership remains with `AssetBrowserCardRenderer` and its selected
GUI bridge; detach releases those resources on the renderer owner thread.

The internal model gains thumbnail status/diagnostics and a typed classification
helper. Existing importer preview providers retain their contract. No public
header or ownership registry changes are required. The existing non-installed
`HoroPrefabHierarchyInternal` exact-header interface stages `PrefabAssetPreview.h`
for GUI/test consumers; a named include guard also protects mixed source/staged
header inclusion. No broad include root is added.
