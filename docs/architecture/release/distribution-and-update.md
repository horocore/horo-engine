# Distribution And Update Architecture

## Purpose

This document defines installation artifacts, update channels, signed update
metadata, download, staging, activation, rollback, compatibility, and user
control for HoroEditor and Horo Engine command-line tools.

Game distribution may reuse the packaging and verification primitives, but each
game owns its product-specific update policy.

## Core Decisions

- Installed artifacts are immutable for one product version.
- Updates are described by signed manifests and content hashes.
- Download and staging never modify the running installation.
- Activation is performed by a small trusted updater after the main process
  exits.
- Failed activation rolls back to the last verified installation.
- Update checks respect explicit channels and user policy.
- Project formats are not upgraded merely because an update was downloaded.
- Plugins and toolchains have independent compatibility checks.
- Renderer backends are independently installable signed product components;
  editor-core installation does not imply installation of every renderer.

## Distribution Products

Supported products include:

- HoroEditor desktop package
- `horo-engine` CLI package
- `horopak` package tool
- public SDK/development package
- first-party renderer component packages
- symbols and diagnostic artifacts kept outside ordinary installation

Each artifact has stable product, version, platform, architecture, build
identity, and checksum metadata.

Renderer component install, availability, probe, and no-renderer recovery follow
[Renderer Distribution And Availability](../runtime/renderer-distribution-and-availability.md).
Their signed metadata and private module ABI follow
[Renderer Module Package Manifest](../runtime/renderer-module-package-manifest.md).

## Platform Package Formats

Distribution profiles select one or more platform package formats:

| Platform | Editor / CLI formats | Game formats |
|---|---|---|
| Windows | `.msi`, `.exe`, `.zip` | installer, portable zip, store package |
| macOS | `.dmg`, `.pkg`, `.zip` | `.app` bundle, `.dmg`, store package |
| Linux | `.AppImage`, `.tar.gz`, `.deb`, `.rpm` | portable archive, AppImage, distro package |

A package format defines:

- install layout
- executable identity
- signing requirement
- update eligibility
- uninstall behavior
- file association behavior
- desktop/menu integration
- rollback support
- user-data and cache locations

Package format is not inferred from operating system alone. It is selected by
the release or distribution profile.

The shared distribution domain represents that decision as an explicit typed
selection containing product, full release version, platform, architecture,
build, package, intended installation, and artifact-class identities. Engine
products and game products retain their distinct release-version types.
Renderer components carry their own component identity instead of impersonating
the editor or SDK.

Format descriptors expose install layout, file association, desktop
integration, uninstall, update, signing, and rollback capabilities without
native installer SDK types. Admission rejects unsupported product, platform,
artifact-class, and format combinations before any download or installation
work. Symbols and diagnostic artifacts have no installation identity and use a
separate supplemental artifact class rather than entering the ordinary product
installation path.

The Linux tar.gz preflight reader authenticates the complete signed package,
checks canonical gzip and ustar framing, rejects links and unsafe paths, and
returns a bounded entry index without writing files. `StageVerifiedTarGzipUpdate`
then compares that index with the package's canonical internal file inventory,
checks available capacity, extracts into an absent private sibling directory,
and rechecks every file digest before publishing the durable ready marker.
Failure removes only the newly created stage; a preexisting stage or marker is
left untouched.
The reader accepts one gzip member with no optional gzip header fields and
ordinary POSIX ustar file/directory entries; other tar extensions fail closed.

## Game Content, Patch, And DLC Releases

Game releases may contain multiple content units:

- base game package
- required startup chunks
- optional chunks
- DLC chunks
- language packs
- platform-specific shader or texture packages
- dedicated-server content subsets

The release manifest records for each content unit:

- chunk ID
- semantic content version
- asset archive identity
- required engine/runtime version
- dependencies on other chunks
- install size and download size
- integrity hash and signature
- encryption/protection policy
- mount priority and runtime availability policy

