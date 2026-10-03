# Application Security Architecture

## Purpose

This document defines application trust boundaries for projects, plugins,
gameplay code, assets, subprocesses, files, MCP access, credentials, diagnostics,
and update operations.

Release signing and credential details remain in
[Release Security](../release/release-security.md). This document governs the running
editor and CLI hosts.

## Core Decisions

- Projects and externally supplied packages are treated as untrusted input until
  the user grants the required trust.
- Opening a project does not automatically execute its code, tools, scripts, or
  plugins.
- File access is rooted and path normalized.
- Process execution uses explicit executable and argument arrays with policy
  checks.
- Native plugins and gameplay modules are trusted code, not falsely described
  as sandboxed.
- MCP binds locally by default and requires authenticated, capability-scoped
  access for remote exposure.
- Runtime console and remote-console access are profile-gated, permission-scoped,
  and denied remotely by default.
- Real-time transport connectivity is never application trust. `NetworkRuntime`
  owns bounded protocol/session admission under an immutable host-selected
  loopback, LAN or remote trust policy before gameplay dispatch.
- Secrets remain in credential providers and short-lived secure memory.
- Package source credentials and credential handles never enter package/project
  manifests, lockfiles, cache provenance exported to projects, logs, diagnostics
  or operation history. Source adapters resolve one short-lived origin-scoped
  lease just before an approved request under
  [ADR-058](../../adr/058-package-source-policy.md).
- Security decisions are explicit, auditable, and revocable.

## Implemented Security Primitive Baseline

[ADR-178](../../adr/178-application-security-primitive-and-signature-baseline.md)
defines the shared executable baseline. `HoroSecurity` owns typed security
errors, move-only zeroizing secret values, opaque credential references,
credential lifecycle coordination, trusted signing roots, and exact-artifact
signature evidence. The public contracts do not expose MbedTLS or native OS
types.

Secure random bytes come only from the host-composed OS entropy provider. A
missing or failed entropy, credential, trust, or signature provider is a typed
failure and never selects a plaintext, unsigned, hash-only, or pseudorandom
fallback. Evidence is bound to the current SHA-256 digest and cannot be directly
constructed by callers.

## Trust Domains

| Domain | Default trust |
|---|---|
| Packaged engine binaries/resources | Trusted after installation verification |
| User-created local project data | Data-trusted, code execution not implicit |
| Downloaded project/archive | Untrusted |
| Built-in plugin | Trusted with product |
| External native plugin | Untrusted until approved |
| Project gameplay binary | Untrusted until build/run approval |
| MCP local client | Authenticated local policy |
| MCP remote client | Denied unless explicitly configured |
| Runtime console local user | Product-profile and project-policy scoped |
| Runtime console remote client | Denied unless diagnostics policy enables authenticated access |
| Imported asset | Untrusted parser input |
| Local, imported, migrated or modded save archive | Untrusted parser input; source policy selects permitted operation |
| Cloud-downloaded save archive | Untrusted parser input even after provider authentication/transport success |
| Scope-authorized signed save archive | Authenticated bytes for that scope; compatibility, semantics and freshness remain unproven |
| Toolchain executable | Allowed only through resolved trusted profile |

Trust is scoped by project identity, plugin identity/version, capability, and
where practical executable hash. A broad permanent "trust everything" switch is
not part of normal UI.

## Project Trust

Opening an untrusted project permits safe inspection and validation but blocks:

- project-defined executable code
- external native plugins
- build hooks and custom tools
- arbitrary subprocess execution
- credential access
- network-capable project operations

The editor presents the requested capabilities and their source before trust is
granted. Trust records are user-local and not stored in the project.

The public `Horo/Security/AgentPolicy.h` contract adds a host-owned, in-memory
agent-policy boundary without changing existing credential or MCP APIs. Host
composition must pass an exact project/trust revision when previewing agent
context or approving a tool; replacing that revision invalidates pending grants.
Migration is additive: future agent-provider and MCP-controller callers must use
the policy at dispatch, while existing non-agent security callers are unchanged.
The policy stores only bounded typed decision metadata and content hashes for
pending consent, never raw preview text, arguments, or secrets.

## Path Security

All project-relative paths are normalized and checked against their allowed
root after resolving `..`, symlinks where relevant, archive entries, and native
path semantics.

Sensitive operations define separate roots for:

