# Navigation project settings and profile management

HORO-1299 / #1299 / [NAV-007.9] implements the ADR-105/ADR-110 ownership split.
The registered `core.navmesh` definition uses the existing `.horoasset` source
extension. Registry path text is UTF-8 and is explicitly converted at native
filesystem boundaries; the host retains its canonical native root path.

`NavigationDefinition` remains the sole catalog for agent profiles, areas and
filters. `.horo/navigation.json` schema 2 contains only an explicit definition
AssetId binding, a default-provider identity and a validated runtime capacity /
query / required-capability profile. No catalog is copied into project settings.

## Editor workflow

Open Settings → Navigation, select a registered definition and choose **Open
Navigation Definition**. Routine catalog controls live in a persistent workspace
Asset document tab; closing Settings does not release that document's authority.
Create/update preserves stable identities. Profile display-name changes preserve
all references. Area/filter identity changes require an explicit successor and
use the same reviewed replacement operation as deletion. Package-contributed
area/filter provenance cannot be overwritten by project gestures.

**Review deletion** captures all known definition-local filter costs and typed
Scene surface/link/agent/filter/modifier references before enabling confirmation.
Rows include asset identity, object/filter identity, field, old identity and the
explicit replacement. A deletion without replacement is allowed only when no
references remain. An ambiguous agent catalog, unknown/unloaded registry owner,
stale file bytes, closed/reloaded document or changed registry generation rejects
admission. No confirmation silently recaptures a different set of references.

Prefab sources and Scenes containing prefab instances currently lack a canonical
transaction port in this workflow. Their exact asset identities are enumerated as
unsupported owners, references remain **unknown**, and destructive confirmation
is disabled. Non-destructive catalog creation/display-name editing does not
pretend to repair those owners. There is no generic prefab rewrite or partial
repair fallback.

Confirmation stages every immutable definition root, canonical Scene root and
history allocation before publication. All registry/document revisions and
cancellation are checked again before the first swap. Publication has no
callbacks, notifications or allocation. The next workspace frame projects the
new canonical state. Undo/redo replays the complete correlated group. Independent
Scene undo cannot split a coordinated entry. Weak owner-qualified handles reject
closed, reloaded, destroyed or independently advanced participants; they do not
retain a raw SceneDocument beyond its host lifetime.

**Save changed source documents** publishes each dirty source independently
through Assets' exact-byte CAS, native tracked atomic replacement and durability
receipt. A late edit remains dirty after an older save ticket. Save All is **not**
an atomic multi-file filesystem transaction: earlier durable files remain saved
if a later file fails. Uncertain durability is reported separately and does not
clear dirty state. The workspace leave guard requires an explicit save/discard;
Save As and reload cannot silently retarget an unresolved navigation group.

## Project policy, target compatibility and local preferences

Create an explicit policy draft, bind its definition, edit the provider identity,
finite budgets, query family/quality/ceilings and required capabilities, then save
policy separately. The authoring template is an editable example (64 agents,
8 surfaces, 128 tiles, 8 concurrent queries, 1 MiB/tile, 128 MiB total,
8192 work units/tick, 1024 nodes, 128 points, 1000 m). It satisfies the existing
profile envelope; it is not a measured performance claim or an active provider
selection. The user explicitly confirms the provider string. No provider is
constructed, discovered or silently substituted by this feature.

`CheckNavigationProjectCompatibility` admits the complete immutable runtime
profile against explicit host-supplied target/provider metadata. Missing target
metadata is displayed as Unknown and never promises native support. The selected
provider identity must match exactly; unavailable capabilities remain typed
failures. A runtime/cook composition may load the application store and pass its
validated RuntimeProfile to existing admission APIs; editor widget state is not
runtime authority and this ticket does not install a new backend bootstrap.

`NavigationProjectSettingsSession::Definitions()` returns a borrowed span over a
preallocated session projection, rather than an owned vector per draw. Entries
carry the exact document opening/revision and refresh in place after successful
commit or undo/redo. Readers copy an individual snapshot when they need an owned
immutable lease; spans must not be retained across mutation or Close. Closing
invalidates rendering access, and a replacement opening rebuilds its projection.
Rendering never calls filesystem freshness checks; explicit preview/commit/save
retain those checks. The Settings dropdown caches its asset labels per opening.
This unshipped API change updates both rendering callers and public consumer
coverage together. Allocation/read and refresh/close/reopen regressions cover it.

Area/path/agent overlay toggles are stored only in the user's existing
`editor_settings.json` rendering group. They never enter the project policy,
definition source, cook inputs or provider capability fingerprint. Applying user
preferences does not implicitly save a project policy draft.

## Persistence and migration

