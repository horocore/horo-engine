# Configuration System Architecture

## Purpose

This document defines the typed configuration system shared by the editor,
games, CLI, MCP, Python tooling, build and release services, renderer backends,
and observability runtime.

Configuration changes behavior. It is not a general persistence store, command
bus, secret vault, or replacement for authoritative runtime models.

## Core Decisions

- Every setting has a typed schema, owner, default, validation rule, and reload
  policy.
- Configuration is resolved into immutable snapshots.
- Readers never query environment variables or JSON files directly.
- Precedence is explicit and shared across hosts.
- Secrets are represented by references to credential providers, not plain
  configuration values.
- Dynamic settings publish one committed-change notification after a validated
  snapshot becomes active.
- Unknown keys and invalid values produce diagnostics; they do not silently
  change behavior.
- Domain projectors such as ADR-103 Network consume the one resolved snapshot and
  apply typed cross-field/capability constraints; they do not implement another
  source-precedence resolver.

## Configuration Domains

Configuration is divided by ownership:

| Domain | Examples | Typical persistence |
|---|---|---|
| Engine defaults | fixed timestep, default backend | packaged resources |
| User preferences | theme, editor behavior, log filters | user config directory |
| Project settings | default scene, physics, target profiles | `.horo/project.json` |
| Project package and extension requests | desired package IDs, versions, sources and contribution groups | `.horo/packages.json` |
| Workspace settings | panel layout, local editor state | editor workspace file |
| Session overrides | temporary profiling or preview options | memory only |
| Invocation overrides | CLI and environment overrides | process launch |

Workspace presentation state remains governed by
[Project Model](../editor/project-model.md). It may use the same serialization and
validation infrastructure without becoming global configuration.

Project extension package requests are portable project configuration, but they
are not a trust grant and do not store resolved package paths. Extension package
resolution, trust decisions, and development overrides follow
[Extension System](../extensions/plugin-system.md),
[Horo Package System](../packages/package-system.md) and the dedicated
[Package Source Policy](../../adr/058-package-source-policy.md). Generic settings
precedence does not choose package authorities or resolve source conflicts.

### Network configuration specialization

[ADR-103](../../adr/103-network-project-configuration-and-build-profile-ownership.md)
uses this resolver for every editor, CLI, MCP, CI and packaged-host entry point.
The Network module owns inert `network.*` descriptors, legal sources, validation
and projection into one `EffectiveNetworkConfiguration`; Foundation remains the
only source/precedence/provenance authority.

Portable project policy, local user preview preferences, product release profiles,
memory-only host requests and private credential bindings remain distinct typed
inputs. Precedence applies only to sources permitted by each descriptor. Product
capability and security floors constrain the winning values: a higher-precedence
override cannot add an unpackaged network mode/provider, weaken trust policy,
expand a qualified network budget or import renderer/device tiers.

Project files and packaged product manifests contain no credential value, private
key or machine-specific credential binding. They may declare stable credential
requirements; private user/CI/host input binds those requirements to opaque
provider references, and only the consuming operation resolves a short-lived
secure value. Safe provenance and ordinary environment summaries omit both raw
values and machine-specific references.

## Schema

```cpp
struct SettingDescriptor {
    SettingKey key;
    SettingValueType type;
    SettingValue defaultValue;
    SettingScope scope;
    ReloadPolicy reloadPolicy;
    Sensitivity sensitivity;
    ValidationRule validation;
};
```

Descriptors are registered by the module that owns the setting. Registration is
complete before configuration resolution begins. Duplicate keys, incompatible
types, and conflicting ownership are startup errors.

Descriptor registration is performed by the composition root from validated
module descriptors. A module does not self-register settings through static
initializers or a process-global settings registry. This matters for the IDE:
Settings pages, command-line flags, MCP schemas, project validation, and Python
tooling must all see the same setting contract without each surface inventing
its own list of keys.

Keys use stable dotted names:

