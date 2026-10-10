# Local save storage qualification

HORO-1429 / #1429 / [SAV-003.13] qualifies the existing storage contracts under
[Save Game and Persistence](../architecture/runtime/save-game-and-persistence.md).
Lifecycle integration uses the published HORO-1430 delivery in PR #3370. This
document records the declared gates; it does not claim those gates have run.

## Declared compositions

| Composition | Executable evidence required |
|---|---|
| Linux native filesystem | Storage qualification and user-state qualification on the native POSIX adapter |
| Windows native filesystem | Same qualification targets in `HoroCiWindowsChecks`, plus existing root/lock/commit contracts |
| macOS native filesystem | Same qualification targets in the desktop CI build, plus existing root/lock/commit contracts |
| Headless server | Explicit server namespace, no profile/viewport requirement, isolated slot bytes |
| Deterministic test | Caller-driven bounded composition with injected failure and unchanged admitted source |

`HoroRuntimeSaveStorageQualificationTests` checks physical owner reopening,
all three explicit desktop root policies, spaces and Unicode in the approved root,
user/profile/environment/server separation, missing slots, rejected/bounded writes,
unrelated staging retention, POSIX permission denial, archive-backed corrupt versus
incompatible LKG planning, and bounded index reconstruction. Its deterministic
composition test uses the same independently framed archive as the native tests.
Testing a Windows or macOS root policy on Linux proves policy selection only;
native filesystem behavior requires a run on that OS.

`HoroSaveStorageUserStateQualificationTests` runs the real post-start preference
migration next to the product save tree. Migration, idempotent rerun, verified
preference backup restore, and a rejected transform preserve runtime slot bytes.
Installation-version directories remain empty. Restore retains its backup;
another migration attempt reports `BackupRequiresRepair` rather than overwriting
that evidence. This gate exercises user-state migration compatibility, not a
complete installer launch or product activation.

## Required lifecycle and fault evidence

The qualification target exercises every actual I/O stage occurrence of an
overwrite copy, including repeated catalog synchronization and cleanup
acknowledgements. Each injected failure checks reader-valid selections, restart
and two reconciliation attempts, with unrelated temporary and generation files
in the same directory. Rename and permanent-delete tests inject a catalog sync
failure after replacement and retain last-known-good bytes until reconciliation.
External export while journal cleanup is deferred verifies an independent
complete archive, including opaque chunks, rather than exporting internal files.

Run the published `HoroRuntimeSaveSlotLifecycleTests` alongside qualification.
That gate covers delete/copy/rename/import/export, traversal-like display labels,
wrong profile, malformed/untrusted archives, stale ownership, generation
collisions, signature admission, new destination ownership and capability-reported
recycle rejection. Display labels are metadata; commands accept no import path.

Fault-stage qualification must distinguish pre-publication failures from a
rename followed by failed durability synchronization. Failed preparation/write,
low-space/quota, flush and rename must retain the previous valid generation;
unknown publication must retain recovery evidence and reconcile under the lease.
The native stage observer injects a typed disk-full error around actual file
operations; this is deterministic fault coverage, not a physical ENOSPC/quota or
power-loss experiment. Retained immutable bytes in the deterministic memory
composition likewise do not establish hardware durability.

Existing `HoroRuntimeSaveRootResolverTests`,
`HoroRuntimeSaveFilesystemLockTests`,
`HoroRuntimeSaveSlotCommitTransactionTests`,
`HoroRuntimeSaveSlotIndexTests`, `HoroRuntimeSaveSlotRecoveryTests` and
`HoroRuntimeSaveTestCompositionsTests` retain their containment, case-alias,
interruption, locking, ordering and bounded recovery contracts. The final
qualification must run the applicable gates, not infer success from registration.

## Capability limits and reporting

- Windows immutable-reader publication requires Windows 10 version 1709+ and
  filesystem support for the existing rename contract. Unsupported publication
  fails closed. Native CI results must identify the runner/filesystem; a policy
  test is not evidence for arbitrary Windows volumes.
- POSIX permission denial is evaluated with ordinary user permissions. Privileged
  execution cannot establish that denial guarantee.
- Symlink/reparse restrictions and locking are native capabilities. Unsupported
  facilities must not be replaced by an unlocked or path-following fallback.
- Native OS tests do not establish crash durability on unqualified network or
  removable filesystems. Power-loss and hardware faults require separate evidence.
- Any skipped, unavailable or incomplete gate remains unproven. Record OS,
  composition, source SHA, command and outcome for every claimed result.

The local run must target the real PR parent after published-parent integration.
Final readiness additionally requires exact-head hosted CI, Sonar and Codacy,
actionable review remediation, and full ticket acceptance criteria. Human review
approval remains separate.