Patch releases compare a previous verified release manifest with the candidate
manifest and may produce delta packages. Delta packages are optimization only:
the updater must be able to fall back to the full package when delta validation
or application fails.

Signed update manifest schema v2 adds delta ZIP records separately from the
standalone full packages. Each delta record identifies its allowed full package
and binds the exact base, patch, and full target file inventories by digest.
Schema v1 remains the canonical encoding when no deltas are present. The
`DeltaZipArchive` format is admitted on supported update hosts only as a delta
record; full-package discovery and installation activation reject it as a
standalone product. `SelectUpdatePackageCandidates` authenticates the manifest
and returns the allowed full package together with an optional delta matching
the verified installed base inventory. An unknown base yields the full package.
`PlanUpdatePackageAttempt` bounds delivery to an applicable delta attempt followed
by its signed full-package fallback; a failed full package ends the sequence.

`PlanUpdateFileDelta` compares the complete base, candidate, and delta file
inventories against their authenticated canonical digests. The patch inventory
contains exactly the changed and new files plus the required executable
entrypoint when its bytes are unchanged; deleted files are absent from the
candidate inventory. Executable mode and entrypoint role are part of file
identity. `StageVerifiedDeltaZipUpdate` verifies the signed
download and extracts the patch into a private tree without publishing a ready
marker. `ReconstructUpdateFileDeltaStage` revalidates the
plan, verifies both quiescent source trees, copies into a new private directory,
and verifies its exact full target inventory. Failure or cancellation removes
only that newly created directory. It does not publish a ready marker or grant
activation authority. For a ZIP target produced by the deterministic full-ZIP
producer, `RepackVerifiedDeltaAsFullZip` recreates the complete package from
that verified tree. It compares the exact resulting size and digest with the
allowed signed full package, verifies its publisher signature, and only then
publishes the existing complete-package ready marker. Existing activation,
rollback, and retention use this normal full-package evidence. A byte mismatch
cannot be re-signed locally; `PrepareSelectedZipUpdateStageHttps` discards its
owned reconstruction and downloads the signed allowed full ZIP instead. The
same worker falls back after an inapplicable or failed delta, but cancellation
and uncertain cleanup stop the attempt. Its target inventory must match the
digest in authenticated delta metadata. Other full-package formats retain their
ordinary complete-package delivery path. No partial delta ZIP may be activated.
Schema-v1 readers must not reinterpret v2 delta metadata as v1.

`ResolveAssetChunkMountOrder` checks an exact optional/DLC selection against a
verified base-manifest digest, dependency closure, and dependency-first mount
priority. `PlanAssetChunkRemoval` rejects removal that would strand an installed
dependent. Both are admission plans; the package lifecycle owns verified
archive leases, mount/unmount, and transactional removal under its update
contract. `AssetArchiveProvider::OpenSelected` checks that every encoded chunk
definition matches the authenticated release plan and exposes assets from only
the selected, dependency-closed chunks. It rejects wrong base identity, missing
dependencies, modified archive bytes, or plan drift before exposing any asset.
The host verifies package signatures first and owns provider replacement and
unmount; none of these admission operations grants package trust or mutates
project files.

A DLC or optional content package must never replace base-game files implicitly.
It is mounted through the runtime asset-provider contract and validated against
the same manifest, signature, compatibility, and chunk dependency rules as the
base package.

## Update Channels

Canonical channels:

- `stable`
- `preview`
- `nightly`
- enterprise or offline channel defined by deployment policy

Moving to a less stable channel requires explicit user action. Returning to an
older version is treated as a deliberate rollback with project compatibility
warnings.

`UpdateDiscoveryPolicy` records the selected channel, check interval, automatic
check and download preferences, mandatory security check policy, and telemetry
consent. The host supplies its installed channel and last successful check to
`PlanUpdateCheck`; a channel change is scheduled only after an explicit action.
Startup and periodic checks are scheduling decisions: the host must dispatch
source access asynchronously and must not wait for it before normal startup.
`AssessUpdate` authenticates fetched metadata against installed trust roots and
selects only a host-supported package for the installed product and target.
Source failures and incompatible packages remain diagnostic results and do not
modify the installation. The product host owns source adapters, persistence,
security-update classification, and download decisions.

