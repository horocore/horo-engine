# Play-in-Editor save and load

Open **Window → Tools → Save / Load Manager** during Play. The Manager lists the
current activation's sandbox slots and exposes Save New, Load Selected, Delete,
Refresh, and Cancel. Loading pauses the existing Play session while the normal
Scene lifecycle prepares and publishes the replacement world. Resume Play when
ready to continue simulation.

The Manager uses the same `RuntimeSaveSession`, canonical participants, bounded
archive reader/writer, `SaveSlotLifecycle`, and `SceneRestoreBundle` contracts as
runtime hosts. Save captures actual committed Play state at the owner safe point;
workers encode and publish detached state. Load validates the selected catalog
revision and slot generation before handing the original operation producer to
the Scene bundle. Queue admission or worker completion alone never means the
world has committed.

The authoring `SceneDocument` remains independent. Runtime capture and restore do
not receive a mutable authoring document or its persistence writer. Ordinary
**Save Scene** continues to save authoring edits separately.

## Sandbox lifetime

Each Play activation receives a fresh opaque environment, guest/profile, logical
world, and authored entity inventory under a dedicated Editor product root. The
root is resolved through the existing OS user-state save policy. Project source
files and production game namespaces are outside this capability.

Slots belong to that one activation. Stop, workspace leave, scene replacement,
and native module rebuild close request admission and cancel precommit work.
Already accepted publication can still finish; its original operation reports
that outcome. The owner retains the Scene, native registry, source leases, and
any handed-off restore until their real worker and Scene settlement. Restart
uses a fresh sandbox and does not offer recovery of a previous activation's
slots.

After settlement, cleanup deletes only this sandbox's slots through the normal
slot lifecycle. Cleanup failure or scheduler shutdown is reported as failed or
deferred cleanup; it does not mean the files were removed. Empty namespace
metadata may remain. Application teardown joins the actual workers before
retiring deferred Play owners and closing module providers.

## Participant coverage and diagnostics

A successful save must cover the complete admitted runtime state. Core Scene
capture preserves current transforms, hierarchy, supported typed components,
asset requirements, and explicit authored-entity tombstones. Actual installed
native Gameplay participants supply their own canonical state and exact module
leases. Behavior restoration additionally requires candidate-aware native state,
real inactive materialization, and engine-owned lifecycle records; ordinary
initialization callbacks must not overwrite restored state.

Required state with no supported canonical participant fails with a typed
diagnostic. This includes unregistered native subsystem state, undeclared Lua or
Gameplay persistence, and runtime-spawned provenance for which this host has no
save adapter. Authored physics or navigation descriptors are not substitutes for
live physical or agent state. The Manager displays the error identity and the
process operation history retains the actual operation outcome.

Only one save/load operation or storage action is admitted per activation.
Controls use immutable slot projections, and stale selection consent is rejected.
Closing the Manager leaves accepted work with its owner; use Cancel to request
cancellation. No normal frame joins a save worker or waits for storage.

This workflow does not qualify every whole-world participant required by
HORO-1447. That broader coverage remains separate from composing the real
supported runtime save/load path in the Editor.