```text
runtime.fixed_step_hz
render.backend
render.fallbacks
editor.theme.active
editor.autosave.interval_seconds
observability.log.default_level
pipeline.max_parallel_jobs
```

## Resolution Precedence

From highest to lowest precedence:

1. explicit invocation arguments
2. environment variables
3. session overrides
4. project configuration
5. user configuration
6. packaged profile or preset
7. schema default

Not every key is legal at every layer. For example, workspace layout cannot be
set by a project committed to source control, and a release security policy
cannot be weakened by an arbitrary project value.

Environment variables are launch-time invocation inputs. Session overrides may
only override environment values for settings whose descriptor declares
`sessionOverride: true`. Otherwise the environment value remains locked until
process restart. This prevents a temporary editor profiling option or preview
switch from being silently overridden by a stale environment variable.

Each resolved value retains provenance:

```cpp
struct ResolvedSetting {
    SettingValue value;
    ConfigurationSource source;
    std::optional<SourceLocation> location;
};
```

The Settings modal may show where a value came from and why a lower-precedence
value is inactive.

### Resolution pipeline contract

`ConfigurationResolver` is the only Foundation implementation of source
precedence. A composition root supplies one `ConfigurationResolutionRequest`
containing already captured invocation, environment, session, project, user and
packaged-profile maps. Resolution validates every supplied candidate before it
publishes any value. Findings are stable and deterministic in source-precedence,
key and diagnostic order; an invalid shadowed value still fails the complete
candidate rather than being silently ignored.

Each descriptor may provide an explicit `ConfigurationSourcePolicy`. Descriptors
that predate the policy use the compatibility mapping derived from
`SettingScope`; new module contributions must declare the exact legal source set.
The schema default is always the final candidate and is not represented as an
externally writable source bit. The `sessionOverridesEnvironment` flag affects
only that pair and leaves every other precedence relationship unchanged.

Resolver inputs are bounded before publication. The default limits admit at most
1,024 keys per source, 256 bytes per key, 64 KiB per string value, 1,024 bytes of
safe source-location text, a 4 MiB document and 256 environment bindings. Hosts
may lower these limits but may not bypass them with an alternate resolver.

## Environment Variables

Environment keys use the `HORO_` prefix and an explicit mapping declared by the
owning descriptor:

```text
HORO_RENDER_BACKEND=opengl
HORO_RENDER_FALLBACKS=opengl,null
HORO_LOG_LEVEL=debug
HORO_THEME_FILE=/path/to/theme.json
HORO_RUNTIME_FIXED_STEP_HZ=120
```

Modules do not invent undocumented aliases. Empty, malformed, or unsupported
values fail validation with a source-specific diagnostic.

The environment is read once by the host adapter and converted to a safe input
map. Arbitrary environment access is not spread throughout engine modules.

`ConfigurationResolver::CaptureEnvironment()` accepts explicit `HORO_` bindings
and a borrowed `ProcessService`. It never calls process APIs directly. Missing
variables are absent candidates; empty, malformed, duplicate, unbounded or
secret-bearing values fail with safe diagnostics that contain the binding name,
never the raw value.

## Module Settings Contributions

Feature modules contribute settings through descriptors, not through UI code or
ad hoc JSON parsing:

```cpp
struct ModuleConfigurationContribution {
    ModuleId module;
    std::span<const SettingDescriptor> settings;
    std::span<const EnvironmentVariableBinding> environmentBindings;
};
```

The host validates contributions before snapshot resolution:

- setting keys must be prefixed by the owning module or a documented built-in
  namespace;
- user, project, workspace, session, and invocation scopes must be legal for the
  setting's owner;
- reload policy must match the resource lifetime affected by the setting;
- environment bindings must be explicit and must not collide with another
  module's binding;
- Settings modal pages, CLI flags, MCP schemas, and Python adapters are derived
  from the same descriptor set where practical.