## Update Manifest

```json
{
  "manifest": {
    "schemaVersion": 1,
    "product": {"kind": "editor", "componentId": ""},
    "channel": "stable",
    "version": "0.8.2",
    "buildId": "build_123",
    "sequence": 12,
    "publishedAt": 1781438400,
    "expiresAt": 1782043200,
    "minimumUpdaterVersion": 1,
    "minimumRootRevision": 2,
    "releaseNotes": "Editor stability and accessibility improvements.",
    "compatibilityImpacts": ["Plugins built for API revision 1 must be updated."],
    "packages": [
      {
        "platform": "macos",
        "architecture": "arm64",
        "format": "mac-dmg",
        "packageId": "editor-macos-arm64",
        "installationId": "horo-editor",
        "url": "https://example.invalid/editor.dmg",
        "size": 12345678,
        "sha256": "<64 lowercase hex digits>",
        "signature": {"algorithm": "ecdsa-p256-sha256", "publisherId": "com.horo", "keyId": "update-2", "signature": "<128 lowercase hex digits>"}
      }
    ]
  },
  "signature": {"algorithm": "ecdsa-p256-sha256", "publisherId": "com.horo", "keyId": "update-2", "signature": "<128 lowercase hex digits>"}
}
```

The signed document uses canonical JSON with exact schema fields; the signature
covers the canonical `manifest` object. Time values are Unix seconds in UTC.
The installed updater persists the highest accepted sequence and root revision;
transport responses cannot lower either value. The manifest and package identity
are verified according to [Release Security](./release-security.md). Transport
security does not replace artifact signature and hash verification.
`releaseNotes` and `compatibilityImpacts` are optional plain text inside the
signed canonical payload. Their absence preserves existing schema-v1 bytes and
parsing; present notes are capped at 32 KiB, with at most 16 non-empty impact
summaries of 1 KiB each. Editor presentation wraps these strings as text, never
interprets them as commands or markup. Older installed clients may reject a
manifest that adds these optional fields and must retain their current version.

The editor update session is owned by the graphical host, independent of the
settings view. An installed-product host may inject an update backend only after
it has authenticated the installation identity, trust-root snapshot, source
selection, private staging paths, and updater-helper handoff. The manifest
cannot supply those authorities. The backend binds download to the selected
verified package, and staging is format-specific: the ZIP stager accepts only
ZIP packages and the Linux tar.gz stager accepts only tar.gz packages. The
installed-product host selects one for its admitted format; an absent stager
leaves update actions unavailable. A restart or rollback action requests
a helper handoff and remains pending until the host reports a verified outcome
on a later launch. The installed host may inject only a verified `Active` or
`RolledBack` helper result with the update backend when starting the editor;
the editor maps that result to the visible session state. A failed or absent
helper result never becomes a success state. Hosts that do not compose updates
keep using the default app entry point without an update context. The running
editor never switches its own executable files.

The native installed-manifest transport explicitly selects TLS 1.3 as both its
minimum and maximum protocol version before any request configuration. TLS 1.2
and earlier are rejected; peer and hostname verification remain mandatory. An
additional supported protocol version requires an explicit transport-policy
revision and corresponding negotiation tests.

For network discovery, the installed host supplies exact HTTPS manifest
endpoints keyed by editor channel, including the expected signed channel. The
private manifest source rejects an absent or ambiguous mapping, userinfo,
fragments, and query credentials. Its TLS-verified fetch does not follow
redirects and caps the response at the canonical manifest limit. A fetched
document cannot choose its own endpoint or expected channel; signature and
installed-product admission are still checked by `AssessUpdate`. The host must
provide the authenticated endpoint map and trust-root snapshot before it can
inject this source into the update backend.

## Update Trust Root And Metadata Freshness

The updater verifies update metadata against a configured trust root. The trust
root is installed with the product or provided by an enterprise/offline policy.

