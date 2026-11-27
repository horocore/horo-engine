# ADR-116: Save Data Threat Model and Trust Policy

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Protected save assets, attacker capabilities, source trust classification, bounded admission, integrity/authenticity/replay policy, tool authority, credentials, development/shipping profiles and security non-goals
- **Issue**: [SAV-008.1](https://github.com/HoroCore/horo-engine/issues/1495)
- **Jira**: [HORO-1495](https://horo-engine.atlassian.net/browse/HORO-1495)
- **Related**: [ADR-002](002-credential-handling.md), [ADR-008](008-error-model-exception-boundary-and-registry.md), [ADR-112](112-save-archive-container-and-compatibility-policy.md), [ADR-113](113-local-storage-user-profile-and-slot-ownership.md), [ADR-114](114-canonical-runtime-world-persistence-boundary.md), [ADR-115](115-cloud-save-authority-revision-and-conflict-policy.md)
- **Normative documents**: [Save Game And Persistence](../architecture/runtime/save-game-and-persistence.md), [Application Security](../architecture/security/application-security.md), [Platform Services](../architecture/runtime/platform-services-architecture.md), [CLI Architecture](../architecture/interfaces/cli-architecture.md), [MCP Architecture](../architecture/interfaces/mcp-architecture.md), [Release Security](../architecture/release/release-security.md)

## Context

A `.horosave` may have been created locally, copied from another installation,
downloaded through a provider, emitted by an older migration, modified for a modded
game, signed by a server or selected by a tool. Its path and source label do not prove
that its bytes are structurally safe, compatible, authentic, current or authorized
for the requested namespace.

The existing save decisions define canonical state, immutable archives, namespace
ownership, transactional restore and revision-aware cloud conflicts. They do not by
themselves state one threat model across the parser, storage adapter, UI, CLI, MCP,
cloud provider, migration process, release profile and credential boundary. In
particular, a content hash detects accidental or deliberate byte changes but does not
authenticate an author; a valid signature authenticates a permitted key and scope but
does not prove freshness; encryption provides confidentiality only when separately
selected and correctly keyed.

Treating local files as inherently trusted exposes the runtime to malformed lengths,
decompression bombs, path races, wrong-profile substitution and replay. Conversely,
claiming that signature checks, capability checks or native plugin permissions create
a general anti-cheat boundary or sandbox would promise protection the engine cannot
provide inside a user-controlled process.

This ADR defines the protected assets, attacker model, source classification,
admission pipeline, owning controls, host profiles, tool surfaces and deliberate
non-goals. It does not choose a product-specific anti-cheat system, encrypt ordinary
local saves or make a compromised client authoritative over server gameplay.

## Decision

### 1. Protected assets and authorities are explicit

The policy protects:

- process memory safety and bounded CPU, memory, disk and decompression work;
- the last-known-good local slot and every conflict/migration recovery generation;
- product, environment, platform-user, game-profile, server-world and logical-slot
  namespace separation;
- canonical gameplay/world state from partial, incompatible or wrong-scope restore;
- provider-account and cloud-revision isolation across users and devices;
- signing private keys, cloud credentials and credential-provider references;
- diagnostic and tool surfaces from leaking raw save content or private identity; and
- availability of local save/load when cloud or external tooling is unavailable.

Authority remains divided:

| Decision or asset | Owner |
|---|---|
| Framing, archive integrity, schema compatibility and participant decode | Runtime Save codecs and compatibility policy |
| Physical roots, containment, slot leases, atomic publication and quarantine storage | `SaveStorageAdapter` |
| Semantic participant validation and detached restore candidate | Owning canonical adapter and `RuntimeSaveService` |
| Signature requirement, trusted key/scope and replay floor | Trusted host/product/namespace policy |
| Cloud session, provider revision and opaque blob transport | Platform Services backend |
| Cloud lineage, download admission, conflicts and recovery copies | `CloudSaveCoordinator` |
| Capability approval and operation admission for UI/CLI/MCP | Application host and security policy |
| Key use and provider authentication | Credential/signing provider |
| Presentation and confirmation | UI, CLI or MCP adapter; never persistence authority |

No parser, archive field, filename, provider timestamp, UI choice or tool caller may
select its own trust root, weaken the host profile or publish directly into a live
slot.

### 2. The attacker model covers bytes, storage and invocation

The baseline assumes an attacker or fault may:

- supply arbitrary, truncated, oversized, contradictory or non-canonical bytes;
- forge counts, offsets, codecs, compression ratios, hashes, metadata, identifiers,
  schema versions, lineage, timestamps and signature descriptors;
- replace or mutate a file between discovery, verification and publication;
- exploit symlinks, reparse points, case/Unicode aliases, traversal, hard links,
  duplicate normalized names and storage exhaustion;
- replay an old but validly signed generation or copy data across product, profile,
  slot, server-world or provider-user scopes;
- race local writers, other processes, cloud devices, profile switching, deletion,
  cancellation and shutdown;
- return malformed, stale, reordered or attacker-controlled cloud bytes and metadata;
- invoke inspection, import, export, migration, load, delete or conflict commands
  through UI, CLI or MCP without the required capability; and
- induce parsers/migrations to consume excessive memory, CPU, recursion, temporary
  storage or diagnostic output.

The ordinary client process, operating-system account and storage provider are not
assumed uncompromised anti-cheat authorities. A malicious native module running in
the host can inspect process memory and call operating-system APIs outside Horo's
interfaces. That case is addressed by code trust, process isolation where applicable
and server authority, not by pretending the save parser is a sandbox.

### 3. Every archive starts as untrusted bytes

Trust is classified by evidence and requested use, never by directory or transport:

| State | Meaning | Permitted use |
|---|---|---|
| `UntrustedBytes` | Origin is known at most as bounded provenance; contents are not admitted | Bounded acquisition, framing probe and quarantine only |
| `IntegrityVerified` | Exact framed bytes match `ArchiveContentHash` and decoded entries match their declared hashes | Compatibility and semantic validation; not proof of author or freshness |
| `AuthenticatedScope` | A required/present signature verifies under a host-selected key authorized for the declared product/user/world/slot scope | Continue policy evaluation; not proof of latest generation |
| `CompatibleCandidate` | Versions, required participants, dependencies and deterministic migrations are admitted | Detached decode/prepare only; no live mutation |
| `SemanticallyPrepared` | Every required owner validated a complete detached candidate for one expected runtime/base revision | Aggregate no-fail restore commit at the owning safe point |
| `LocallyPublished` | A complete archive generation was atomically durably published under the expected slot lease | Eligible local source; bytes are reverified when later read |
| `Quarantined` | Verification, scope, policy or identity failed, or provenance must be retained for investigation | No load/migrate/sync winner use; bounded metadata-only inspection |

`LocallyPublished` describes a completed storage transaction, not permanent trust in
future reads. External mutation, disk faults or policy changes require the archive to
pass admission again. Catalog membership, a `.horosave` suffix, location under a save
root, cloud authentication and a platform “download complete” result grant no higher
state.

### 4. Source policy is classified before admission

| Source | Required baseline policy |
|---|---|
| Local slot | Open only through typed address and storage lease; reverify framing/integrity/signature/scope/compatibility on read |
| Imported/copy/guest save | Explicit import capability and source provenance; verify in operation-owned staging; publish as a new destination generation without modifying the source |
| Cloud download | Treat bytes and provider metadata as untrusted; enforce size limits before/during transfer, then local verification, scope checks and CAS-aware coordinator publication |
| Migration input | Verify the original under its source policy first; migrate detached with bounded work; finalize and sign a new generation under destination policy |
| Modded save | Admit only under an explicit product/mod profile and isolated namespace; never weaken a secure production/server namespace or trust native mod code |
| Server-signed save | Require a scope-authorized server key and requested world/account binding; separately enforce trusted replay/generation policy where rollback matters |
| Tool-inspected save | Bounded metadata/framing inspection may run without load authority; inspection success does not admit migration, import, restore, export or mutation |

Unknown origin is permitted only where the selected import/inspection policy explicitly
accepts it as untrusted input. Provenance labels help audit and user decisions but do
not substitute for cryptographic or semantic evidence.

### 5. One bounded admission pipeline owns all sources

Every load, import, migrated output and cloud download follows these stages:

1. Resolve a trusted operation profile, requested typed source/destination address,
   capability set, byte/work budgets and cancellation policy.
2. Acquire an operation-owned handle or staging object through the storage/transport
   adapter; do not follow archive-supplied paths.
3. Read the fixed framing with checked arithmetic and enforce total/stored byte limits
   before payload allocation.
4. Verify exact framing, `ArchiveContentHash` and trailer form before trusting payload
   control fields; reject trailing, overlapping, duplicate or unreferenced bytes.
5. Apply host-selected signature/key/scope and optional anti-replay policy. The
   archive cannot request `Disabled`/`Optional`, add a trust root or lower a floor.
6. Parse deterministic metadata and entry tables with limits on counts, lengths,
   compression expansion, nesting, strings, diagnostics and total work.
7. Preflight archive/save/participant compatibility and build one complete bounded
   migration plan; unknown required semantics fail closed.
8. Decode and semantically validate every required participant in detached storage.
9. Revalidate source/destination identities, expected generations, leases, profile
   epoch and cancellation gate before the no-fail restore or atomic publication.
10. Publish one typed result and safe audit summary, then retire staging/recovery data
    according to policy.

Parser or migration failure never mutates the active runtime, live catalog or source
archive. A resource-limit result is not retried with an automatically larger/unbounded
profile. High-risk third-party codecs may run in a restricted helper process, but
process isolation does not remove the same format, integrity and semantic checks.

### 6. Integrity, authenticity, confidentiality and freshness stay separate

`ArchiveContentHash` and participant hashes detect changed bytes; they are not a MAC,
identity proof or permission. `SaveSignaturePolicy::Optional` authenticates every
present valid signature but cannot detect signature stripping from an otherwise
unsigned-acceptable source. `Required` provides tamper/authorship protection only for
keys and scopes selected by the host.

A valid signature does not prove that a generation is newest, authorized to overwrite
another generation or free of hostile-but-schema-valid data. Replay-sensitive products
use a separately trusted monotonic generation/receipt/ledger bound to the namespace;
attacker-controlled timestamps and archive counters are not anti-rollback state.

The v1 runtime save container provides no confidentiality. Product policy that stores
sensitive data must avoid it, minimize it or introduce a separately reviewed
authenticated-encryption envelope with key rotation, recovery and platform backup
behavior. Release `.horo` archive encryption policy does not implicitly encrypt
`.horosave` files.

### 7. Credentials never cross the save data boundary

Archives, catalogs, sync journals, recovery copies, migration metadata, CLI/MCP
requests/results and diagnostics contain no raw credential, private key, reusable
authorization header, provider token or machine-local secret value. Portable policy
may name a stable public key/trust-policy requirement; private bindings remain in the
credential/signing provider.

The consuming operation obtains a short-lived scope-limited credential/key lease only
after capability and namespace admission. Platform Services uses provider credentials
only inside the selected backend request. Runtime Save submits a bounded signing or
verification request without receiving private key material. Cancellation, sign-out,
profile switch and shutdown revoke/retire leases before operation state is released.

Errors and audit records use safe stable key/provider/policy IDs only when policy
allows them. They never echo credential handles, paths into credential stores or raw
archive data.

### 8. UI, CLI and MCP are adapters over the same use cases

Surfaces may expose separate capabilities such as `save.inspect`, `save.import`,
`save.export`, `save.migrate`, `save.load`, `save.delete` and
`save.conflict.resolve`. A capability authorizes one bounded operation; inspection
does not imply load/mutation, and project trust does not imply access to another user,
environment or server namespace.

Tools pass typed addresses or explicitly admitted external-file handles to application
use cases. They never pass archive-selected destination paths, call codecs directly,
hold slot/provider leases, access credentials or mutate the catalog/runtime from a
transport/UI thread. Destructive or external-source operations follow confirmation
policy and optimistic generation/revision checks.

Inspection returns bounded allowlisted metadata, validation state and safe diagnostics.
Raw participant bytes, complete decoded gameplay state, credentials, private platform
identity and unrestricted filesystem paths are excluded by default. Development-only
raw export requires a distinct capability, explicit destination root and audit policy;
it cannot be enabled by an archive or remote caller.

### 9. Development and shipping profiles differ only in admitted authority

All profiles enforce framing, checked arithmetic, parser/decompression/work budgets,
path containment, cancellation, transactional publication, credential isolation and
safe diagnostics. Debug builds cannot disable memory-safety or storage invariants.

Development/test profiles may enable metadata inspection, isolated unsigned/modded
namespaces, deterministic fixture generation, raw developer export and verbose safe
diagnostics through explicit local capabilities. Their product/environment IDs and
roots remain separate from shipping/server data. A file is never promoted between
profiles by rename or directory copy.

Shipping profiles expose only product-declared import/mod/tool operations, use fixed
fail-closed compatibility and signature policies, deny remote MCP save mutation unless
explicitly configured/authenticated, and omit parser bypasses or arbitrary raw export.
Secure server/account namespaces require scope-authorized signatures and any declared
anti-replay authority. User-controlled local single-player products may deliberately
accept unsigned saves, but must describe that as lack of tamper protection rather than
as a trusted or secure-save mode.

### 10. Failures are typed, quarantined and safely observable

Stable policy outcomes include CorruptOrMalformed, ResourceLimitExceeded,
IntegrityMismatch, SignatureRequired, SignatureInvalid, UntrustedSigner, WrongScope,
ReplayRejected, UnsupportedVersion, MigrationUnavailable, PolicyDenied,
CapabilityDenied, StaleGeneration, ProviderPreconditionFailed and Quarantined.
Adapters map them to their stable CLI/MCP/UI envelopes without collapsing security
denial into ordinary not-found or silently falling back to a weaker policy.

Security events record bounded source kind, requested operation, typed namespace
identity, safe archive/generation hash correlation, effective policy ID, result and
actor/session identity allowed by privacy policy. They do not retain raw save bytes,
decoded state, labels/free text, local paths, credentials or private provider metadata.
Repeated malformed/resource-limit/authentication failures are rate-limited without
hiding the terminal operation outcome.

Storage and archive protection providers process unclassified request, payload,
key-reference and scope data. The engine suppresses diagnostic publication during
storage execution, archive encryption and decryption, and signature verification.
The same policy applies to callbacks that capture, migrate, read, write or restore
detached Save state, including gameplay checkpoint staging. Forwarded diagnostic
contexts keep that policy. The engine emits its typed operation and stage outcomes
outside those callback scopes.

### 11. Qualification proves the boundary and non-goals

Required evidence covers:

- each source kind entering as `UntrustedBytes`, including a file already present in
  the canonical save directory and a provider-authenticated download;
- truncated/oversized/overflowing framing, duplicate/overlap/gap/trailing bytes,
  hostile counts/strings/nesting/codecs and decompression/work/disk exhaustion;
- symlink/reparse/hard-link, Unicode/case alias, replacement-after-open and
  source/destination namespace traversal races;
- hash/signature stripping/wrong key/wrong scope, old valid replay, copied slot/profile/
  world data and attacker-controlled timestamp/generation claims;
- malformed, signed-but-semantically-invalid and compatible-but-prepare-failing
  participants leaving active runtime and prior local generation unchanged;
- import/migration/cloud failure and cancellation at every staging, verification,
  signing, CAS, local-publication and recovery-retention boundary;
- capability separation across inspect/import/export/migrate/load/delete/conflict,
  including GUI/CLI/MCP equivalence and remote denial in shipping policy;
- no credential/raw-state/path leakage through results, logs, history, crash bundles
  or conflict UI; and
- development/shipping, unsigned/modded/secure-server and no-cloud profiles with no
  cross-namespace promotion or automatic policy downgrade.

## Consequences

### Positive

- Local, imported, migrated, modded and cloud saves share one fail-closed admission
  contract instead of inheriting trust from their location.
- Integrity, signature, confidentiality and anti-replay claims are precise.
- Tooling can inspect hostile inputs without gaining restore or mutation authority.
- Development flexibility cannot silently weaken shipping/server namespaces.

### Costs

- Hosts must define concrete budgets, capability maps, key/scope policy and optional
  anti-replay storage per product profile.
- Import, cloud and migration paths require staging/quarantine and fault-injection
  coverage instead of reusing a direct local-file load shortcut.
- Products needing confidentiality or adversarial client enforcement need additional
  reviewed systems beyond the v1 save container.

## Rejected Alternatives

### Trust files inside the save directory

Rejected because local malware, users, sync tools, other processes and storage faults
can replace those bytes. Directory containment limits path authority; it does not
validate content.

### Treat a matching SHA-256 digest as authenticated

Rejected because an attacker who changes the archive can recompute an unkeyed digest.
The hash is integrity identity, while authorship requires host-selected signature
policy.

### Treat every valid signature as current and safe

Rejected because a valid old generation can be replayed and schema-valid content can
still violate semantic or resource policy. Scope, compatibility, semantic validation
and optional anti-replay state remain independent gates.

### Give inspection tools direct codec or filesystem access

Rejected because adapters would bypass capability, budget, namespace, credential,
lease and audit ownership. Tools use the same bounded application use cases.

### Disable parser limits in development builds

Rejected because malformed fixtures, imported projects and compromised dependencies
can still attack the developer host. Development may add explicit capabilities and
diagnostics, not remove memory-safety and transactional invariants.

### Describe save signing as anti-cheat or sandboxing

Rejected because a user-controlled native process can bypass client-side interfaces
and a trusted native module is not isolated by capability checks. Competitive truth
belongs to an authoritative server or a separately designed anti-cheat system.

## Non-Goals

- Preventing a local user or native code with equivalent operating-system authority
  from reading or modifying all client process/storage state.
- Providing general malware scanning, DRM, anti-cheat or a sandbox for native mods.
- Encrypting v1 `.horosave` content or guaranteeing confidentiality from local/cloud
  administrators.
- Recovering data after every copy, recovery generation and provider backup is lost or
  maliciously deleted.
- Proving freshness from timestamps, archive counters, filenames or signatures without
  a separately trusted anti-replay authority.
- Making client saves authoritative for server-owned competitive state.
