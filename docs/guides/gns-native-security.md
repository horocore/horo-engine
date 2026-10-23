# Explicit native GNS product security

This optional private composition advances HORO-1089/#1089 [NET-002]. It does not
add an account/lobby service, stock credential loader, default trust anchor or an
authentication fallback. The existing raw `CreateGnsTransport()` remains available
for its explicit raw transport contract.

`CreateSecureGnsTransport` accepts an owned `GnsNativeSecurityOptions`: public root,
app ID, exact canonical trust snapshot, host-selected peer trust, exact transport
limits and retained bounded startup issuer. It returns one unique transport and
four views of the same revocable native authority. Credentials, proof issuance and
entropy remain separate retained host capabilities. Available delivery metadata
describes the selected candidate; authorities remain unavailable until native
certificate provisioning actually succeeds.

The real `RunNetworkProduct` embedding entry accepts the explicit native selection
alongside its immutable admission capture. Native certificate/peer/key/channel
slots in that capture must be empty; the factory fills them explicitly and creates
one final owned capture. Conflicting providers or trust/limit selections fail
before initialization. Existing embedding callers omit the new optional argument;
standalone rejects a native security request. No capability absence chooses trust.

Startup retains the existing single process adapter. Native initialization and
selected-root install precede a public CSR request. The issuer runs with neither
the native lock nor Horo's process/transport locks held, then returns bounded
`SecureBytes` certificate by the explicit deadline. Native key match, CA, identity,
app and expiry validation must succeed before publication. Private-key import is
rejected; native-generated signing, DH and traffic keys never leave GNS.

The backend reads evidence only for the current owner-local transport handle and
one exact admission generation. Native root generation, allocation incarnation,
verified certificate digest, certified identity and live AES-GCM contexts are
revalidated on inspection/binding. Local handles are not wire authentication
material. Public correlation hashing cannot fabricate native protection or grant
Active; the existing canonical controller/authentication adapter performs those
transitions.

Shutdown revokes the provider borrow before taking the transport lock, preserving
lease→transport lock ordering. It then closes native resources, kills/drains the
backend and retires the selected root. A failure retains cleanup responsibility in
the existing process owner even after the adapter is destroyed; a replacement
cannot initialize until that token retires. No error allocation occurs in native
cleanup barriers. Original issuer failures stay typed; safe native status classes
remain distinct from authentication rejection and stale generation.

The pin and BSD license are retained in `cmake/patches/gns/manifest.json`. The
build-owned patch helper verifies pristine contexts and patched hashes. In-repo
and external build paths cannot make Git apply into a parent checkout. Only the
three changed native translation units override upstream exception disabling;
the actual upstream C++17 requirement and unrelated translation units stay intact.
The unpublished native initialization candidate uses the existing failed-BInit
`Destroy` path under the native global lock. It records low-level reference
ownership before an allocating instance-list insertion and publishes the global
owner only after successful initialization. This relies on the existing concrete
partial-initialization cleanup contract; it does not make arbitrary live native
`Destroy` operations nonthrowing.


The source760043 checkpoint qualified a secure real two-ProductHost run on
macOS arm64/POSIX. Both child processes owned actual native GNS and Physics,
used an explicit ephemeral test CA/issuer and distinct canonical authentication
paths, handled an admitted registered gameplay message, then shut down and
proved native-owner reuse. Its56 unique cases passed with no failures or skips.
The manifest records the exact source freeze and executed JUnit hashes. These
binaries embed revisionb8679; source-preserving reconciliation to8e5f does not
relabel their build identity. New edits require their own qualification.
Raw GNS tests or injected transport evidence alone do not substitute for this proof.

The private paired-child qualification target uses the pinned native Ed25519 PKI
implementation for a fresh test CA and the existing HMAC primitive for a separate
explicit test credential. Connection signing keys remain native-owned. The parent
claims a private directory before any secret creation; actual owner/mode checks,
exclusive files, descriptor checks and zeroed scratch protect fixture material.
Only paths and fixed public phase/error identifiers cross the child-process
interface. Each child runs a real ProductHost, canonical admission and native
transport; an observing forwarder retains only the actual local Connect handle.
The receiving handler requires the runtime's own active lifecycle before replying.
A successful process token follows actual shutdown and native-owner reuse.

This fixture currently requires native Physics and provable POSIX owner-only file
semantics. Windows DACL qualification is separate; unsupported file protections
fail closed. The positive parent CTest actually ran at the checkpoints recorded
here. Windows runtime qualification remains incomplete. Fixed loopback port 48657 is an explicit qualification
resource; an occupied port causes a reported startup failure, never a backend or
trust fallback.

### Directional readiness migration

All GNS profiles now follow one backend-neutral readiness contract: native
`AcceptConnection` success only reserves the existing bounded pending slot.
After the single transition to connected, an inbound handle publishes `Accepted`
with its original listener generation; an outbound handle publishes `Connected`.
Both events permit immediate transport `Send`, including in the unlocked owner
callback. Each handle generation publishes exactly one directional ready event.
Readiness still grants no principal, active lifecycle or gameplay dispatch.

Previously GNS published inbound `Accepted` before `Send` was available, then a
second `Connected` event for the same inbound handle. The canonical admission
controller correctly treated `Accepted` as server readiness and attempted its
bounded offer, which the pending backend rejected. The later duplicate also
looked like an outbound client. Callers waiting for two generic `Connected`
events for one loopback pair must instead wait for one `Accepted` plus one
`Connected`. The existing GNS loopback, DNS and IPv6 callers migrate together;
the raw readiness regression sends from inside `Accepted` and asserts results
outside the noexcept callback. The real paired ProductHost regression retains
both native and canonical proof authorities. No security-profile timing exception,
second registry or authentication shortcut is introduced.

