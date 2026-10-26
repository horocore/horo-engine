# Canonical host replication world binding

HORO-1162 / #1162 / [NET-007.2] replaces two competing observations: the former
mode role assembled from a Scene/peer tuple, and the headless process service's
unused Empty replication lifecycle. The NetworkRuntime-owned world controller
now owns the actual replication lifecycle. Applications still select local Scene
candidates, factories, trust and concrete transport before activation.

This source cut requires fresh native and owning consumer qualification. Earlier
PR3536 checkout validation is not evidence for these changed C++ contracts.

## Invocation and migration

Standalone remains independent of a network peer, backend or binding policy.
Client, listen and dedicated **gameplay** composition requires an explicit
`NetworkWorldBindingSetup` captured by `NetworkProductAdmission` from the actual
host's required-feature declaration, canonical codec snapshot, exact server
principal/provenance and authority-publication capability. Missing policy fails
with a typed policy error before listeners. No credential, principal or trust
root is inferred from project data or command-line arguments.

Transport-only qualification is an explicit `NetworkWorldCompositionPurpose`.
The CLI selects it with `--transport-qualification`; it cannot also request a
gameplay module. A successful transport qualification does not publish a gameplay
world role. Existing transport-only fixture and CLI callers must select that
purpose deliberately. Actual secure embedding supplies the complete captured
binding/security owners; there is no default account, issuer or key loader.

`NetworkModeWorld` now contains only a host-owned local Scene candidate and kind.
Callers no longer supply a synthetic authority epoch. A checked, owner-thread
NetworkRuntime issuer produces distinct typed world-session and epoch identities.
Neither is cast from host generation, peer operation generation, Scene identity,
a packet or frame count. Failed candidates burn their issued occurrence; counters
never wrap during the owner's lifetime. Authenticated channel generations fence
prior owners and processes.

`NetworkModeRoleView::session` remains the local canonical peer operation
generation. `worldSession` identifies the published replication occurrence.
Server authority comes only from that same published lifecycle. A client view
has no local authority epoch; the descriptor's remote epoch identifies its remote
object namespace and never grants publication or mutation rights. Actual work
must acquire the role-derived phase/tick lease from that same lifecycle.

## Control and native lanes

The product declares one mandatory, versioned world-binding control identity and
required protocol feature in its existing registry. There are no globally
reserved IDs. Required reliable Control and Command use distinct configured
native GNS lanes (and Snapshot has another lane when selected). The selected
factory and exact initialization configuration declare the same lane count;
configuration failure cannot publish Accepted/Connected. The dispatcher observes
native `m_idxLane`; envelope fields cannot fabricate routing or protection.

The active world controller owns each peer's Control sequence and fixed outbox.
Pre-Active admission has its existing separate closed negotiation sequence.
Other active Control producers must share this sequence owner rather than
starting an independent counter on the same lane. Reliable order is guaranteed
within a lane, with no cross-lane receive-order promise.

The fixed 60-byte v1 payload contains version/kind, world occurrence, remote epoch,
negotiated schema fingerprint and expected previous occurrence digest. It contains
no local Scene/path, local transport handle, peer generation or credential/proof.
The domain-separated digest names only occurrence/schema; Initial, PREPARE and
COMMIT phases do not change that identity. It is correlation, not authentication.

The real admitted handler additionally checks the exact retained canonical peer,
local operation generation, current owner clock, trusted Client endpoint,
negotiated required feature/schema/protocol, authenticated server principal,
credential provenance and authority-publication capability. A packet cannot
bootstrap its own peer, select a local Scene or create an object authority grant.
Cross-connection/candidate replay cannot substitute for any of these checks.

## Publication, travel and lifetime

Initialization stages the server occurrence; only a real
`CommitDeferredLifecycleChanges` phase commits it. A client remains explicitly
unready until the authenticated Initial binding is staged against its host's
selected local Scene and committed at that same canonical phase. Privileged replication work
waits for canonical publication; Active/Role/Acquire remain explicitly unready.
Local Scene/presentation callbacks may continue, but cannot infer network
authority from their invocation. Session revocation precedes those callbacks. The actual ProductHost/Session/Transport factories use this same owner.

Travel first prepares all local Scene/Physics/service candidates and detached
replication lifecycles while retaining the old published aggregate. PREPARE is
identity-only. A remote client publishes only matching COMMIT for its one pending
candidate, exact prior identity, peer/principal/schema and host-selected local
Scene generation. Duplicate PREPARE never extends its finite deadline. Cancel,
expiry, disconnect and shutdown discard pending state and preserve the first
canonical terminal reason. No authority transfer or distributed atomicity is
promised.

A same-host listen pair publishes both prepared worlds in one nonfailing local
safe-point swap. Before that swap the controller owns every required bounded
COMMIT envelope/outbox reservation. Afterwards reliable backpressure retains the
same exact bytes/sequence until accepted or its finite deadline; a failed peer is
retired truthfully. Local committed state is never silently rolled back on only
one side to hide a send failure.

The canonical world allocation is stable across permitted composition moves.
Moved-from destruction cannot revoke the moved-to owner. Session/transport
participants borrow weakly; admitted handlers and queued control work revoke
before world-state/Scene/native destruction. Shutdown invalidates outstanding
world leases before dependent services retire, including partial startup and
queued work. No global registry, extra thread or mutex supplies lifetime safety.

The installed `NetworkWorldBinding.h` owns only inert wire identities, policy and capture values under
`HoroNetworkRuntime`. The full controller and `NetworkModeHostAccess` are non-installed headers staged
by `HoroNetworkModeHostInternal`. Only actual ProductHost composition and owning regressions link that
interface privately. Installed Mode consumers use `ServicesStarted`, `Active`, `Role` and
`AcquireReplication`; they cannot reach controller transport attachment or peer publication APIs.
Service startup permits negotiation while world publication is still pending. Prepared admission is
inert; server peer binding starts only after the canonical Active callback. Non-Control ingress then
requires the exact canonical published local world in addition to existing peer and delivery checks.

Standalone deliberately has no network binding controller, world-session identity, or authority epoch.
Its local role remains host/Scene scoped and `AcquireReplication` is unavailable. Local callbacks may
continue with an unavailable network role after revocation; this never admits replication or Command
handlers. Transport qualification is an explicitly selected purpose and publishes no gameplay role.

Queued generic Control records are independent of travel reservations. Cancellation releases only the
matching unsent PREPARE; failed local publication releases only COMMIT records reserved by that attempt.
An accepted remote PREPARE has its own finite fence, which unrelated Control traffic cannot renew.