Update metadata includes:

- manifest schema version
- product identity
- channel identity
- published timestamp
- metadata expiry timestamp
- minimum supported updater version
- minimum allowed product version when anti-rollback is enabled
- signing key identity

Expired metadata is rejected unless an offline policy explicitly permits it.
Mounted local, removable-media, and administrator-synchronized enterprise
sources use a configured absolute root containing `manifest.json` and
`packages/<64 lowercase digest hex digits>.zip`. The digest-named package path
is derived from the authenticated package record; neither manifest URL nor
media content may choose an arbitrary local path. Source descriptors have
unique IDs and explicit precedence. A host uses `MayTryNextOfflineSource`
to try the next source only when media or metadata is unavailable; a present
but invalid signature, stale policy, unsafe path, or package mismatch stops
fallback. Source and private stage parents remain quiescent for an import
operation.

`ImportOfflineZipUpdate` parses the same canonical signed manifest and calls
the same discovery, package verification, ZIP extraction, and ready-marker
paths as an HTTPS update. An administrator may configure at most 30 days of
manifest expiry grace for a particular source. The installed trust root must
still be unexpired, signatures and package digests remain mandatory, and the
monotonic sequence floor is unchanged. Downgrade requires both an explicit
host action and source administrator policy. The default freshness parameter
on `VerifyUpdateManifest` and `AssessUpdate` is strict, preserving existing
online callers; hosts migrating to offline source descriptors pass the bounded
policy only for that selected source.

Returning to an older version is allowed only through explicit rollback policy
and user or administrator action. Automatic update checks must not downgrade a
product because an attacker served an older valid manifest.

Signing key rotation and revocation are handled through a versioned trust-root
update policy. A compromised update key invalidates affected manifests and
requires a fail-closed updater response unless an administrator recovery policy
is active.

An installer-authenticated revision-one root establishes the initial key set.
Each subsequent canonical root document carries a complete replacement key set,
its parent revision, a strictly increasing revision, a nondecreasing manifest
sequence floor, and expiry. The currently installed root verifies its detached
signature before the updater may durably replace the root snapshot. Omitting an
old key from the replacement set revokes it. A network response cannot bootstrap
a root or skip revisions; administrator recovery needs a separate explicit path.

## Update State Machine

```text
Idle
  -> Checking
  -> Available
  -> Downloading
  -> Verifying
  -> Staged
  -> AwaitingRestart
  -> Activating
  -> Active

Downloading / Verifying / Activating -> Failed
Activating -> RolledBack
```

State is persisted so interrupted downloads and activation can recover safely.

## Download

Downloads use:

- bounded disk-space preflight
- temporary staging directories
- resumable ranges only when server identity and content validators match
- strict redirect and destination policy
- progress through the common job model
- cancellation without damaging an installed version

