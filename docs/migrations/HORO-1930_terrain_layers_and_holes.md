# HORO-1930 / #1974: terrain layer painting and holes

The new authoring source container has its own schema 1. It does not change the
engine project version, RAW-F32 input, HTIL, HTMF, HTSG or HTPM formats. Its prefix
starts with a reserved NaN representation, so valid finite RAW-F32 cannot be
mistaken for this container. Existing height assets retain their original source
representation until an explicit clean conversion.

The complete container owns heights, all normalized weight channels, binary holes,
coordinates, semantic source revision, the ordered layer-set binding, and lossless
authored foliage IDs/transforms with their used definition-source bindings. Its
length-delimited foliage section delegates validation and canonical encoding to
`FoliageAuthoringSource`; it does not duplicate that wire format. Combined sample,
channel, foliage and integrity work is admitted before bulk allocation. Layer-set
source has its own bounded schema 1 with real material AssetIds. Four active paint
targets can select any of the sixteen stored layers. No inactive channel is dropped.

Conversion prepares a fresh document session and advances the source revision
before native publication. Passing no new layer binding preserves all existing
channels and binding. Selecting a different binding explicitly initializes the
new ordered channels with full weight in the first layer. Historical import
metadata remains byte-for-byte unchanged and fenced. Payload-only atomic source
replacement retains AssetId; directory-sync uncertainty retains the new owner and
publication identity while keeping it dirty. The document remains the sole source
mutation/history authority. Workspace sessions, host capability tokens, registry
epochs and cook grants are not persisted source authority.

`IAssetImporter::ValidateReimportDestination` is a new C++ virtual admission hook.
Generic reimport invokes it on a bounded capture of the current destination before
reading historical upstream input or staging files. Existing implementations
inherit success; terrain RAW rejects an active authoring-container prefix, so old
RAW provenance cannot erase authored channels or placements. Rebuild C++ importer
implementations and consumers together; retaining old C++ vtables across hot reload
is unsupported. Extension C transport tables are unchanged; their host-side C++
adapters rebuild normally. The actual external path is
`HoroAssetImporterDescriptor`/`HoroRegisterAssetImporterFunc` in
`include/Horo/Extensions/ExtensionAbi.h`: it passes C callbacks and opaque context,
not an `IAssetImporter` object. `RegisterExternalAssetImporter` validates table
size/version, copies the descriptor and constructs `ExternalAssetImporter` inside
the host. Package activation uses that same ExtensionManager adapter. Thus this
C++ virtual addition does not change the external importer ABI or Gameplay SDK
boundary; direct C++ implementations must rebuild. This adds no Terrain dependency
to Assets and performs no callback or activation during contribution-descriptor
creation/validation.

Reimport retains both original payload and metadata identities under native writer
leases from admission through publication and compensation. Rollback copies come
from those admitted captures, and each replacement uses the same canonical source
publication primitive with the original identity. Foreign replacements are never
restored over. The explicit legacy reimport policy admits up to 1 GiB and streams
identity checks; ordinary source capture/publication keeps its existing 64 MiB
ceiling. Registry publication is the commit barrier: a later index persistence
failure retains the new pair and registry generation and reports durability
uncertainty. Precommit failure compensates only bytes still matching this
operation’s promoted identities. Native writer leases coordinate cooperating
writers; this is neither a crash-atomic two-file transaction nor a filesystem
content compare-and-swap against external writers after the final check.

`TerrainAuthoringSourceCodec.h` and `FoliageAuthoringSource.h` are owned by
`HoroTerrainImport`. The previous `Editor::TerrainAuthoredPlacement` spelling is a
source alias of the neutral `Terrain::TerrainAuthoredPlacement` carrier; rebuild
C++ consumers because its canonical type name changes. Source, paint and foliage
consumers link the actual owning targets. Public Application cook/creation headers
belong exclusively to `HoroTerrainAuthoringApplication`, which composes existing
Assets, Terrain producers and project mutation admission. GUI and executable
composition link that bridge explicitly. Dedicated public consumers cover these
boundaries without publishing a repository-wide include root.

The offline cook pins real source/identity pairs for terrain, its layer set, used
foliage definitions and their actual mesh/material source references. Producer
`InputDependencies` and cache selections include this complete source closure.
Native source capture/cache/publication runs on one feature-owned control thread;
shared JobSystem CPU jobs create actual visual, collision and navigation geometry.
The new private HTAB v1 bundle contains those canonical manifests/tiles/roles and
sealed authored-foliage placement evidence. Material source hashes are never
represented as compiled material artifact hashes. Domain cache validation derives
the bounded canonical output again, including on cache hits; it is tooling CPU
work and is not claimed to avoid that cost.

Authored foliage uses a cluster span equal to the maximum cell extent of its
actual verified terrain tile, rather than a fixed span that may exceed the grid.
The canonical cooker still determines closed far-edge cluster identities. This
policy has a named version in the profile digest, and the authoring cook strategy
advances to geometry v2 so earlier fixed-span cache entries cannot alias it.

The UI never joins during Draw/Poll. One attempt and its completion are retained;
cancellation/replacement revokes adoption immediately, including when native I/O
has already committed an old result. Teardown drains the control thread and all
joined CPU children before releasing registry, mutation, filesystem or JobSystem
services. Source owner borrows never enter worker captures. Catalogue admission is
explicitly bounded and rejects ambiguous semantic type/layer identities; it does
not retain a truncated prefix. Existing document limits still bound exact patch
history. Failure keeps the accepted source and history unless a truthful native
publication receipt says a replacement already occurred.

There is no built-in persistent material-source producer/cooker in this repository.
Layer-set and foliage creation require actual project-provided tracked
`core.material` assets; foliage also requires actual `core.mesh` assets. Missing
references disable or reject creation. Opaque test material sources establish only
tracked identity and captured bytes, not material semantics or compiled readiness.
These editor/source features do not claim native rendering, Physics/Nav activation,
foliage culling, a material pipeline, or manual visual validation.