- project read
- generated project write
- source asset write
- package extraction
- user config and state
- temporary files

Archive extraction rejects absolute paths, traversal, duplicate normalized
entries, unsafe links, and resource-expansion limits.

## Asset And File Parsing

Parsers treat sizes, counts, offsets, compression ratios, and recursion depth as
untrusted. They validate before allocation and use configured resource limits.

Renderer component manifests are native-code authority inputs and use the stricter
[ADR-053](../../adr/053-renderer-module-manifest-parser.md) contract: bounded
canonical JSON parsing and internal semantic/path validation complete before
archive extraction, dependency resolution, probe, or library load. Parsed fields
cannot select their own trust root; later signature/catalog verification remains
bound to the exact canonical digest returned by the parser.

High-risk or historically unsafe third-party parsers may run in a restricted
helper process with bounded I/O.

## Save Data Trust

[ADR-116](../../adr/116-save-data-threat-model-and-trust-policy.md) defines the
application threat model for runtime save data. No save becomes trusted because it is
inside a save root, listed in a catalog, downloaded under an authenticated provider
session, produced by a migration or signed by some key. Every read begins with bounded
untrusted bytes and advances only through explicit integrity, host-selected
signature/scope, compatibility, semantic preparation and generation/lease gates.

The save operation profile separates inspect, import, export, migrate, load, delete
and conflict-resolution capabilities. UI, CLI and MCP are adapters over the same
application use cases; inspection authority cannot publish or restore a slot. Raw
participant state, unrestricted local paths, provider identity and credentials are
excluded from ordinary tool results and history.

[ADR-134](../../adr/134-cloud-blob-transport-revision-precondition-and-offline-ownership.md)
requires a provider-authenticated cloud read to publish only complete, revision-
consistent, length/digest-checked opaque bytes. That is transport integrity, not save
trust: the complete download still begins local ADR-116 admission as untrusted bytes.
Partial/multipart staging never escapes, and the transport cannot choose trust policy,
interpret archive state or turn failed validation into an empty/not-found success.
Only the ADR-115 coordinator owns durable upload/delete intent and reconciliation;
generic Platform Services retry storage contains no second cloud mutation copy.

Development may admit isolated unsigned/modded namespaces and additional inspection
tools, but it never disables framing arithmetic, parser/decompression/work limits,
path containment, transactional publication or credential isolation. Shipping and
secure server namespaces reject policy downgrade. A valid signature is not anti-replay
state, encryption, semantic validity, anti-cheat or a native-code sandbox.

## Platform Progression Trust

[ADR-133](../../adr/133-platform-progression-authority-trust-and-idempotency.md)
separates a committed gameplay fact, Horo progression intent, frontend request and
remote provider projection. Local-client achievement/stat/score submissions are
tamperable and cannot authorize shared economy, competitive rewards, simulation or
access control. Idempotency keys, provider callbacks, SDK anti-tamper features and
signed local saves do not turn them into server evidence.

Definitions with integrity impact require `AuthorityServer`. Clients send ordinary
authenticated gameplay input/commands; the authoritative server gameplay owner derives
the fact and uses a server-only progression commit capability. Client, listen-server
client world, debug UI and provider login cannot mint that capability or downgrade the
cooked policy. Server credentials and privileged provider/gateway routes stay out of
client artifacts.

Repeated conflicting mutation IDs, stale authority/session generations and impossible
value transitions may emit bounded audit events. Mutation/diagnostic state stores typed
Horo IDs and normalized values/outcomes, not credentials, raw provider account IDs,
native tokens or personal display data. Idempotency deduplicates an already-authorized
intent; it never authenticates one.

## Platform Identity And Consent

[ADR-135](../../adr/135-platform-identity-session-generation-privacy-and-consent.md)
keeps provider account locators private and exposes only non-guessable, generation-
scoped live subject capabilities. Local storage/profile and gameplay identities remain
separate explicit mappings; display name, email, gamertag or provider login cannot
authorize/link them. Every user-scoped commit revalidates session generation and the
applicable access-policy revision so stale callbacks, caches and durable work cannot
cross sign-out/account switch.

Consent/restriction is the intersection of operation purpose/data classes, provider/
OS permission, product consent, region/legal and child/parental policy. Only a typed
Granted capability admits work. UI presents/submits a decision but cannot grant itself,
and development/debug paths cannot bypass or convert denial into empty success.
Personal/social presentation is bounded, untrusted and ephemeral by default. General
logs, metrics, crash bundles, saves/journals and ordinary tools exclude raw account/
handle values, friends graphs, display data, presence free text and credentials.