The policy store accepts only the authorized project's `.horo/navigation.json`.
Writes require exact previous bytes under the shared project writer and source
publication lock, a consecutive document revision and a valid successor runtime
profile. Before-commit failure preserves old bytes. After-commit durability
failure returns the actual new identity plus its error; it never claims rollback.
Schema 1's `provider` key explicitly migrates in memory to schema 2's
`defaultProvider`; the next explicit save writes schema 2. Unknown versions,
unknown/duplicate keys, malformed identities, negative/fractional/overflowing
integers, invalid enums and capacity inconsistencies reject without writes.
Rollback requires restoring a previously reviewed source/policy through the same
conflict-checked publication boundary, not changing an accepted schema number.

The AssetId-bound policy and codec belong to Application/ProjectSettings, which
declares NavigationApi and Assets dependencies. NavigationApi remains
foundation-only and owns just the Asset-free authoring error vocabulary. Frame dirty projection uses
the canonical Scene port's allocation-free IsDirty query rather than copying roots.

New public contracts belong to NavigationApi, ProjectSettings, EditorModel and
EditorServices in the public-header ownership registry. Existing settings/modal
and GuiScreen composition callers are updated together. GuiScreen's added host
method requires rebuilding editor hosts; it is outside the supported GameModule
SDK boundary. GameRuntimeContext, module registrations, exported descriptors and
GameModule vtables/layout remain unchanged at SDK 21. No SDK epoch is invented.

## Validation scope

`HoroNavigationProjectSettingsTests` covers exact reference review, two-owner
repair and grouped replay, every observed allocation-failure position, late owner
failure, weak lifetime/reload fences, real native policy CAS and durability
faults, schema migration/rejection, real source-session save/reopen and target
metadata admission. `HoroSceneDocumentTests` also exercises canonical root reload
against a retained review. A narrow public consumer compiles all new contracts.
These targets are registered in Windows, navigation and Sonar editor closures.
The editor and CLI must compile in addition to the headless contracts. Actual
commands/results belong to the delivery receipt; this document claims no run.

### Scene decode allocation boundary

Coordinated navigation repair retains canonical Scene encode/decode validation for
content fingerprints. Scene schema-1 decoding now rejects duplicate object keys
and parser depths of 64 or more, including ignored extension fields; existing
typed component validation and canonical encoded bytes are unchanged. This bounds
the stack used to clear a completed JSON tree without allocating during retirement.
The fault-injection regressions enumerate all observed allocation positions in
commit, grouped replay, and detached Scene decoding. This does not promise safety
under persistent memory exhaustion in the upstream JSON parser's partial-tree
cleanup; an allocation exception must reach the workflow boundary to become its
typed capacity failure.

### Presentation and Scene identity lifetime

Settings modals and definition tabs hold revocable, owner-thread session borrows,
not retained raw project owners. Each draw/open resolves the exact captured
opening without filesystem access or allocation. Close revokes its anchor before
retiring any document, projection or history; destruction and a later project
opening cannot revive old handles. Resolved pointers are synchronous,
nonreentrant borrows and cannot cross an event-loop boundary.

Save Copy As preserves the active Scene and navigation opening. Save As validates
its services and physically contained `.horo` destination before closing a clean
navigation group; rejected basic destinations preserve all owners. Replacing the
active identity, reloading or restoring recovery still requires an explicit
save/discard resolution of every dirty navigation owner.

### Windows allocation qualification

The three exhaustive commit, grouped replay and completed-Scene-DOM failure sweeps
use the existing `SceneAllocationQualification` admission and the existing
`ci-audio-release` Windows composition. The entire engine and test executable use
that same Release STL ABI. Windows checked-iterator Debug still exercises regular
functional, lifecycle and unchanged-projection cases; these three global failure
sweeps explicitly report unsupported there and are mandatory in Release.
MSVC 14.44's default `std::vector` constructor allocates an iterator proxy inside a
`noexcept` function. Injecting failure into that implementation-specific allocation
can terminate even before the operation's throwable allocation boundary. A single
target ABI override, exempted allocator size, sampled positions and longer timeout
are not substitutes for the consistent qualification composition.

No 4096-request assumption bounds the sweeps. Every request measured in the exact
composition is injected once, and the existing shared qualification runner checks
all three exact discovered names, zero disabled/failed/skipped Release cases, and
matching nonzero measured/completed-position markers in actual JUnit output.
The completed-DOM guard removes child-flattening scratch growth in these supported
compositions; it does not promise allocation-free checked-iterator proxy machinery
or persistent-OOM-safe cleanup of an upstream partially constructed parser tree.
The earlier Windows timeout has no captured stack; this compatibility mechanism is
source evidence, not a claim that its precise timeout position was observed.
