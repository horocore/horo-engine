# Extension inspector and property drawer migration

HORO-110 / #110 / [EXT-003.8] adds declarative, permissioned inspection of
project-owned typed behavior fields. It builds on the published ABI1.5 settings
and modal transport; it does not invent extension ownership of GameModule types.

## Authority and composition

A native module declares `editor.inspector` or `editor.property_drawer` in its
manifest and registers copied metadata through ABI1.6 `registerEditorInspector`.
The host composes `EditorInspectorHost` with the existing surface registry and a
sealed permission/capability policy. Registration is transactional with whole
module admission and retains the real retirement/code lease. The descriptor
itself remains inert. Providers receive no Scene document, serialization callback,
selection pointer, renderer handle or arbitrary drawing/edit callback.

A project behavior descriptor belongs to its existing GameModule/Lua registry.
Resolving that descriptor and having broad inspector permission do not establish
that a particular extension owns its data. The actual Inspector therefore starts
with generic controls and offers an explicit **Use this provider for this type**
action. The document owner validates the current selection, schema, provider
activation, immutable declaration and exact field set before storing a bounded
session approval. Only then can that presentation replace generic controls.

Approval is not serialized, issued by a module, or inherited from an ID prefix.
A new explicit approval replaces overlapping presentation approvals for that
exact behavior type/schema. Uncovered fields return to generic controls. Provider
retirement, document replacement and actual project behavior catalog publication
invalidate the relevant evidence. Epochs never wrap into reusable authority.

The editor application supplies the policy and the existing host theme capture.
`GuiScreenHost` registers those explicit services and the workspace borrows them
only for panel attachment. A missing artifact verification gate remains a typed,
fail-closed native activation boundary; this feature does not bypass package trust
or turn arbitrary installed DLLs into approved providers.

## Supported declaration profile

The current editable profile is bool, exact signed64 integer, finite double and
bounded UTF-8 string. Stack and Group containers, labels, text, localization,
read-only/disabled ancestry and typed constraints use the existing FormKit model.
Row, Grid, actions, choices, path controls and unsupported interactive profiles are
rejected during registration. This is not a claim to support every FormKit node.

Vectors, quaternions, unknown fields and opaque component/asset/project payloads
retain generic owner presentation and serialization. No fallback parses arbitrary
provider JSON or rewrites unknown bytes. A future owner codec needs its own
explicit typed admission contract; this feature does not manufacture one.

Every editable binding names one existing field of a resolved behavior schema.
Current values come from the actual document owner rather than provider defaults.
Mixed selection values remain visible. Integer editing uses bounded decimal text
and exact parsing, including limits and values beyond2^53; it never passes through
a double. Invalid values remain view drafts and cannot publish partial changes.

## Transactions, lifecycle and frame work

One accepted gesture resolves current provider capability and owner approval,
captures exact selected attachments and prepares complete replacements while
preserving all undeclared fields. The existing `SceneDocumentTransactions` owner
executes the entire batch once, with session/revision/state/content fences. The
same owner supplies undo and redo; no competing history or executor is added.

A stale selection, changed schema, locked object, invalid second replacement,
revoked provider or shutdown rejects the whole edit without changing document
bytes or history. Providers never run during shared control traversal.

The shared controls include the complete 64-bit selection revision in their
ImGui identity as bounded decimal text. Changing the selection epoch discards an
active draft before it can submit against the replacement selection; the high
revision bits are preserved without inspecting integer object representation.

Copied presentation/selection state refreshes only on actual document, selection,
provider, catalog or approval revisions. Theme/width changes rebuild the validated
shared render snapshot. The owner-lane cache is bounded to16 entries; declarations
have at most64 fields, selection at most256 objects, and approval storage at
most128 entries. Preparing a transaction is gesture work, not normal draw work.
These are design bounds, not a measured zero-allocation performance claim.

## Source and binary migration

Extension ABI1.6 appends one function pointer and one negotiated required-function
bit after the exact published1.5 prefix. Inspectors require minimum host minor6,
a complete1.6 prefix, the required bit and the actual capability composition.
Explicit1.5 negotiation still runs existing settings/modal modules with its exact
prefix; it cannot read or call the new tail. Explicit1.4 activity behavior and
page refusal remain covered separately. Rebuild new inspector modules against the
installed SDK containing `EditorInspectorAbi.h`; old providers need not opt into
inspector support or raise their minimum version.

The host's inspector registration callback has explicit C language linkage,
matching the activity/page callbacks in the native host table. Its opaque context
remains confined to this ABI boundary; registration continues to use the existing
typed module-load transaction and its retirement owner. No ABI field, version or
provider calling signature changes with this linkage correction.

Host source callers of `ExtensionManager` now pass `ExtensionEditorHosts` instead
of independent trailing activity/page pointers. The struct adds the inspector
host explicitly and avoids parallel overloads or ambient discovery. Production
GUI composition, native fixtures and contract consumers migrate together.

`PanelContext` adds borrowed inspector/theme views that must outlive attachment.
External host panel implementations must rebuild against the new header and must
not retain those views after detach. `SceneDocumentTransactions::CheckCurrent`
exposes the existing owner-thread/replacement check without copying authored data;
a wrong-thread caller cannot use it to rebind ownership. Its successful path
allocates nothing. Both are host source contracts, not GameModule exports.

GameModule SDK21 remains unchanged: these editor-host declarations and the
Extension ABI are not part of the supported GameModule export/host-services table.
The owning public-header registry and real consumer targets are updated in the
same change; native Extension ABI compatibility is verified independently.

## Required qualification

The owning regression closure includes real C11 native1.6 registration and
revocation, actual1.5 page loading/behavior,1.4 refusal, source-free installed SDK
consumers, exact integer/malformed-prefix C11 contracts, actual Scene batched
multi-edit/undo/redo/serialization rollback, real controller approval/refusal,
and actual shared-control Inspector rendering/fallback/lifecycle.

No validation is implied by this document. Exact source manifests, configured
targets, test discovery/counts, skips and resource fallback limits belong to the
PR validation receipt after execution.

The C11 `HoroExtensionHasEditorInspectorTransport` guard is shared by actual
native client admission and host negotiation. It checks the exact callback-end
prefix before inspecting the tail, then the ABI major/minor and callback presence.
It grants no registration permission or document approval. This also closes the
client-side wrong-major omission; old 1.5 prefixes remain refused before tail use.

Manifest contribution kinds retain `editor.property_drawer`; authority identities
use the existing canonical dot/hyphen grammar: permission `editor.property-drawer`
and capability `horo.editor.property-drawer`. Underscore authority aliases are
rejected. Passive Stack/Group nodes use the shared FormKit nonfocusable-container
contract; native copying preserves that semantic requirement without relaxing
the form validator.

### Workspace host composition

`EditorWorkspaceDependencies::gameplay` groups the borrowed build service, copied
build environment and shared module retirement queue. Application hosts pass
`gameplay.builds`, `gameplay.environment` and `gameplay.retirements` together;
workspace ownership and retirement ordering remain unchanged.
