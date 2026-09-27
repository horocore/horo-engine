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
still owns transport, private-file I/O, cancellation, and progress dispatch.
`VerifyCompletedUpdateTransfer` then checks the complete private-file size,
hash, and publisher signature before any extractor or staging marker consumes
it.

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
The updater:

1. acquires the installation lock
2. verifies staged and current manifests
3. records rollback information
4. atomically switches the active installation pointer or directory
5. starts a bounded health check
6. commits success or restores the previous version

Platform installers may implement the switch differently, but partial mixed
versions are forbidden.

## Rollback

At least one last-known-good version is retained within a disk budget. Rollback
is offered after:

- activation failure
- startup health-check failure
- explicit user selection

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
