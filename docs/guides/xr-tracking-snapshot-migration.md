# XR Tracking Snapshot Migration

## Purpose

Migrate XR tracking producers and consumers from native device paths, mutable pose
maps, or last-known-pose caches to `Horo::XR::XRTrackingSnapshot`.

## Producer Workflow

1. Retain the exact active `XRSessionId`, `XRWorldOriginRevision`, and a monotonic
   `XRTrackingSnapshotRevision`. Device identifiers are session-owned live values;
   never serialize them or derive them from a native runtime path.
2. Normalize backend poses into validated simulation-purpose `XRPoseSample` values.
   Their runtime sample timestamp must equal the snapshot timestamp.
3. Publish device records in strict `XRDeviceId` order and poses in strict
   `(XRDeviceId, XRTrackedPoseKind)` order. Advertise only the closed Horo capability
   set appropriate for each semantic role.
4. Choose admitted limits within `XRTrackingSnapshotHardLimits`, then call
   `XRTrackingSnapshot::Create`. Overflow rejects the whole candidate; it is never
   partially published or silently truncated.
5. On tracking loss, remove invalid pose records. Permission/session loss publishes
   no device or pose evidence. Never substitute identity transforms or retain a prior
   valid pose as current evidence.

## Consumer Workflow

Call `ValidateXRTrackingSnapshot` before retaining a publication across a lifecycle
boundary. Use `QueryXRTrackedPose` with the exact device, snapshot revision, session,
and origin revision. The query is bounded and allocation-free; copy the returned
`XRPoseSample` only into state whose authority and lifetime are explicit.

Typed outcomes must remain distinct:

- `xr.operation.unsupported`: the selected device does not advertise that pose kind.
- `xr.operation.unavailable`: the known device or pose is temporarily absent.
- `xr.identity.stale`: the session or device generation was replaced.
- `xr.tracking_snapshot.stale`: the retained publication revision was replaced.
- `xr.origin_revision.stale`: the world-origin adapter changed.
- `xr.operation.capacity_exceeded`: the complete publication exceeds admitted bounds.
- `xr.tracking_snapshot.invalid`: evidence is malformed, contradictory, or non-canonical.

## Native source admission at the application cutoff

The private OpenXR producer now offers `OpenXRActionBindings::ObservePoses` before
there are any runtime-issued device IDs. It returns at most five actual head and
prepared grip/aim observations at the same synchronized simulation sample time.
It shares the existing pose collector and consumes the same opportunity as
`SamplePoses`; callers must not invoke both against one `Sync`. The legacy method
retains its existing-device filtering and returns the same typed device records.

`OpenXRApplicationSession::AppendInput` translates that fixed packet into the
non-installed `XRTrackingPublication::ObserveSources` contract. XRRuntime validates
the complete role/kind/session/origin/time packet before changing source records,
then uses its existing `ObserveDevice` issuer. The application never constructs a
device ID. It samples scalar actions with those IDs, publishes through the same
snapshot owner, and appends to the existing Input transaction; it does not commit
Input itself.

An inactive pose action does not prove physical disconnection. No source is created
from an interaction-profile name, a prepared binding, or an invalid head pose. Known
sources become `TrackingLost`, retaining independently invalid components and their
known capabilities. Resumed tracking alone preserves device generation. Actual
session/binding replacement and explicit disconnection evidence retain the existing
generation rules. Failure revokes the candidate snapshot, allowing the host's one
Input cutoff to neutralize XR without erasing keyboard/gamepad contributions.

All packets and source-update scratch are fixed arrays; no allocation, native
dispatch resolution, lock, second clock or snapshot issuer is added to a successful
sample. The new native-dispatch and XRRuntime-owner regression cases are source
coverage until executed on the current frozen worktree; they are not hardware
qualification.

## Validation Record

`HoroXRApiTests` covers immutable ownership, exact confidence, allocation-free query,
canonical ordering, hard/admitted bounds, contradictory evidence, loss neutralization,
unsupported/unavailable distinctions, replacement, origin change, and shutdown.
`HoroXRApiPublicHeaderConsumer` verifies the owned installed header boundary.

## References

- [XR Architecture](../architecture/runtime/vr-ar-architecture.md)
- [XR Coordinate and Pose Migration](xr-coordinate-pose-migration.md)
- [ADR-159: XR Action, Tracking and Input Projection Ownership](../adr/159-xr-action-tracking-and-input-projection-ownership.md)


### Private Play view-policy declaration migration

`EditorPlayViewSource` is now declared beside `EditorPlayState` in the existing
non-installed `EditorPlayState.h` host interface, rather than solely in
`EditorPlaySessionController.h`. Its existing integer values remain
`AuthoredCamera=0` and `SelectedXr=1`; no installed public header, serialization
schema or object layout changes. The controller selects this same value from the
actual attached XR host, the workspace model projects it to the Game panel, and
Play preparation still validates the matching real factory/participant before
Scene publication. A selected logical mirror image does not prove native frame
availability or change the renderer's physical `IsReady()` contract. Desktop
callers retain the authored-camera default. Consumers include the Play controller,
workspace controller/model, Game panel and staged OpenXR application-host consumer;
new CPU panel cases exercise the actual Draw path without a native acquisition.