This keeps the editor modular without creating separate configuration systems
for GUI, CLI, MCP, game projects, and extensions.

## Immutable Snapshots

```cpp
class ConfigurationSnapshot {
public:
    template<typename T>
    const T& Get(SettingKey<T> key) const;

    ConfigurationRevision Revision() const noexcept;
};
```

`ConfigurationSnapshot` is an immutable, reference-counted snapshot handle.
Copying it is cheap and keeps the captured revision alive for the consumer's
lifetime. It does not copy the entire resolved configuration set. The snapshot
holds the validated value map internally; readers never observe partial updates.

`ConfigurationSnapshotRef` is the same handle type used when a snapshot must be
captured without implying authority over it, for example inside a `JobDescriptor`
or an operation context.

The configuration service builds and validates a complete candidate snapshot,
then atomically publishes it. Readers receive a stable snapshot for the duration
of their operation or frame.

No reader observes a partially applied multi-key update.

## Dynamic Reload

Every setting declares one reload policy:

| Policy | Behavior |
|---|---|
| `Immediate` | New snapshot may apply at the next owned synchronization point |
| `NextFrame` | GUI/runtime swaps the snapshot at a frame boundary |
| `NextOperation` | Existing jobs retain their captured configuration |
| `ProjectReopen` | Requires closing and reopening the project |
| `ProcessRestart` | Requires host restart |

The owner validates whether a transition is legal. A dynamic renderer setting,
for example, may require resource recreation and must not be marked `Immediate`.

After commit, the configuration service publishes a bounded notification:

```cpp
struct ConfigurationChangedEvent {
    ConfigurationRevision revision;
    ConfigurationDomain domain;
    std::vector<SettingKeyId> changedKeys;
};
```

Subscribers query the authoritative snapshot. The event does not carry the
entire configuration or secrets.

For dynamic reload, the host captures all sources and calls `StageReload` off
the frame and job hot paths. Staging runs the canonical resolver over the whole
input, including shadowed values. A failed parse or validation is returned with
diagnostics and cancels the pending candidate; it cannot change the active
revision. Repeated valid stages replace the pending candidate, so a watch storm
before activation commits only the newest complete snapshot. If host file read,
environment capture, or document parsing fails before `StageReload`, the host
calls `CancelPendingReload` before surfacing the original diagnostic. This also
invalidates any resolution still in flight. The host serializes capture attempts
in source-change order so an older failure cannot cancel a newer candidate.
Staging also precomputes the sorted changed-key set and next revision so
activation does not copy the resolved map at the synchronization point.

The host calls `ActivateReload` at its owned synchronization point. A candidate
waits until **all** changed descriptors permit that point; mixed-policy changes
are never partially activated. `Immediate` admits the next explicit host safe
point. `NextFrame` requires a frame boundary; `NextOperation` requires the host's
operation-submission boundary, after current operations have captured their
configuration. Neither boundary implies the other. `NextFrameAndOperation` is
available when both conditions hold. Project reopen and process restart satisfy
the earlier boundaries. Already-running operations retain their captured
snapshot. Equal resolved values and provenance are a no-op. An explicit
`Commit` or `ResolveAndCommit` invalidates an older pending reload. Concurrent stages are generation-checked so
an earlier, slower resolution cannot replace newer input.

One event with sorted changed keys is enqueued after a successful snapshot swap.
The process-owned `EngineDataBus` delivers it only when its owner calls
`DispatchQueued`, on that owner's thread and outside the configuration lock.
The bus enqueue itself is queue-only and does not invoke subscribers; the
service still releases its lock before enqueueing.

Concurrent `ActivateReload`, explicit `Commit`, and `ResolveAndCommit` calls may
enqueue after another commit has advanced the active revision. Notifications
identify the snapshot that committed, but cross-thread delivery is not
monotonic. Subscribers compare the event revision with a freshly queried
snapshot and ignore older revisions; events never authorize replaying an older
value. The bus may also drop events under backpressure, so revision polling is
required where missing a transition would matter.

