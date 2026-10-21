# Runtime UI layout inspection and responsive presentation

HORO-821 (#821, [RUI-013.2]) adds copied inspection evidence and bounded responsive
layout alternatives to the existing `UiAnimationOwner`. The responsive design
proposal and ADR-073 remain **Proposed**; this guide describes the implemented
public contracts without changing their decision status.

## Source and cooked migration

`UiDocument` source 1.4 adds `presentationProfiles`, a canonical array of tables
keyed by authored canvas. Each table has a nonzero revision and finite rows with
unique IDs and priorities. A row contains typed extent/aspect/form/view/capability
selectors and complete layout styles for its target elements. Priorities are
ordered ascending; the first matching row wins. No match selects the neutral base.
There are at most 32 rows and 4096 overrides across a document. Invalid, duplicate,
foreign-canvas, unsupported and oversized inputs fail admission.

Cooked document format 4 appends a length-prefixed canonical profile table after
the existing sections. Formats 1, 2 and 3 decode explicitly to the neutral base;
they contain no profile trailer. Older source versions omit profiles and remain
neutral. The serialized profile trailer shares the existing 4 MiB JSON parser ceiling;
the separate raw table admission byte ceiling does not waive that limit. Profile
bytes must not be written using an old version. Element
presentation version 2 and `core.ui_style` cooked version 1 are independent formats.
Document reconstruction must copy `PresentationProfiles()` through the builder,
as the canvas editor and source serializer now do.

Raw source/cook validation does not require a runtime instance. Runtime table
admission requires an actual admitted `UiReloadGeneration`, exact authored canvas,
and current targets. The table pins the allocator issuer and exact burned root
handle. It accepts normal revisions of that same tree; rebuilt or foreign trees
must undergo fresh admission. No public issuer accessor or parallel registry is
introduced.

## Owner and host ordering

The host supplies resolved logical content extent through `UiAnimationViewport`
and optional typed, revisioned presentation classifications. Changes to previously
explicit classifications or viewport facts require a newer evidence revision.
Unknown form factor and no capabilities are the explicit defaults; selection does
not discover platform services. Extent intervals are half-open and aspect ratios
use reduced integer comparisons.

The owner selects a complete attachment-local base style, then calls the existing
computed-style `ProjectLayout`. Explicit property-to-layout bindings therefore
remain final, including animation, sealing and accessibility precedence. Inspect
selected profile evidence separately from final evaluated constraints and geometry.

`PrimaryAttachment()` identifies the semantic owner's geometry. `Attach`,
`UpdateAttachment` and `Detach` admit at most seven additional geometry caches on
the owner thread, outside frame preparation. All attachments share the original
semantic tree, controls, bindings, focus graph and pointer capture owner. Extra
frames expose local clipped focus bounds, not another focus state machine.

Primary layout preparation issues the interaction revision. Additional engines
consume the actual prepared primary reservation and cannot independently choose a
revision. A failed later attachment abandons the whole candidate. Successfully
prepared but abandoned interaction revisions are burned, so gaps are expected.
All styles, layouts, clipping, controls and attachment geometry publish together.
Unchanged publications may reuse their existing interaction revision.

Each render view still needs an exact successfully presented receipt before
input is eligible. Committed geometry and copied inspection evidence do not grant
input authority. Detach closes the view immediately. Reload burns new attachment
identities, removes extra attachments and disables inspection admission; hosts
reattach and configure the new generation explicitly. Failed or cancelled reload
preserves existing admission. Retained old frames and inspection leases remain
readable after reload and shutdown.

## Optional immutable inspection

`ConfigureInspection` reserves independent copied storage for one attachment and
chooses whether capture is enabled. `Inspect()` returns the latest successful
copied lease. Standalone `UiLayoutInspector` provides typed capture/drop/rejection
outcomes, transactional rebind, and explicit shutdown. Its creating thread owns
all mutations and lease acquisition; retained immutable leases may outlive it.

Capture validates tree, layout, clipping and selection lineage and capacities
before writing a slot. Disabled capture, full retained slots, small capacities or
rejected sources leave the last observation intact and cannot fail ordinary
layout, input or publication. Snapshots copy hierarchy, final measure constraints,
measurement/arrangement, actual consumed dirty-work provenance, clipping, scroll
state and selected profile evidence. They hold no borrowed source pointers.

`UiLayoutRecord` retains its original first three aggregate fields and appends
constraints/work. Existing geometry consumers remain source compatible. Manually
constructed records must supply real evidence before presenting them as observed
layout. Work counters count actual calls in the bounded two-pass evaluator;
causes describe work consumed by that candidate, not dirty flags after publication.
Fresh candidates reset work; reused unchanged snapshots retain original evidence.
Failed or abandoned candidates never update retained evidence.

## Public topology and validation

`UiPresentationProfiles.h`, `UiLayoutInspector.h` and the value-only `UiLayoutWork.h` belong to `HoroRuntimeUi` in
the public header ownership registry. The existing Runtime UI public consumer
compiles both contracts. Regression sources exercise selector boundaries, issuer
and root identity, source/cook roundtrips and legacy payloads, retained inspection,
actual host attachment publication, computed width precedence, observer exhaustion,
reload and late attachment rollback. Execution evidence must be reported separately;
source coverage and formatting alone do not establish a passing test run.

No allocation or performance improvement is claimed without measured evidence.