Downloaded bytes are untrusted until all expected hashes and signatures pass.
`PlanUpdateTransfer` admits a fresh body only for the exact signed package URL,
declared byte count, and a complete response. It permits a range resume only
when a durable partial-file checkpoint matches the package hash and size,
original and effective URL, strong ETag, exact start offset, and total length.
A changed validator, unexpected redirect, or server response that ignores the
range fails closed; the caller must explicitly discard the old partial file
before a new transfer. `CheckUpdateTransferSpace` reserves host-requested free
capacity, and checkpoints advance only after bytes are durably stored. The host
selects the transport and owns private storage, cancellation, and progress
dispatch.
`VerifyCompletedUpdateTransfer` then checks the complete private-file size,
hash, and publisher signature before any extractor or staging marker consumes
it. The file-backed path streams the private package through SHA-256 in bounded
memory; the host keeps that file quiescent through extraction so the verified
bytes cannot change between the check and use.
After package authentication, a format-specific reader must expose the complete
archive index to `ValidateUpdateArchiveIndex` before extracting any entry. The
preflight uses the release artifact path grammar and rejects links, special
files, nonportable or colliding paths, file parents, and excess entry or
expanded-byte counts under host policy limits.
The reader and staging host remain responsible for proving that extracted bytes
match the archive and its declared per-file inventory. `VerifyUpdateStagedTree`
streams each private staged file through SHA-256, checks its declared length,
and rejects missing, undeclared, linked, or special entries before a ready marker
may be published. The host must keep the private stage quiescent during this
check; the archive reader must supply the complete authenticated file inventory.
For ZIP packages, `StageVerifiedZipUpdate` first authenticates the complete
private package, preflights every central-directory entry and local header,
then requires exactly one `horo-update-files-v2.txt` entry. Its canonical UTF-8
payload starts with `horo-update-files-v2\n` and contains sorted rows of
`<path>\t<decimal byte count>\t<sha256:64 lowercase hex digits>\t<0644|0755>\t<content|entrypoint>\n`
for every other regular file. Exactly one nonempty entrypoint is required and it
must be executable. The inventory entry is not installed. The reader rejects missing, extra,
duplicate, mismatched, or noncanonical rows before extraction, then compares
every decompressed file with its declared digest and size, applies the signed
permission mode to the staged file, and verifies that mode before publication. It writes into an
absent sibling directory durably only after checking that the authenticated
expanded size fits the available capacity with the host's free-space reserve.
It checks the completed tree again and removes the new tree on failure before
any ready marker can survive. ZIP producers must
write this inventory before package signing; they can use
`BuildCanonicalUpdateFileInventory` to produce the bounded, sorted bytes.
`UpdateZipPackageProducer` is the ZIP backend for `ProduceReleasePackage`. It
streams frozen source files into a private package, verifies the bytes actually
read against the pre-sign inventory, writes the canonical internal inventory,
and records the final package digest. Hosts install it explicitly for ZIP
selections and provide the same archive limits used by staging. Existing
package backends do not gain ZIP behavior implicitly.
The v2 inventory and v2 ready marker replace the v1 contract. Producers must
declare the product entrypoint and executable file paths from the frozen source
inventory before signing. Existing v1 ZIP and tar.gz packages must be rebuilt
and re-signed by a v2 producer; the staging reader does not infer an entrypoint
or silently accept v1 metadata. Callers constructing `ReleasePackageRequest`
must supply both new fields. The bootstrap host must launch only the authenticated
entrypoint and must not derive a default path or change mode at launch time.
`ProbeVerifiedUpdateEntrypoint` provides the shell-free process boundary for a
trusted host-selected health command. It reauthenticates the package, v2 ready
marker, and complete staged tree before passing the signed entrypoint path to
`IExternalProcessRunner` with a bounded lifetime and output budget. The host
keeps the stage quiescent and owns product-stop and launch-admission coordination;
the probe alone does not establish that gate or install platform integration.
The native filesystem exposes OS-held shared product-launch leases and an
exclusive maintenance gate on the installation's `.product-launch.lock` file.
On POSIX, a lease closes its descriptor without an explicit `LOCK_UN`: descriptors
inherited through `fork` or duplicated for a bounded probe share one lock, and
closing one reference must not release the remaining process's gate.
The platform process runner accepts a borrowed exclusive maintenance lease for
one bounded probe. It transfers only a duplicate of that OS capability, using
an explicit Windows handle allowlist or one POSIX descriptor action. The child
may adopt it only after matching the native file identity to the installation
lock; the environment carries the descriptor number, never authority by itself.
The ordinary product launch path must still obtain a shared lease.
An installed product launcher retains a shared lease for its entire process
lifetime. Bootstrap, repair, uninstall, and update activation retain the
exclusive gate after requesting product shutdown and before mutating the active
state. If any launch remains active or races with maintenance, the operation
fails before publication. This gate is additive to the transaction lock and
does not terminate processes; launcher integration and a cooperative stop
channel are required before production composition is complete.
`PrepareZipUpdateStageHttps` is the blocking host worker operation for ZIP
updates: it resumes or downloads into the protected private package file, then
authenticates and extracts that same file before returning a durable ready
marker. A complete checkpoint reuses its verified bytes without network work.
`PrepareTarGzipUpdateStageHttps` applies the same protected download and
checkpoint boundary to a Linux tar.gz package before its format-specific
inventory verification and extraction. The two operations share the private
path validation; existing ZIP callers need no migration.
The host owns background dispatch, private-path allocation, and quiescence.
Other package formats require
readers with the same preflight and durable publication sequence.
Partial-file checkpoint evidence uses a bounded canonical schema. Recovery
parses it as untrusted bytes and rechecks it against the selected signed package
and the new transport response before appending any downloaded bytes. The
private checkpoint store publishes canonical evidence with a durable prepared
file and atomic replacement, then recovery requires both the partial file and
checkpoint to exist as regular files with exactly matching durable byte counts.
Native private-file writes require an absent file at offset zero or a regular,
single-link file at the exact checkpoint offset. Each append is flushed before
its checkpoint advances; an interrupted write without matching checkpoint
evidence is rejected during recovery.
The host-composed `UpdateDownloadSession` admits final HTTP headers before any
body byte, loads the previous checkpoint, checks remaining private storage,
and publishes progress as durable byte counts. Cancellation, excess body data,
and short responses cannot finish or mark a package ready. A complete response
is authenticated against the signed package before it can be used for staging.
`DownloadUpdatePackageHttps` binds a concrete TLS-verified cURL GET to this
session. It disables redirects, admits only the signed effective URL, parses
bounded final headers before the first body callback, and rejects ambiguous
framing or content encoding. A resumed request sends the durable byte range and
its strong ETag with `If-Range`; a fresh full response cannot silently replace
partial bytes. The host calls this blocking adapter on its update job and owns
progress dispatch to the user interface.
Once the authenticated archive reader has extracted and supplied its complete
per-file inventory, `PublishVerifiedUpdateStage` rechecks the complete private
package and staged tree, clears any stale marker, and atomically publishes a
durable marker bound to package and inventory digests. The host keeps both
private inputs quiescent through this publication.

