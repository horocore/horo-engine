# Physics Inspector Controls

HORO-907 / #907 / [PHY-008.1]

The graphical Inspector edits the canonical authored `RigidBodyComponent`,
`ColliderComponent` and `PhysicsConstraintComponent` values. Sensors are collider
flags, not a competing component model. Fixed and distance are the only authored
constraint kinds in Scene schema 1; hinge and slider remain explicitly unsupported.
No native solver handle, filter index or cooked payload enters an editor draft.

The body card exposes motion, mass/density, initial velocities, damping and speed
limits. Collider cards expose analytic box/sphere/capsule/static-plane or an exact
shape asset/subresource, body reference, local quaternion/translation, scale,
collision profile and the complete material-slot map. Constraint cards expose
fixed/distance parameters and exact first-body and second-body/world frames.
Stable component/slot identities, schema and authored generation are read-only;
reference IDs use lossless nonzero uint64 decimal text, and assets/profiles require
canonical UUID text. Missing project identities are never fabricated or resolved
by the view. A syntactically valid reference does not prove project registration,
asset availability, filter compatibility or native support; the normal cook/runtime
owners still make those decisions.

## Preparation and publication

`PhysicsInspectorSession` owns transient typed values and bounded identity text.
It captures a new draft only when the actual object selection revision or document
session/revision/state changes, and releases it on detach or selection loss. Draw
performs no document snapshot, filesystem access, backend selection or asset lookup.
Text parsing occurs on input changes; the complete candidate is assembled only
for an explicit Apply gesture. Discard/Escape restores the captured baseline;
cancellation wins over Apply in the same frame.

New body creation stages its body and initial box collider together. Collider and
constraint additions are also staged. The author must supply actual project
profile/material identities and valid endpoint references before Apply. Removal
is staged too: deleting a body while a surviving collider/constraint references it
fails aggregate validation and publishes nothing. Several staged changes are one
undoable edit, rather than a sequence of partially valid producer publications.

The controller binds `SceneDocumentTransactions` to its existing `SceneDocument`
and `EditorHistory`. `EditorWorkspaceScreen` supplies the real host-issued
`documentSessionOwner`; no address-derived identity or fallback owner is accepted.
A document replacement retires the borrowed port and binds the new replacement
epoch. Exact current selection revision, opening identity, document revision and
state are checked before preparation; the canonical transaction owner rechecks
thread, cancellation, lock and revision admission before publication.

The additive `Horo/Editor/PhysicsInspector.h` contract belongs to `HoroEditorModel`.
`PreparePhysicsInspectorEdit` copies only the selected object after bounding and
fingerprinting its baseline, replaces only its three Physics fields and validates
all body/collider/constraint references against the complete snapshot. Existing
payload changes advance their generation without wrapping; new components start
at one. Generated objects are refused by this ordinary hand-authored edit path.
Enabled legacy TriggerVolume plus canonical Physics remains an explicit conflict.
Original Physics validation error information is retained across this boundary.

The resulting `SceneObjectReplacement` is submitted once to the existing
`Bind/Execute` owner. There is no Physics-specific history or serializer. Event
storage is prepared before the commit and the existing controller event, selection,
preview and undo/redo paths reconcile the resulting publication. Rejected edits
retain document, dirty state and history and expose an actionable diagnostic.

## Contract and migration

This is an additive host/editor C++ preparation API and localized GUI behavior.
Scene schema 1, authored Physics schema 1 and the supported GameModule SDK 21
exports remain unchanged. Existing callers need no wire migration or recook solely
for these controls. External EditorModel consumers may include the new header
through its owning target; public consumer coverage uses only that target's usage
requirements. No private source-root include is exported for this feature.

The graphical editor still composes explicit Null Physics. Editing or enabling an
authored component does not silently select a backend or activate a native play
world. Backend composition remains the separate lifecycle contract documented in
[Physics Architecture](../architecture/runtime/physics-architecture.md).

## Validation scope

Owning regressions cover complete graph rejection, exact generation and identity
handling, non-Physics/persistence preservation, one-entry undo/redo, stale and
foreign owner/selection fences, locked/invalid inputs, owner thread/shutdown,
transient staging/discard and actual shared-control render/model paths. Render
records alone are not a GPU smoke or manual interaction result. The delivery
receipt records actual executed targets, tests, skips and CI-only validation.

The preparation API clamps its object fingerprint reservation to
`MaximumSceneObjectFingerprintBytes` (16 MiB), independently of the larger
transaction-wide preparation budget. A caller's smaller byte ceiling remains
binding. This changes no document wire format or GameModule SDK layout; existing
callers keep their transaction settings and rebuild against the named public limit.
