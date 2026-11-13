# ADR-009: Configuration Schema, Precedence and Secret Boundary

- **Status**: Proposed
- **Date**: 2026-08-28
- **Supersedes**: None
- **Scope**: Foundation configuration schema, domain ownership, source precedence, environment indirection and credential references
- **Issue**: [CFG-001.1](https://github.com/HoroCore/horo-engine/issues/1821)
- **Jira**: [HORO-1777](https://horo-engine.atlassian.net/browse/HORO-1777)
- **Normative document**: [Configuration System](../architecture/foundation/configuration-system.md)

## Context

`docs/architecture/foundation/configuration-system.md` defines one typed configuration contract shared by the editor, runtime, CLI, MCP, Python, build and release hosts. Settings are declared before composition, resolved from explicitly ordered sources, exposed through immutable snapshots and never used to carry credential values.

The current implementation provides a useful typed baseline, but it is a single-layer draft/commit service rather than the documented multi-source resolver:

- `include/Horo/Foundation/Configuration.h` exposes `SettingKey`, `SettingDescriptor`, `ConfigurationSchema`, immutable shared `ConfigurationSnapshot` handles, `ConfigurationDraft` and `ConfigurationService` from the `HoroFoundation` target.
- `SettingDescriptor` carries key, type, default, one `SettingScope`, reload policy and sensitivity. It does not carry an owning module/domain, a validation rule, legal source set, explicit environment binding or the `sessionOverride` policy.
- `ConfigurationSchema::Register()` rejects duplicate keys and default/type mismatches, and `Seal()` prevents later registration. It does not validate dotted-key syntax, namespace ownership, source legality, environment collisions or module contributions.
- `ConfigurationService` starts from schema defaults and atomically replaces a shared immutable value map after validating a revision and primitive value type. It does not retain `ConfigurationSource` provenance or resolve multiple source layers.
- `LoadJson()` silently ignores unknown keys. `SaveFile()` truncates the destination directly and snapshot serialization iterates an unordered map. These behaviors do not meet the normative diagnostic, deterministic-write or atomic-replacement contracts.
- `ConfigurationChangedEvent` is published after the service lock is released and carries bounded revision/key metadata, but its domain remains `All`, changed-key ordering is not deterministic and reload policy is not enforced.
- Editor settings remain authoritative in `EditorSettingsService` and `EditorSettingsStore`; four appearance values are projected into Foundation configuration. This is a temporary adapter, not a complete shared configuration authority.
- `SettingSensitivity::SecretReference` exists, but the schema cannot validate reference syntax or prevent a caller from supplying a credential value as an ordinary string. No configuration-owned credential resolution path exists.
- Configuration values are not resolved from environment bindings. Direct environment reads that exist elsewhere are host/platform bootstrap behavior and must not become a second settings resolver.

[CFG-001.1] requires a ratify-or-revise decision for this baseline, an explicit ownership map, one precedence contract shared by all hosts and a strict boundary between configuration and credentials.

## Decision

**Foundation owns inert configuration types, schema validation, source-resolution contracts and immutable snapshots. Modules own their setting descriptors and stable namespaces. The application composition root collects and validates contributions, captures host inputs once and constructs the active resolver. Credential providers own secret values; configuration carries only bounded opaque references.**

### Ratify-or-revise outcomes

| Area | Current state | Outcome |
|---|---|---|
| `SettingKey`, primitive `SettingValue` and type checking | Typed Foundation values exist | **Ratified for the current value set.** New value kinds require typed schema additions, not string parsing at call sites. |
| `ConfigurationSchema` registration and sealing | Explicit builder; duplicate keys and invalid defaults fail | **Ratified as the inert Foundation baseline.** Registration remains side-effect free and sealing precedes resolution. |
| Descriptor ownership and validation | No module owner, validation rule, legal-source set or environment binding | **Revised.** Module-owned contributions and composition-time namespace/source validation are implemented by [CFG-001.3]. |
| `SettingScope` | One enum mixes ownership/persistence concepts and cannot express legal source sets | **Revised.** It remains a compatibility field until the typed source-policy contract in [CFG-001.2]/[CFG-001.3] replaces it; it is not the authoritative domain owner. |
| Immutable shared snapshots | `shared_ptr<const Data>` preserves captured revisions | **Ratified.** Readers continue to observe one complete revision and never a partially applied update. |
| Resolution and provenance | Defaults plus successive draft commits; no `ConfigurationSource` | **Revised.** [CFG-001.2] adds the single resolver and per-value provenance. Direct `LoadJson()` commits are not the final resolution model. |
| Unknown and malformed inputs | Unknown JSON keys are ignored; primitive parse/type errors stop at the first failure | **Revised.** Unknown, illegal-source and malformed values produce typed diagnostics in the deterministic validation order defined below. |
| Persistence | Direct truncating write and unordered serialization | **Revised.** [CFG-001.2] owns versioned deterministic documents, temporary same-filesystem writes, durable flush and atomic replacement. |
| Reload and notifications | Immutable commit and post-lock event exist; reload policy/domain projection are not enforced | **Ratified as a starting mechanism, revised as a lifecycle contract.** [CFG-001.4] owns synchronization-point activation, rollback and one deterministic committed-change notification. |
| Editor settings adapter | Editor model is authoritative; four values are projected into Foundation configuration | **Ratified only as migration compatibility.** [CFG-001.5] moves the Settings modal to the shared authority without preserving two independently mutable sources of truth. |
| `SecretReference` sensitivity | Marker exists; reference validation and credential-provider boundary do not | **Revised.** [CFG-001.6] introduces bounded typed references and operation-local credential resolution; raw secrets remain forbidden. |

### Ownership map

Ownership has three independent dimensions and must not be inferred from persistence location:

| Concern | Owner | Rule |
|---|---|---|
| Setting identity and descriptor | Declaring built-in or extension module | Stable dotted key lives under the module's registered namespace; descriptors are inert. |
| Schema/contribution validation | Application composition root using Foundation contracts | Collect all selected contributions, reject duplicates/collisions and seal once before resolution. |
| Resolution algorithm and immutable snapshot representation | `HoroFoundation` | Apply the canonical source order, retain provenance and publish complete revisions. |
| Engine defaults | Declaring module descriptor | Default is always present and type-correct before registration succeeds. |
| Packaged profile/preset | Distribution/application composition | Supplies signed or packaged policy input; it does not redefine descriptors. |
| User configuration | Host user-settings adapter | Persists user preferences in the platform user-config location. |
| Project configuration | Project/application layer | Owns portable project settings and project extension requests; requests never grant trust. |
| Workspace configuration | Editor workspace owner | Owns local presentation/layout state and never becomes portable project authority. |
| Session overrides | Active host/session owner | Memory-only, bounded by descriptor policy and discarded with the session. |
| Invocation/environment inputs | GUI/CLI/MCP host adapter | Capture once at startup/request boundary and translate into typed source maps. |
| Credential values | Platform/release credential provider | Resolve only inside the consuming operation; never enter configuration snapshots, events, files, logs or diagnostics. |

`ConfigurationDomain` remains a notification/query projection during migration. It does not assign schema ownership. Module identity plus the validated key namespace is the authoritative ownership contract.

The current production schema has no orphan descriptors: `editor.theme.active`, `editor.appearance.accent_color`, `editor.appearance.ui_scale_percent` and `editor.appearance.code_font_size_px` are all owned by the Editor module, use the `editor.*` namespace and are user-persisted preferences. Test-only descriptors remain owned by their test composition roots. Every new production descriptor must enter through the module-contribution validation path; adding a key directly to a host-local parser does not establish ownership.

### Canonical source precedence

All hosts use this default order from highest to lowest. The descriptor-gated environment/session exception below is the only permitted inversion:

1. explicit invocation arguments;
2. environment bindings captured by the host;
3. session overrides;
4. project configuration;
5. user configuration;
6. packaged profile or preset;
7. schema default.

The resolver evaluates only sources permitted by the descriptor. Environment variables are not discovered from dotted keys: each mapping is explicitly declared by the owning module, uses the `HORO_` prefix and is collision-checked at composition.

The documented `sessionOverride` exception is retained. When both permitted environment and session candidates exist for one setting, the resolver selects the session candidate only when that setting's descriptor enables `sessionOverride`; otherwise it selects the environment candidate. When only one of those candidates exists, the resolver selects it if the descriptor permits its source. This conditional comparison changes no other pair in the precedence order and is not an alternate host-specific precedence list.

Foundation and feature modules never call `getenv()` to obtain setting values. A host/platform adapter captures the environment once and passes a bounded input map into composition. CLI, GUI, MCP and test hosts use the same resolver API; they may provide different source maps, but not different precedence rules.

Each resolved value records its winning source and optional safe source location. Presentation surfaces may explain shadowed values from this provenance, but callers branch only on typed results and setting identities.

### Validation and diagnostic ordering

Candidate inputs that can be parsed safely are validated completely before activation. The resolver emits findings in `(source precedence, setting key, diagnostic code, source location)` order and returns one typed failure carrying that ordered diagnostic list. It never activates the valid subset of a failed candidate.

Failing immediately is reserved for conditions that make further bounded inspection unsafe or meaningless, such as an unreadable document, an oversized input or invalid document syntax. The returned typed error still identifies the source and preserves any diagnostics produced before parsing became impossible. Hosts do not choose their own fail-fast or aggregation policy.

### Secret and credential boundary

`SecretReference` means an opaque identifier for a credential-provider entry, not a secret string with a sensitive label.

- descriptors explicitly declare whether a setting accepts a credential reference;
- reference syntax and size are validated before activation;
- configuration files and snapshots may contain the reference identity only;
- the consuming operation resolves the reference through an explicitly injected credential provider;
- the resolved value is owning, short-lived and excluded from snapshots, events, serialization, logs, diagnostics and support bundles;
- missing credential capability or reference resolution returns a typed error at the operation boundary;
- environment variables containing credential values are operation inputs owned by the host adapter and are never copied into the configuration snapshot.

Marking a plain `std::string` as `SecretReference` does not make an arbitrary value safe. [CFG-001.6] owns the typed reference and provider integration; until then, production descriptors must not accept credential values through `ConfigurationService`.

### Migration boundaries

- [CFG-001.2] adds source maps, provenance, the resolver and durable persistence without changing the public precedence order decided here.
- [CFG-001.3] adds module contribution descriptors, namespace ownership and environment-binding validation without ambient registration.
- [CFG-001.4] enforces reload policies, synchronization points, rollback and deterministic notification.
- [CFG-001.5] migrates editor settings/modal persistence to the shared authority and removes the temporary dual-model projection.
- [CFG-001.6] adds credential-reference validation and provider-backed resolution.

No follow-up may introduce a host-local precedence list, direct module environment lookup, process-global schema registry or raw credential value in configuration as a compatibility shortcut.

### CFG-001.2 implementation record

[CFG-001.2] implements the decision through `ConfigurationResolver`, typed
source maps, per-key source policies, safe provenance and reference-counted
immutable snapshots in `HoroFoundation`. Environment capture is explicit and
uses the injected `ProcessService`; snapshot readers have no environment or file
handle. Until [CFG-001.6] supplies a typed credential-reference value, external
values for `SecretReference` descriptors fail closed and secret-marked defaults
are excluded from serialization.

Versioned parsing is bounded and strict. Durable publication is implemented by
the `HoroPlatform` `ConfigurationFileStore` over `DurableFileSystem`, preserving
Foundation's dependency direction while providing same-filesystem preparation,
required flushes, exclusive-writer protection and atomic replacement. The prior
direct `ConfigurationService` file methods are fail-closed compatibility stubs;
callers migrate to the composed store and pass its parsed source map to the one
resolver.

## Consequences

### CFG-001.6 implementation record

`CredentialReference` is owned by the `HoroFoundation` public-header boundary so
the immutable configuration value can carry the existing Security identity
without a reverse dependency. `Horo::Security::CredentialReference` remains a
source-compatible alias of that same type. Consumers must rebuild because the
canonical C++ type ownership and the `SettingValue` variant have changed.
Descriptors previously using a string default with `SecretReference` sensitivity
must migrate to `SettingValueType::CredentialReference` and an unset or validated
`CredentialReference` default. String values are rejected for those descriptors.

The bounded serialized identity is `cred:v1:` and 48 lowercase hexadecimal
digits, matching `CredentialVault` issuance. JSON uses an explicit
`{"credentialReference":"cred:v1:..."}` value. An unset default is omitted from
persistence. Environment bindings parse that identity through the same validator.
Identity parsing establishes syntax only. The explicitly supplied
`Security::CredentialProvider` retains authority over existence, expiry and
revocation. `CredentialVault` implements that operation-facing contract; its
raw `CredentialBackend` is not the configuration resolution interface.

`Horo::Security::ResolveConfigurationCredential` resolves inside a consuming
operation and returns the existing move-only `SecureBytes` owner. Provider error
payloads are replaced with safe typed descriptors before crossing this boundary.
The operation must release the secret when it finishes. Snapshots, JSON and
notifications carry only references or key/revision metadata. Native provider
qualification remains owned by SEC-001 downstream work.
The operation supplies its observation time explicitly in the provider's expiry
domain. Primitive CLI option parsing rejects reference-valued configuration with
a safe incompatible-value diagnostic; it does not coerce the reference to text.

### CFG-001.5 and CFG-001.7 implementation migration

Descriptors may declare inert localization identities, inclusive integer ranges,
bounded string rules and read-only presentation policy. The resolver enforces
these rules on every admitted source, including shadowed values. Portable key
segments contain ASCII letters, digits, underscores or hyphens and are separated
by single dots. Existing callers with invalid keys or sensitive string defaults
must migrate before registration succeeds. No descriptor invokes a service.

Drafts name their authored source; existing callers retain Session semantics,
while the editor writes User values. Snapshots retain immutable supplied values
and provenance for every admitted layer. Changing a shadowed value advances the
revision, so an editor cannot overwrite an externally changed user preference
just because an invocation override still wins. Unchanged complete inputs do not
publish another configuration revision.

Editor legacy appearance fields project the shared configuration authority. The
combined modal operation reserves a same-thread prepared configuration write,
validates and persists one editor document, then activates both projections
before notifying observers. Destruction before activation cancels the prepared
write. Persistence uses `SourceToJson(User)`, never resolved invocation or
environment values. Loaded nested configuration is untrusted ingress and cannot
be written through the low-level store without a validated snapshot.

Theme hosts now supply captured UTF-8 override and user-resource roots explicitly
to `Theme::Apply`/`RefreshThemeList`; the theme reader no longer discovers these
paths from ambient environment variables. Callers that need user themes must
pass the user-resource root. The optional arguments preserve built-in-only uses.

- The current schema and immutable snapshot code remain valid foundations rather than being replaced wholesale.
- Current `ConfigurationService` file loading is explicitly a prototype ingress, preventing new callers from treating last-write order as canonical precedence.
- Domain ownership is derived from validated module identity and key namespace, while persistence/source policy remains independently typed.
- GUI, CLI, MCP, Python and tests converge on one resolver contract and can explain the same winning value through provenance.
- Editor settings migration must end with one authoritative commit path; persistence and activation failures cannot leave two committed revisions.
- Credential references can be portable where policy permits, while credential values remain provider-owned and short-lived.

## Rejected Alternatives

- **Treat `SettingScope` as both module ownership and source precedence.** Rejected: one enum cannot express namespace ownership, legal source sets and the environment/session exception without conflating independent contracts.
- **Let each host merge configuration in its own startup code.** Rejected: precedence and validation would drift across GUI, CLI, MCP, Python and tests.
- **Derive environment variable names mechanically from dotted keys.** Rejected: aliases, collisions and compatibility become implicit and cannot be validated before activation.
- **Keep editor settings and Foundation configuration as permanent peer authorities.** Rejected: cross-surface commits can diverge or partially succeed, leaving no single committed revision.
- **Store raw credentials in a sensitive `SettingValue`.** Rejected: a sensitivity marker does not provide secure lifetime, redaction or provider isolation.
- **Implement the complete resolver in this M0 decision.** Rejected: the focused follow-up tickets own implementation and regression coverage; this ADR fixes their shared contract first.