[ADR-159](../../adr/159-xr-action-tracking-and-input-projection-ownership.md)
applies the same intersection to articulated hand joints, eye gaze and their derived
gesture/behavior signals. Runtime capability discovery and OS permission do not replace
purpose-bound consent. An admitted consumer receives only its minimum typed derivation;
raw joints/gaze and continuous pose history remain excluded from ordinary logs, crash
bundles, metrics, replay, analytics, AI context and support capture. Revocation closes
collection, invalidates snapshot generations and neutralizes dependent Input actions.

[ADR-162](../../adr/162-mixed-reality-ownership-privacy-and-capability-tier.md)
classifies raw camera/depth as restricted sensor data; gaze/continuous pose as sensitive
biometric/behavioral data; room planes/meshes as sensitive environment geometry; and
anchor references/maps as sensitive location-derived persistent data. Derived hits and
light estimates retain spatial classification. Each consumer needs its own purpose and
least-privilege generation; provider discovery or OS permission is not consent. Ordinary
logs, crash bundles, metrics, replay, saves, AI context and support exports exclude these
data unless a separate visible bounded retention/export policy admits them.

[ADR-163](../../adr/163-xr-tooling-diagnostics-privacy-and-qualification-ownership.md)
requires XR inspectors and captures to use the same purpose/consent/access revisions as
shipping runtime. Development build type is not consent. Capture requests declare
channels, duration/frame/byte bounds, redaction, protection, retention and export; local
runtime overrides and detailed evidence never become portable project settings or public
support claims.

## Process Execution

Process policy validates:

- executable identity and resolved absolute path
- owning toolchain or plugin capability
- argument boundaries
- working directory
- environment allowlist
- timeout and cancellation
- output bounds

Shell concatenation is forbidden by default. Project content cannot inject
extra arguments through untyped strings.

## Plugin And Code Security

Native code can access process memory and therefore requires trust. Plugin
permissions improve authority control but are not a security sandbox.

Native activation is gated before platform library loading. The gate requires
an approved algorithm, exact artifact digest, trusted publisher/key identity,
and valid detached signature. Missing, corrupt, stale, unknown, or unverifiable
evidence prevents the platform loader and every module callback from running.
Large private artifacts may be verified by streaming their exact file length and
SHA-256 digest before applying the same trust-root and detached-signature check.
The caller must keep that private file quiescent until the verified bytes are
consumed; a verified path alone does not freeze its contents.

Untrusted extension execution requires process isolation with:

- a versioned IPC protocol
- restricted filesystem roots
- no inherited credentials
- bounded memory, CPU, and output where supported
- explicit network policy

## MCP Security

MCP defaults:

- loopback binding
- authenticated sessions
- per-tool capability declaration
- request size and rate limits
- no direct mutation from transport threads
- redacted audit records

Remote binding requires explicit configuration, secure transport, and a threat
review. MCP tools cannot bypass project trust, path, process, or credential
policy.

## Credentials

Credentials are:

- referenced by opaque ID
- resolved only for the authorized operation
- held in short-lived secure values
- excluded from configuration snapshots, event payloads, errors, logs, crash
  records, and diagnostic bundles
- wiped where the platform and memory contract permit

Credential prompts identify the requesting operation and capability.

## Network Access

Network clients declare destination policy, protocol, timeout, size limits, and
credential requirements. Plugins and projects require the `network.client`
capability.

Redirects, proxy behavior, certificate validation, and download integrity are
explicit for update and package operations.

## Real-Time Session Trust

[ADR-098](../../adr/098-protocol-session-and-trust-policy.md) defines the
real-time admission boundary. A transport `Connected` event, encrypted packet
channel, IP address, private subnet or provider identity claim does not authorize a
gameplay session. Before `Active`, only bounded control-channel admission messages
may reach `SessionAdmissionController`; all gameplay/RPC/replication input is
rejected and never queued for later dispatch.

The host resolves an immutable trust-policy snapshot before listener/connect
creation. Projects, peers and transports cannot install trust anchors, weaken
verification or widen bind scope. Exposure requirements are:

- loopback development proves both bind and peer are loopback and grants only an
  explicit local principal or configured credential; the profile cannot bind LAN
  or public interfaces;
