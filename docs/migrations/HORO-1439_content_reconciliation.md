# HORO-1439 content reconciliation migration

Runtime owns the existing-format container writer and schema-2 canonical codec. RuntimeScene owns mounted-content reconciliation, saved-scene queue admission and content-bound capture. GameplayApi owns the private installation receipt, issued only by an actual loaded GameplayModuleHost module. RuntimeScene does not depend on ModuleHost or Editor.

The saved-scene preparation factory requires owned reconciliation evidence and an explicit host baseline decoder receiving actual provider-loaded cooked bytes. Shipping cooked-scene decoding and aggregate application save composition are absent; host composition must supply its real format decoder and register the bound declaration adapter. This backend API is not automatically installed in a shipping host.

Scene queue admission now issues a retained publication receipt. Only actual aggregate transfer publishes SceneRuntimeId/revision; its snapshot is observational. Existing ordinary/cell-payload callers translate the result to their prior void contract. Empty/moved receipts are invalid; allocation precedes pending-work replacement. Access is owner-thread only, and successful identity survives Scene retirement. No callback/allocation occurs in receipt transfer. Failure/cancellation/shutdown cannot publish an identity.

Prepared candidates default to Unqualified dataset projection. Actual Physics/AI/Navigation roots that own no persistent datasets explicitly report Absent. Content-owned queue rejects unqualified/persistent projections before publication; ordinary queue retains compatibility. PersistentWorld capture bindings need an actual dataset identity producer and are explicitly unsupported. IDs are never manufactured. Native receipt observations do not replace callback admission: new entry uses the existing owner-thread boundary; accepted work retains actual native/adapters until completion.

## Semantic schema 2

Existing container versions 1/2 and participant schemas remain independent. Semantic schema 1 requires explicit trusted legacy policy, and its hash is never reused for changed captures. Schema 2 retains hash domain `HoroSave.CanonicalState.v1` with explicit UInt32 semantic version 2 inside the canonical stream.

The bounded stream encodes length-delimited project/world/base-scene UUID bytes, UInt32 version 2, an empty dataset sequence qualified by actual prepared composition/capture scopes, then owner-sorted participant tuples. Each contains UTF8 owner, UInt32 participant schema and record-sorted tuples of UUID bytes, UInt8 representation and payload. Known tag 1 contains exact canonical bytes. Opaque tag 2 contains UInt16 codec, UInt64 stored length, UInt64 decoded length, UInt32 alignment, decoded SHA-256 bytes and exact stored bytes. This asserts storage preservation rather than decoded semantic equivalence.

Required owner `horo.save.scene.canonical.v2`, participant schema 1, record `2d576609-77ae-4fb2-bf4a-9e7b63cbc002` stores UInt32 layout version 1 plus a record-sorted sequence of UUID bytes/UInt8 representation. It covers every directory entry exactly once, including itself as Known. Required owners cannot be Opaque. Layout bytes participate in the canonical hash; missing/extra/duplicate/conflicting/self-opaque/version-mismatched data fails before host callbacks. Persisted classification survives later codec support changes. The required independently decoded content declaration cannot be bypassed by layout tags.

Accepted safe-point snapshots privately seal actual world/source admission facts and can re-save after live retirement without new callbacks. Combined capture and retained storage/known-decoded sizes are admitted before copies or decompression. Production reader/hash admission and exact unknown-data byte/digest/codec/alignment verification precede exposing finalized output.

## Validation status

Source and regressions are in progress. Intermediate compilation is not final acceptance. Independent schema fixtures, compressed retention budgets, actual capture/re-save/native aggregate lifetimes and malformed layouts remain required before delivery.

### Gameplay installation header ownership

`GameplayPersistenceInstallation` is declared in the GameplayApi-owned
`Horo/Gameplay/PersistenceInstallation.h`. RuntimeScene consumes only its frozen
persistence descriptor, generic canonical capture adapter, and exact native
admission/lifetime pin. `GameplayPersistenceAdapter` and staged restore remain in
GameplayRuntime. Hosts include the new header when retaining installation receipts;
only actual `LoadedGameModule::AcquireInstalledPersistence` can construct them.
The descriptor aliases the immutable adapter declaration and shares its native
lifetime; generic adapter factories cannot create installation authority.

The updated main moved Scene lifecycle tests into `RuntimeSceneLifecycleTests.cpp`;
structural revision assertions follow that owner, while publication receipt cases
remain in `RuntimeSceneTests.cpp`. Both compile in the owning Scene test target.

### Detached accepted snapshots and work admission

Only the owner safe-point capture path seals the privately published Scene,
Dataset-absent composition, exact source identities and capture provenance.
The seal is allocated before participant callbacks. Background `ReSave` uses
only immutable captured records and const aliases of source archive, layout,
retention policy and unknown payloads; it never observes live Scene receipts,
installation controls or module admission. Const aliases retain source/native
storage without copying all retained payloads for each capture. Existing reader
selection work is debited through its atomic work ledger.

Re-save admission conservatively reserves 32 passes of payload and framing/JSON
bytes, plus 128 KiB per record for bounded participant lookup, sorting and tuple
work. Header allowance covers maximum six-byte JSON escaping of actual UTF-8
provenance strings, bounded identity/key overhead and permitted padding;
manifest allowance covers all bounded owner names and UUID references. Both
full reader admissions, logical validation and retained-byte comparison are
included. This is a deliberately conservative admission bound, not measured CPU
work or an exact minimum. Tight limits may reject a valid archive before
materialization even when an individual downstream reader would admit it.
Known retained compressed records also debit decoded bytes against the **new**
re-save limits before selection; opaque frames do not require decoding.