## Staging

Staging:

1. creates a version-specific directory
2. extracts with path and resource limits
3. verifies every declared file
4. validates executable identity and package structure
5. writes a staged-install manifest
6. marks the version ready atomically

The running process never loads libraries or plugins from an incomplete staged
version.

## Activation

Activation occurs after editor and CLI processes using the installation exit.
`VerifyReadyUpdateStage` must reauthenticate the exact durable ready marker,
signed private package, and staged file tree while the installation is locked
and quiescent, immediately before any active-version transition. A marker alone
is not activation authority.
The updater:

1. acquires the installation lock
2. verifies staged and current manifests
3. records rollback information
4. atomically switches the active installation pointer or directory
5. starts a bounded health check
6. commits success or restores the previous version

Platform installers may implement the switch differently, but partial mixed
versions are forbidden.
`ActivateVerifiedUpdate` is the portable helper transaction for a versioned
installation root. Its package IDs select immutable sibling version trees; a
bounded active-pointer file names exactly one package ID and signed digest.
The helper takes an exclusive installation lock, asks the host to prevent new
product launches and wait for existing processes, rechecks both ready versions,
then writes a durable pending journal before atomically replacing the pointer.
A failed bounded startup probe restores the previous pointer. After a crash,
the same version evidence lets the helper resolve the journal to the verified
previous pointer before another activation attempt. A different or malformed
journal fails closed for explicit operator recovery.

## First Installation

`BootstrapVerifiedInstallation` admits an absent installation pointer only.
It uses the same authenticated ready-stage evidence, immutable package-ID
directory, installation lock, and active-pointer record as the normal updater.
The host checks the actual operating system, architecture, minimum OS,
destination permissions, capacity, conflicts, and package policy before the
transaction starts. The shared transaction writes a durable first-install
journal before registering operating-system integration or activating the
candidate. A bounded first-launch probe must succeed before the journal is
removed. If registration, activation, or the probe fails, the candidate
pointer and its integration are undone; an uncertain cleanup retains the
journal and blocks a new install until recovery proves the state. Recovery
never treats a marker alone as installation authority.
`CheckBootstrapBuildTarget` rejects packages targeting a different OS or CPU
than the running installer binary. Native integration hosts additionally probe
the actual OS and hardware (including emulation), OS version, capacity, and
permissions before making any changes.