- LAN requires encryption/integrity, authenticated server identity through a
  product trust anchor, exact pin or explicit pairing, and either a credential or
  an explicitly bounded guest policy;
- remote requires encryption/integrity, authenticated server identity and a
  short-lived replay-resistant application admission proof bound to the server,
  protocol transcript, nonces and expiry.

Provider/native identity and client claims are inputs to an injected credential
verifier, not gameplay roles. Successful verification produces a bounded immutable
Horo principal; credential expiry or revocation closes the M0 session. Credentials,
proofs, trust secrets and transcript secret material never enter logs, metrics,
errors, project data or diagnostic bundles.

[ADR-103](../../adr/103-network-project-configuration-and-build-profile-ownership.md)
requires portable network policy to name only a stable credential requirement.
Private user/CI/host input binds that requirement to a credential-provider
reference, which is resolved only inside the bounded consuming operation. Product
capability manifests, project files, caches, ordinary environment summaries and
safe provenance contain neither raw secret/private-key values nor machine-specific
bindings. Missing provider capability fails explicitly and cannot select a weaker
trust policy.

Admission enforces finite pre-active connections, bytes, messages, fields, attempts,
verification work, per-source rate and timeouts before expensive verification.
Malformed, incompatible, replayed, downgraded, expired or hostile input fails
terminally and never falls back to a weaker/guest profile.

## Diagnostic And Privacy Policy

[ADR-070](../../adr/070-capture-and-voice-io-ownership.md) treats microphone,
remote voice, derived features and transcripts as privacy-sensitive. Security and
host policy own capture purpose, consent provenance, retention/export/upload and
diagnostic inclusion; an OS microphone grant does not authorize recording,
networking, speech processing or telemetry. Raw content is excluded by default.

Security-relevant events include:

- trust grant, denial, and revocation
- plugin load and permission denial
- blocked path traversal
- rejected executable or process request
- MCP authentication and rate-limit failures
- update signature failure
- rejected save integrity/signature/scope/replay policy
- denied save inspection/import/export/load/delete/conflict capability

Records contain stable identities and safe reasons, not secrets or unnecessary
user content. Diagnostic bundle generation shows the user what categories will
be included.

## Security Updates

The application can revoke:

- plugin trust by identity/version
- project execution trust
- MCP tokens
- cached package/update trust
- credential references

Security-sensitive defaults may become stricter in a release without preserving
an unsafe previous default.

## Testing

Required tests cover:

- project trust capability gating
- path traversal and symlink escape
- archive extraction limits
- command argument injection resistance
- runtime console command permission, profile, allowlist, and remote-denial checks
- environment allowlisting
- plugin permission and trust checks
- MCP authentication, authorization, and rate limiting
- credential redaction across every signal
- loopback/LAN/remote bind and trust-policy enforcement
- protocol downgrade, schema/capability mismatch, wrong peer pin/root and replayed admission proof
- pre-active gameplay denial, bounded malformed admission input, timeout, revocation and shutdown races
- parser resource limits and malformed input
- update signature rejection
- local/imported/cloud/migrated/modded/server-signed save admission and quarantine
- save framing, decompression/work budgets, wrong-scope substitution and valid-old replay
- GUI/CLI/MCP save capability separation and raw-state/path/credential redaction

## Related Documents

- [Release Security](../release/release-security.md)
- [Extension System](../extensions/plugin-system.md)
- [Platform Abstraction](../foundation/platform-abstraction.md)
- [MCP Architecture](../interfaces/mcp-architecture.md)
- [Runtime Debug Console And Development Overlays](../runtime/debug-console-and-overlays.md)
- [Horo Package System](../packages/package-system.md): package trust levels and code contribution policy
- [Error And Diagnostics](../foundation/error-and-diagnostics.md)
- [Networking Architecture](../runtime/networking-architecture.md)
- [ADR-098: Protocol, Session and Trust Policy](../../adr/098-protocol-session-and-trust-policy.md)
- [ADR-103: Network Project Configuration and Build-Profile Ownership](../../adr/103-network-project-configuration-and-build-profile-ownership.md)
- [ADR-116: Save Data Threat Model and Trust Policy](../../adr/116-save-data-threat-model-and-trust-policy.md)
- [Save Game And Persistence](../runtime/save-game-and-persistence.md)
