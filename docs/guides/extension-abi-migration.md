# Native extension ABI negotiation

ABI 1.1 keeps the v1 host prefix and load/unload symbols while adding an optional
inert `horo_extension_query` bootstrap. The host negotiates before calling load;
an incompatible query cannot receive a registration callback. Missing queries
select the legacy 1.0 contract, not an inferred newer ABI.

## Version and size rules

- `HoroExtensionHostApi::abiVersion` remains the major version, currently 1.
  The appended `abiMinorVersion` is 3; `reserved` is zero. Earlier minor
  requirements of 0, 1 and 2 still negotiate against their original prefixes.
- A query fills `HoroExtensionRequirements` in host-owned storage. It must return
  the complete supported requirements size, matching major, a minimum minor no
  greater than the host's, and a host size the host can supply.
- Required function bits are explicit. Unknown bits and nonzero reserved fields
  fail negotiation; requested callbacks must be present.
- Input-table tails may be appended. Modules must test size before reading new
  fields, and must still validate their requirements in load when supporting an
  older host that does not call query. No field is reordered or repurposed.
- Output `structSize` starts as writable capacity. A module must not write past
  that capacity and returns its populated prefix size. Module results may end
  before `moduleId`, before `moduleVersion`, or after the complete current table.
  Absent legacy identities are cleared and remain manifest-owned. Partial fields
  and sizes beyond supplied capacity fail before contribution commit.
- Query is metadata-only: no allocations, lifecycle state, retained pointers,
  callbacks into host services, or registration. Query errors propagate and
  contract-violating exceptions are contained by the host.

## Source compatibility and ownership

The public header now compiles as C11 and C++20. C `typedef` is intentional; the
C++-only Sonar preference for `using` is inapplicable to this shared header.
Status and setting tags use `uint32_t` rather than compiler-sized enum wire
types. Constant names and values are unchanged. Code that explicitly spells the
old enum tags must use the public typedef names instead. Rebuild native modules
with the updated SDK; binaries built with nonstandard short-enum options are not
covered by compatibility guarantees.

The existing example, host adapter, C consumer, and C++ public-header consumer
are affected callers. The C consumer is a registered CTest target, not a C++ test
that merely includes the header.

Importer context ownership requires a module-provided destroy callback whenever
the context is non-null. A rejected registration does not transfer ownership;
the module must clean it up. Accepted contexts retain the module lease and are
destroyed through the originating module, including transaction rollback.

This bootstrap is not a sandbox or package trust decision. It cannot prevent a
malicious native module from ignoring buffer bounds or accessing process memory.

## Platform provider operation profile migration (ABI 1.3)

The host minor is now 3; the major and all 1.0/1.1/1.2 host-table prefixes are
unchanged. Existing 1.2 provider modules keep `abiVersion == 1` and set
`HoroPlatformServicesProviderDescriptor::structSize` to
`offsetof(HoroPlatformServicesProviderDescriptor, operations)`. Recompiling an
old initializer with the new `sizeof` would incorrectly advertise a tail it does
not implement. The host validates that original prefix and never reads the tail
from a version-1 descriptor.

To use the operation lifecycle, a native module's inert query requires host minor
3, then load registers descriptor `abiVersion == 2` with the full size and a
non-null `HoroPlatformProviderOperations` table at its exact version-1 table
size. The host copies the function pointers during registration. The module
retains ownership of its code and candidate. It keeps every host sink pointer
valid only through close-ingress and drain; drain may return BUSY and must be
retried on the owner lane before candidate retire/destroy. Callback payloads are
borrowed only for one call and copied into the host's finite queue. Old
factory-only modules remain loadable but exact operation-host startup reports
`platform.lifecycle.unsupported_profile`.

The affected callers are the extension registration adapter, the exact
capability resolver, provider admission, and the narrow Platform Services
lifecycle host. Native C fixture modes 10 and 11 exercise old-prefix loading
and the appended operation profile respectively; service-host tests cover
startup rollback, BUSY drain, off-thread callback ingress and repeat teardown.

## Platform Services completion error migration

Provider operation callbacks must set `HoroPlatformProviderCompletion::resultCode`
to a `HoroPlatformProviderResultCode` value. Translate SDK results inside the
private provider package. Zero remains success; values 1 through 10 represent
stable provider failure categories; 11, 12 and 13 represent cancellation,
timeout and capability absence. Unknown native results use
`HORO_PLATFORM_PROVIDER_UNKNOWN_FAILURE`. The host also treats unrecognized wire
values as Unknown. Never place native error text, credentials, account IDs or
response bodies in a failure payload. Host processing discards failure payloads.
The completion struct layout is unchanged, so ABI 1.3 providers only need to
update the semantic mapping of `resultCode`.

## External C ABI 1.4 → 1.5 settings and modal pages