Hosts must budget the bounded bus queue for their maximum activation rate and
inspect queue-drop counters; activation and event dispatch must not run inside
render or job hot paths. The existing explicit draft `Commit` remains a
synchronous editor transaction; its subscribers receive its event on the caller
thread. Hosts that need deferred reload use the staged path.

## Settings Modal

The Settings modal edits a draft transaction:

```cpp
struct ConfigurationDraft {
    ConfigurationRevision baseRevision;
    std::unordered_map<SettingKey, SettingValue> proposedValues;
};
```

```text
active snapshot
      |
      v
validated draft
      |
      +-- preview supported values
      +-- apply atomically
      +-- cancel and roll back previews
```

The modal is a publisher and subscriber through `EditorDataBus`, but the
configuration service remains the authority. Applying a draft commits through a
typed settings operation. The service publishes the resulting change event;
the modal does not impersonate it.

If the active configuration revision changed after the draft was created, the
service either rebases safe fields or rejects the apply with
`configuration.draft_stale`. The modal must reload the active snapshot and let
the user review conflicts before retrying. This protects against concurrent
changes from CLI, MCP, or another editor panel.

Theme settings follow the component and token rules in
[GUI Design System](../editor/ui-design-system.md).

## Secrets

Configuration may contain a credential reference such as a keychain entry ID or
CI secret name. It must not contain the credential value.

Secret resolution:

- occurs only inside the operation that needs the secret
- returns an owning secure value with a short lifetime
- is never included in snapshots, events, logs, diagnostics, or workspace files
- follows [Release Security](../release/release-security.md)

## File Format And Writes

Configuration files are versioned structured documents. Writes are:

- validated before persistence
- written to a temporary file in the same filesystem
- flushed as required by the durability contract
- atomically replaced
- protected against concurrent writer loss
- deterministic in key ordering where practical

Comments and unknown extension fields are preserved only when the selected
format and schema explicitly support them.

The current JSON contract is strict `schemaVersion: 1` plus one object-valued
`values` member. Unknown document members, unsupported value kinds and oversized
inputs fail before resolution. Unknown setting keys are retained only long
enough for the resolver to report them; they are never activated.

Native persistence is owned by `ConfigurationFileStore` in `HoroPlatform`, not
by snapshot readers or `HoroFoundation`. The store acquires an exclusive lock
beside the destination, writes and flushes a deterministic snapshot document to
a fixed same-directory prepared path, and then requests an atomic replacement
from `DurableFileSystem`. A write or replacement failure removes the prepared
artifact without changing the last published document. Readers may observe the
old or new complete file during replacement, never partial content. Direct
`ConfigurationService::LoadFile()` and `SaveFile()` entry points now fail closed;
host composition migrates by reading with `ConfigurationFileStore`, parsing with
`ConfigurationResolver::ParseDocument()`, and resolving the returned source map.

## Threading

Configuration resolution and file watching may occur on workers. Snapshot
activation occurs on the owning synchronization point. Callbacks never execute
while the configuration service holds an internal registry lock.

Long-running jobs capture the snapshot revision used at submission. A later
configuration change does not silently alter an in-progress build, cook, import,
or release job.

## Testing

Required tests cover:

- precedence at every source layer
- source restrictions by setting scope
- malformed environment and file values
- atomic multi-key updates
- snapshot immutability
- dynamic reload policies and rollback
- concurrent readers during activation
- deterministic writes and recovery from interrupted writes
- secret-reference redaction
- project extension package requests do not grant trust or store resolved
  package paths
- settings modal apply, preview, cancel, and external-change behavior

## Related Documents

- [Project Model](../editor/project-model.md)
- [GUI Design System](../editor/ui-design-system.md)
- [Editor Modal Host](../editor/editor-modal-host.md)
- [Error And Diagnostics](./error-and-diagnostics.md)
- [Release Security](../release/release-security.md)
