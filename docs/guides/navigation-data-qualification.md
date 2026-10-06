# Navigation data and authoring model qualification

`HoroNavigationDataQualificationTests` and, when the pinned native provider is
enabled, `HoroNavigationAssetDataQualificationTests` exercise one shared authored
corpus through the existing four representation boundaries. They add no source
migration, provider selection, runtime fallback, or competing identity authority.

The corpus has two grounded profiles, one project-owned area and filter, explicit
metric coordinates, stable collision producer/contribution identities, and an
eight-tile limit. Its NAVDEF01 1.0 payload is 201 bytes with SHA-256
`f222d1ed83d2e016d04ea81c238cbc964f00b2c570e51e041bd5e13ef93d2586`.
That reference was independently assembled from the documented little-endian
fields, binary32 measures, and exact UTF-8 names, rather than captured from the
encoder being tested. Reordered authoring and surface inputs must retain those
bytes and the same capture fingerprint.

The portable suite covers production HNAV/NAVDEF01 round trips, every truncated
semantic payload prefix, hostile counts and lengths, lowered payload ceilings,
checksum corruption, unsupported source/payload versions, and inert/foreign
records. Each rejected corpus member asserts its exact error domain and code;
the previous immutable source remains byte-identical. Prefab qualification uses
the real resolver, stable Scene identity remap, typed component projection and
expansion. Renaming the source path/display label and reloading its revision must
retain the Scene object, surface and definition AssetId identities and the exact
multi-profile Scene asset requirement.

The native suite starts with that same decoded definition and captured collision
geometry, builds real Recast tiles, validates HNT1/HNS1 closure encoding, and loads
through the existing canonical asset adapter. It checks exact profile/surface,
source provenance and actual-byte tile content identity. Malformed inner closure,
unsupported framing and lowered partition/decoded storage ceilings fail before
admitting any tile cache state. Actual Scene service reload tests preserve the
previous Scene and query-pinned world after malformed replacement or an explicit
provider-unavailable result. A missing host factory fails closed with the current
`CapabilityUnavailable` diagnostic and publishes no Scene/world or fallback backend.

## Diagnostic correction and caller migration

Previously the host adapter combined absent composition with shutdown/invalid
capacity in one guard and returned `CapacityExceeded` for all three. An absent
factory now reports the existing `navigation.capability.unavailable` code after
the unchanged bounds/shutdown guard. Invalid capacity retains its previous code;
errors returned by an explicit factory, including `ProviderFailed`, retain their
original domain/code and context. No layout, signature, ABI, ownership registry or
target dependency changes are required. Callers of
`NavigationAssetSceneActivationParticipant::Prepare`, including aggregate Scene
activation, should present missing provider composition as unavailable instead of
advising a larger memory budget. The actual Scene-service regressions cover both
the corrected composition error and preservation of factory error provenance.

## Measurement scope

The existing test-only `AllocationProbe` implements portable ordinary and aligned
C++ allocation/deallocation pairs. A stack-owned, owner-thread observation window
is active only around the production decode or load call; fixture construction,
assertions, formatting, reporting and other threads are outside the window.
The qualification verifies both ordinary/aligned requests and nested window
isolation before consuming its measurements.

Reports record workload, configured build mode/platform, encoded bytes, C++ request
count, cumulative requested bytes and largest request. Native loading additionally
reports validated tile `StorageBytes()` and cache capacity accounting. Cumulative
requests are **not peak resident memory**; direct C allocation/native provider
allocation, allocator metadata, shared-control blocks and other threads are not
fully observed. Provider cooking happens outside the parser/loader window. These
measurements do not establish a bound for every transient native allocation.

Hostile parser requests must stay within the corpus's explicit 4 KiB largest and
16 KiB cumulative C++ request ceilings; native loader requests stay within 64 KiB
largest and 256 KiB cumulative ceilings. Encoded and decoded limits and no-partial-
publication assertions independently qualify the format/admission contract.
Actual figures are emitted by the executable and retained in CTest's test log;
they must be reported only after that configured batch has run.

## Validation

Build both qualification targets with native navigation enabled, and run their
CTest entries together with the existing Navigation API/runtime, canonical asset
loading and Prefab Scene expansion suites. Use serial build/test execution when
required by the host resource gate. The portable suite also remains available in
a provider-omitted composition; no native result may be claimed for that omitted
configuration. The ordinary/aligned probe implementation retains the platform's
matching `malloc/free`, `posix_memalign/free` or `_aligned_malloc/_aligned_free`
pairs and stays outside all installed engine APIs.