ABI 1.5 appends `registerEditorPage` to `HoroExtensionHostApi`. It does not
change any 1.0–1.4 prefix, activity transport, or GameModule SDK21 layout.
Existing activity modules must continue to request the 1.4 prefix ending at
`offsetof(HoroExtensionHostApi, registerEditorPage)`, rather than recompiling an
old initializer with the new full `sizeof`. Rebuild page modules with the
packaged `EditorPageAbi.h`; query minor5, `HORO_EXTENSION_REQUIRES_EDITOR_PAGE`,
full host-table size and explicit `editor.page` capability. Load must repeat
size/version/null checks before dereferencing the new tail. Rollback removes
page claims/callbacks and uses the unchanged activity profile on an older host;
it never treats a settings/modal page as an activity or persistent document tab.

A proper explicit1.4 query receives only its original host prefix and no page callback. Page registration additionally requires the validated minor5/page-function/complete-prefix proof at ingress; manifest claims or a composed GUI alone cannot grant transport. The actual C11 legacy-query regression verifies that neither Settings nor Modal claims publish through the old negotiation.

Registration is staging only. The host copies definitions/messages and checks
manifest ownership, requested/approved permissions and exact capability grants
before transactional commit. Session `open`/`close` are owner-lane operations;
opening during module load fails. The host owns draft, focus, cancellation,
validation and active settings. Callback payloads and cancellation views are
borrowed for that call only. Native callbacks run on attributed jobs with code
and capability leases; revocation closes admission before draining callbacks.

Page schema1 supports bool, exact int64 and bounded single-line UTF-8 strings,
text/label/action and Stack/Group layout. Medium/Neutral styling and automatic
focus are the supported presentation profile. Row/Grid, path/choice, decimal,
color/vector, multiline and confirmation actions fail registration explicitly.
Integer fields use decimal text end-to-end, including signed endpoints and
values above2^53; converting them through double is invalid.

Settings pages choose one canonical User/Project/Workspace scope. The real GUI
composition supplies the existing durable filesystem and explicit selected
project/configuration roots. Apply validates the whole candidate and provider
findings, durably writes the canonical snapshot, then performs a noexcept active
swap. Any uncertain write retains dirty `RecoveryRequired` state and original
error until explicit reload. Modal fields are Session scoped, validate then
submit, and never persist. Open/draft/focus/modal stack data is never workspace
state. Reset/cancel do not write. Parent/child workflows require exact provider
and active parent identity; the existing modal host owns focus/input exclusivity.

Affected callers include `ExtensionManager`, native activation/retirement,
`EditorSurfaceRegistry`, `GuiScreenHost`, Settings/Modal host adapters, SDK
packaging/reconfigure/external C11 consumers and native page fixtures. All host
C++ consumers rebuild against the new additive controller contract. The GUI
adapter consumes validated `BuildEditorUiRenderSnapshot` from the actual shared
design tokens/fonts. A private owner-boundary structural capture generation
tracks real theme changes; it is neither persisted nor a second theme authority.

## Page transport admission and filled icon geometry repair

The ABI1.5 wire layouts and versions remain unchanged. `EditorPageAbi.h` now
owns the allocation-free C11 structural checks shared by the actual C11 page
producer and the host ingress/callback adapter. These check complete prefixes
before tail fields, bounded array extents and pointer/count pairs, exact selected
value kinds and integer limits. They do not validate arbitrary foreign memory,
UTF-8 content, manifest ownership, permissions, duplicate identities or FormKit
semantics; the existing host copy/admission validation remains mandatory.
Checked invocation retains the caller-held executable lease and exception
boundary. C11 modules can use the checked session/finding helpers without
retaining call-lifetime host views. The actual native fixture reports a localized
negative-value finding, proving the real sink prevents durable publication.
Existing modules need no binary migration; rebuilt sources can reuse these same
checks. Old negotiated1.4 sessions still cannot access the appended page callback.

The theme frame's `icons.strokeWidth` now admits finite zero to represent a filled
font glyph without an outline. Negative and nonfinite widths remain invalid.
The GUI bridge publishes that zero explicitly; `ImGuiStyle::FrameBorderSize`
continues to describe control focus/border interaction and is not icon authority.
The semantic glyph support mask is retained. This is a validity-contract expansion
without a layout/version change; external host/theme validators should rebuild
and accept zero rather than substitute an invented positive outline. Affected
callers are the real GUI theme capture, canonical theme validator, public theme
header consumers and page view/render tests.


### Settings modal C++ composition with navigation

`SettingsModal` now captures both the active navigation session's revocable borrow
and the explicitly composed extension page host/theme. The sixth optional argument
remains the navigation session pointer. The seventh argument is
`SettingsModalPagePresentation`, pairing the shared page host and captured theme.
Update page-enabled callers to supply the navigation argument (or `nullptr`)
before that presentation aggregate, and rebuild GUI consumers. The constructor is fallible when it creates an owned page view;
its allocation can throw instead of satisfying the earlier navigation-only
`noexcept` contract. This avoids two constructor paths with different authorities.

`GuiScreenHost` owns the common settings factory for menu actions, Welcome-screen
requests, and provider-selected pages. Each path captures the current navigation
borrow and admission diagnostics together with the actual page/theme authority.
Modal lifecycle tests cover navigation retirement, while real host menu tests
retain original navigation admission failures. Native page ABI 1.5 is unchanged.
