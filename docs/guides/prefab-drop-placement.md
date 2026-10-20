# Prefab drop placement

Assets emits the existing process-local `HORO_ASSET_REFERENCE` payload for registered
`core.prefab` sources. Its workspace opening, Scene revision and replacement epoch
are captured once at drag start. Both hierarchy and viewport emit the existing
typed `AssetSceneDropRequest`; panels own no source resolver or document mutation.
This private process-local payload changed size; producers and consumers must be
rebuilt together. No public header, persisted Scene format or asset identity changes.

The workspace captures the complete prefab source closure through the host-composed
loader and canonical resolver. Ordinary Scene conversion validates a detached
candidate before geometry extraction. A retained preview pins immutable mesh leases
and source byte commitments, with no live runtime activation or history change.
Hierarchy hover resolves the same candidate and highlights its destination. Viewport
hover renders its composed primitive meshes as non-pickable ghosts. Prefabs without
renderable geometry show a unit placement marker; that marker is never authored.

Hierarchy center drops use the hovered ordinary Scene object as parent; row edges
use its ordinary parent, and background drops use the Scene root. They add an identity
local placement transform, preserving prefab-local transforms. Prefab-member targets
are rejected. Viewport drops are Scene-root placements using the shared surface,
XZ-plane and camera-front policy with the complete composed prefab bounds. Ghosts
are excluded from placement intersections so repeated hover and final delivery agree.

Final delivery rechecks the originating opening/revision, registry identity/type and
source path, rejects play/recovery/external-conflict states, acquires the project
writer lease and verifies captured prefab source/sidecar bytes, project metadata and
accepted persisted Scene bytes. The canonical `CreateScenePrefabInstanceCommand`
then commits exactly one semantic history record. Failure preserves authored Scene,
selection and history. Success selects the new instance; ordinary dirty, autosave,
save/recovery, undo/redo and conflict services remain authoritative. Undo removes
only the placement and redo restores its exact identity and transform.

The source closure is handed to ordinary hierarchy and runtime conversion after
commit so the placed prefab appears in the viewport through the regular runtime
pipeline. Cancel, another workspace command, asset publication or owner teardown
retires the disposable preview. The project lease protects cooperating writers;
there is no filesystem-atomic guarantee against a noncooperating external writer
changing bytes after the final digest check.

Regression scope: `HoroAssetSceneDropTests` and the existing workspace-prefab tests
inside the editor GUI test target. These cover bounded payloads, hierarchy parent
policy, nested geometry/bounds, preview self-hit exclusion, cancel, one-entry undo,
redo/save/reopen, missing/malformed sources, foreign path/session, stale opening or
revision, missing parent, and source/project/Scene external changes.