### Native adversarial qualification scope

The native fragmentation case sets only a private test-scoped GNS
`MTU_PacketSize` of576 before either real UDP loopback connection exists. It
reads back both that value and the smaller derived `MTU_DataSize`, then sends one
admitted1200-byte reliable Horo message and checks one exact reassembled payload
without a duplicate. Unlike the shared nine-byte Null fragmentation workload,
this message exceeds the verified native no-fragment bound. An RAII guard restores
the prior global value before the single native adapter retires, including test
assertion unwinding; the public1200-byte cap and production API do not change.

The wrong-root parent gives the two real children distinct ephemeral native CA
roots/issuers, while both credential/proof providers read the same owner-only
credential file. Public roots are checked distinct before launch. Neither child
may observe native directional readiness, an active transport or a gameplay
handler; the client must receive the canonical typed native connection failure.
Both child owners must shut down and allow native-owner reuse before the parent
accepts their fixed rejection tokens. Zero sent/received statistics mean no
application messages were admitted; native PKI handshake UDP traffic is required
and is not counted by those fields. This prevents a mismatched Horo proof
secret from concealing native CA rejection.

### Unready outbound attempt retirement

A GNS peer close while the exact connection is still pending is a failed
connection attempt: it emits one `Failed` with `TransportConnectionFailed`.
An established connection still emits normal `Closed`. This distinction does
not interpret vendor text as authentication evidence or expose native end reasons.
Old native callbacks cannot name a replacement transport generation.

The private product service hands the actual successful `Connect` handle to its
existing client owner before the first `PollEvents`; `Connect` queues callbacks
rather than invoking an event consumer. This pending record creates no peer,
principal or session generation. A matching terminal retains its original stable
descriptor once; unrelated inbound/server events cannot consume it. Successful
client preparation transfers ownership to the canonical lifecycle. Otherwise
`Reconcile` propagates the original typed attempt error through the existing
`FailPhase` path before Scene/Physics/travel callbacks, retiring worlds and native
borrows. Shutdown/detach preserve the first error, and the private terminal borrow
survives Shutdown until owner destruction. A retired host requires fresh owners;
there is no implicit reconnect or authentication fallback.

### Follow-up qualification provenance

The source checkpoint `acaf189a` on main `690bde79` built the eight native/product/
consumer targets with that exact source revision. Its fresh raw GNS and ModeHost
JUnit executed 62 unique cases: 62 passed, zero failed, zero skipped. This includes
both actual child peers handling admitted messages, the verified MTU576/1200-byte
reassembly case, distinct-native-root rejection with shared Horo proof authority,
and pending-attempt failure/rollback before postpoll world or travel work.
The JUnit SHA256 is
`42e6ab27464af3684f1f934057a7712e271f106b6733c6141a5fa263351d4db0`.
A disjoint pass built the API OBJECT consumer and Disabled/Null/composition targets,
then executed eight composition cases and two linkage contracts, yielding 72
unique passed native cases overall. An OBJECT consumer is compile-only evidence.

The manifest's source760043 product checkpoint remains historical; its remaining
work describes that earlier checkpoint. Subsequent implementation or quality edits
require their own current-input evidence and do not inherit a new binary identity
from this paragraph. Windows secure-file and runtime qualification, hosted quality
gates and the external HORO-1177 workflow dependency remain distinct limitations.

### Private composition and provisioning migration

`SelectNetworkProductGns` is the one non-installed, GNS-conditional product selection operation used by both the executable composition root and native paired-product qualification. It validates the explicit trust/configuration and absence of competing native providers, creates the inert concrete candidate, then captures its exact delivery declaration and retained authority owners through the canonical `NetworkProductAdmission` builder. Protocol, trust, limits, handlers and clock ownership remain explicit host inputs. The paired child applies only passive observation after this production selection; it no longer reproduces provider binding in test code. GNS-disabled products neither compile this source nor receive its private native dependency.

The private native lifetime value groups sockets, selected-root generation and resolver initialization under the existing transport mutex and owner-thread I/O rule. Shutdown and partial initialization retire them together; an unsuccessful native retirement still belongs to the process cleanup owner. This is a private field/operation migration, with no public native type or second authority.

Provisioning now uses lexical lock scopes: prepare the bounded native request under the transport lock, invoke the issuer outside both Horo and native locks, then reacquire and check shutdown before examining an issuer failure or publishing initialization. Reentrant shutdown keeps priority over a later issuer result. Native exceptions remain contained by the genuine noexcept invocation barrier; error construction stays outside that barrier. An exception from an individual native close is handled by the existing whole-native Kill fallback, whose failed retirement is retained for retry.

The private binding operation accepts the existing fixed192-byte context. Bounded `std::array`/`std::bit_cast` conversions preserve every root/context/digest byte when crossing the native POD interface; they introduce no exporter, derived key, secret retention or public API. Native trust, certificate proof and exact protected-channel generation checks are unchanged.

New issuer/selection/detached-authority regressions require fresh native and targeted coverage qualification for each delivery freeze. Historical73-case and hosted9319-case receipts retain their original source/head identities; they do not qualify this later batch.