The current authenticated stage readers support portable ZIP and Linux tar.gz
packages. Native Windows, macOS, and Linux installer formats require format-specific stage
readers and integration hosts with the same verification and rollback
guarantees. `RepairVerifiedInstallation` reauthenticates the active package and
tree under the installation lock before restoring idempotent integration and
running a bounded startup probe. `UninstallVerifiedInstallation` first verifies
the active package and exact inventory, writes a durable removal journal, and
deactivates the product before asking the platform host to unregister its
integration and remove that version's owned files. A partial removal stays
inactive and the same package/inventory can resume it. The host's removal
contract preserves projects, settings, caches, logs, credentials, and shared
components; a separate explicit data-removal policy would be required to
remove any of them. `LinuxPortableBootstrapHost` admits only validated tar.gz
portable selections at exact immutable version paths. It probes the running
kernel and hardware architecture, write access, and free transaction space.
Registration is deliberately empty because tar.gz format policy supports no
desktop or file-association integration. An executable-host process coordinator
supplies the product-stop and bounded health operations. During uninstall the
adapter reauthenticates any remaining package and ready marker, rejects altered
or undeclared stage entries, and unlinks only signed inventory files and their
empty parent directories. `ZipPortableBootstrapHost` applies the same owned-file
removal to native Windows and macOS ZIP installations after checking the running
OS, architecture, configured minimum OS version, writable destination, and free
transaction space. ZIP policy deliberately leaves registration empty. The
portable removal implementation moved to a target-private helper; there is no
public migration for existing Linux host callers. System-managed package readers,
OS integration hosts, and production process coordination remain outstanding.
The public bootstrap-host
contract now requires an idempotent `RemoveOwnedVersion` operation; existing
host implementers must add that operation before adopting this interface. No
production implementation existed when this contract was added.

## Rollback

At least one last-known-good version is retained within a disk budget. Rollback
is offered after:

- activation failure
- startup health-check failure
- explicit user selection

`RollbackVerifiedUpdate` admits only a prior version of the same product and
installation. Explicit user downgrades require acknowledgement, and normal
policy rejects versions below the trusted floor. Administrator recovery is a
separate host-authorized path. The rollback helper reuses authenticated staged
version evidence, the installation lock, the durable activation journal, and a
bounded startup probe; a failed probe leaves the newer verified version active.
The host must establish authorization and the version floor from trusted state,
not from package-provided or UI-provided values.

After a healthy activation, the helper atomically publishes the verified
previous version as `last-known-good-version` before clearing its transaction
journal. The journal also preserves the prior pin or its absence. A failed
probe or interrupted activation restores both the previous active pointer and
the prior pin; uncertain restoration retains the journal for recovery. Cleanup
must verify that the pin is distinct from the active version and reauthenticate
its package before treating it as rollback-safe content.

`PlanUpdateRetention` uses a complete host-owned installed-version snapshot and
measured occupied bytes. It protects exactly one active and one distinct
last-known-good version. When those pins alone exceed the configured budget,
the plan fails instead of deleting either. Otherwise it selects obsolete
versions by oldest use generation, with package ID as a stable tie-breaker.
The deletion host must recheck the active and rollback records under the same
installation lock immediately before removing only the planned owned files.
`ApplyUpdateRetention` performs that locked recheck, stops installation users,
and verifies protected versions and the selected obsolete version against its
signed package inventory. It writes a durable per-version cleanup marker before
removing any files. A repeated call may resume a marked partial deletion using
the authenticated package and remaining-file inventory, and removes the package
last. Unknown files or links stop cleanup without deleting those entries.

User projects, settings, caches, and credentials are not stored inside the
versioned installation and are not deleted by rollback.