The 40-pass allowance is the sum of bounded whole-pipeline byte visits, with
unused margin rather than a claim that every stage always traverses all bytes:

| Stage | Conservative full-byte visits |
| --- | ---: |
| Capture segment assembly or retained stored-byte copy | 1 |
| Stored/decoded digest and source selection (selection also separately debited) | 3 |
| Schema-2 known/opaque/layout stream materialization and domain hashing | 4 |
| Container assembly and integrity hashing | 3 |
| Writer's production-reader admission (existing reader charges 3 archive passes) | 3 |
| Re-save's independent production-reader admission | 3 |
| Logical readback selection, canonical reconstruction and hash | 5 |
| Source/destination unknown retention copy and exact comparison | 4 |
| Source/candidate canonical preservation authority reconstruction, raw hash and metadata copy | 8 |
| Header/manifest serialization, normalization and bounded padding margin | 6 |
| Total reserved | 40 |

Directory/frame bytes and metadata are charged independently before payload
materialization. Maximum owner count is 256, owner identity length 96 bytes,
record count 16,384, and each UUID is fixed-width. A 128-KiB per-record allowance
covers bounded owner lookup, stable sorting and repeated tuple/directory visits;
manifest allowance is 2,048 bytes per possible owner and 64 per reference, plus
fixed framing. Six-byte JSON escape expansion uses actual UTF-8 provenance
string lengths; maximum permitted header padding is separately included.
These bounds cover native records by the same admitted immutable record sizes.
They deliberately overcharge overlapping traversal and unused metadata capacity.

### Logical baseline substitution policy

A typed compatible cooked substitution resolves physical installation only. The
original logical asset/base-scene ID and expected source digest remain in the
required declaration and accepted header seal. Every later source admission must
apply the explicit project mapping again and verify the actual replacement
provider envelope; capture never silently migrates persistent IDs. The host
baseline decoder receives that admitted replacement artifact and still produces
the exact expected Scene definition/revision. Semantic schema-1 archives with an
existing declaration enforce it as well; the legacy fallback cannot ignore a
real declared substitution or re-resolve an already admitted baseline as absent.

Direct optional DLC fixtures exercise explicit Preserve/Quarantine decisions,
required-missing rejection before the decoder, degraded capture denial before
callbacks and retained byte/digest/codec identity after real capture/re-save.

### Diagnostic lifetime and host presentation

`SaveContentDiagnostic` retains the exact typed requirement, resolved disposition,
replacement identity and typed remedy. `SaveContentWorld::Diagnostics` pins these
immutable decisions through the published owner. Accepted snapshots retain the
same frozen decisions in their private seal, so worker encoding after world
retirement can expose diagnostics without reading live installation/Scene state.
Missing content returns the existing typed error plus an exact owner/content
identity diagnostic (`scene.save_content.install_compatible`, mapping to the
`InstallCompatibleContent` remedy) and installation/optional-preservation guidance. These are
presentation facts; observers never install content or change project policy.

Roundtrip inspection extends the source policy only with exact owners/schema
versions actually present as Captured in the privately admitted snapshot. Existing
source version policy is never broadened; unsupported captured schemas reject.
Capture/retention overlap is rejected before callbacks, so an opaque source owner
cannot become known through this path. New capture owners are not required in the
older source archive. The generated canonical layout remains explicitly supported.

The container assembler rejects inconsistent Raw chunk digests before building
archive storage. It reserves one stored-byte visit for each raw digest and passes
only the remaining read-work allowance to production reader admission. Unknown
and compressed chunks remain undecoded in this low-level assembly seam; their
source retention and known semantic validation remain owned by reconciliation.
Independent writer tests qualify duplicate/empty input, stale raw digest and the
exact raw-digest plus three-pass reader work boundary.

### Unsupported optional storage codecs

Container v2 plus semantic schema 2 permits structurally bounded unknown codec
chunks only under an actual optional manifest owner. This does not change the
public `ValidateSaveChunkDirectory` default or permit unknown metadata, required
owners, v1 or schema 1. `SelectChunk` remains unsupported and never decodes such
storage. The canonical layout must authenticate opaque classification and exact
stored bytes before content admission; release changes cannot reinterpret them as
known records. Consumers retain bytes through the minted canonical-preservation authority and the sealed
content capture/re-save path, not through a decoded selection.

### Capture allocation failure boundary

Capture prepares owned typed allocation-failure storage before admission mutates
world/barrier state. That preparation can throw `std::bad_alloc`; it invokes no
participant and consumes no barrier readiness. Once prepared, actual capture-work
allocation failures return the moved `CanonicalCodecAllocationFailed` result;
the unwind handler performs no error construction or allocation. Public capture
is not declared `noexcept`. Regression injection distinguishes throwing fallback
preparation from typed participant-ID preflight failure, observes zero allocations
after the latter fault, and proves unchanged published world/zero callbacks plus
a valid retry of the same requested barrier. Failure-count calibration belongs
only to test setup and does not influence production admission.

Schema-2 opaque preservation uses `ValidatedSaveSceneCanonicalPreservation`, an
owned, non-fabricable Save authority minted only after authenticating the entire
required canonical layout and state hash. Borrowed-reader storage is rejected;
accepted proof values pin immutable archive backing. Raw hash/copy work is charged
on the shared reader ledger before materialization. Owner recognition in a later
release never grants a decoder for an authenticated Opaque record. Required and
Known records keep actual supported decode/hash verification; the generic reader
inspection and round-trip APIs remain strict. Content reconciliation and re-save
use the same qualified source/candidate path and compare exact stored bytes and
directory metadata. This path never permits dropping retained optional records.