## User State, Cache, And Settings Migration

Product updates may require migration of user-level state:

- editor preferences
- recent-project records
- local toolchain profiles
- workspace cache
- asset and shader caches
- plugin resolution cache
- update state records

Runtime save archives are governed separately by the release manifest's
`saveCompatibility` contract and
[ADR-112](../../adr/112-save-archive-container-and-compatibility-policy.md). Update
activation does not rewrite save slots. Save migration verifies the source archive,
uses detached staging, preserves the original unless captured product/user policy
authorizes replacement, and publishes a new slot generation only through the runtime
save transaction.

User projects are not migrated during product update activation. User-state
migration runs only after the new product version starts and validates the
state schema.

Migrations are:

- versioned
- deterministic
- logged
- recoverable where practical
- separated from installation activation
- never allowed to block rollback of the executable installation

Caches may be discarded and rebuilt instead of migrated. Credentials are never
migrated by copying raw secret values; only credential references may be
validated or re-authorized.

`RunUserStateMigration` is a separate application operation invoked after the
new product process starts and before user-state writers begin. The host supplies
explicit user-state and cache roots plus one-step, content-addressed schema
edges for preferences, recent-project records, toolchain profiles, workspace
state, update records, and disposable cache files. It orders the plan
deterministically and refuses lexical project escapes, links, duplicate
destinations, stale source bytes, and unauthorized credential references.
State transforms retain a durable adjacent source backup before atomically
publishing replacement bytes. A failed or interrupted transform leaves either
the original file or that backup for `RestoreUserStateMigrationBackup`; an
unresolved backup blocks overwriting it. Disposable cache entries are removed
only from the dedicated cache root and can be rebuilt. This operation does not
read or mutate project documents, and installation activation never calls it.
`HoroEditor` composes this application operation at startup before loading
editor settings or recent projects. Its host adapter supplies the concrete
legacy-to-version-1 steps for those two files, while the application operation
owns backups, atomic replacement, and recovery. Other state families require
their own explicit host adapters before they can enter a migration plan.

## Compatibility

Before activation, the editor reports:

- project format support
- plugin compatibility
- installed SDK/toolchain compatibility
- operating system minimum version
- renderer/backend capability changes

An engine update does not rewrite projects in the background. Project format
changes follow explicit project-open validation and user-visible operations.

## User Experience

Update checks run at a low frequency and never block editor startup. The UI
supports:

- check now
- channel selection
- download automatically policy
- install on exit or restart now
- release notes
- download and verification diagnostics

Mandatory security updates require an explicit product policy and still retain
diagnostic and recovery paths.

## Offline Distribution

Offline packages use the same signed manifest and verification. Administrators
may provide a local update source or install packages manually without disabling
integrity checks.

## Observability And Privacy

Update records include product, channel, version, phase, duration, byte counts,
and safe error codes. They do not include credentials, full proxy configuration,
or unrelated project data.

Update checking telemetry is opt-in or governed by product privacy policy.

## Testing

Required tests cover:

- manifest signature and hash rejection
- platform/architecture package selection
- platform package format selection
- interrupted and resumed download
- staging path traversal and disk limits
- activation lock contention
- startup health-check rollback
- preservation of user data
- plugin and SDK compatibility presentation
- offline package verification
- update state recovery after process interruption
- update metadata expiry rejection
- anti-rollback rejection for stale manifests
- signing key rotation and revoked-key rejection
- delta patch validation and fallback to full package
- DLC chunk dependency validation
- user settings/cache migration after product update

## Related Documents

- [Release Architecture](./release.md)
- [Release Security](./release-security.md)
- [ADR-112](../../adr/112-save-archive-container-and-compatibility-policy.md): save
  compatibility, migration and slot-publication identity.
- [Horo Package System](../packages/package-system.md): package updates and dependency resolution
- [Application Security](../security/application-security.md)
- [Platform Abstraction](../foundation/platform-abstraction.md)
- [Configuration System](../foundation/configuration-system.md)
