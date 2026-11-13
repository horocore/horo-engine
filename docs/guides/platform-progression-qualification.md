# Platform progression delivery and qualification

## Scope and delivery identity

This work targets the published `origin/codex/editor-improvements` commit
`3df476ff430a2ad28cd62a8a0c1780ae0fea71bc` (verified on 2026-11-13).
The empty archived worktree was recovered from its own Git snapshot
`6172fdf379799c90be23b6913354ff0a9e2927ef`; all 17 task paths were verified,
then preserved while rebasing onto the published base. Recovery evidence is in the
ignored `build/recovery-rebase-4fbcd3cda64644e482cb7a203972ce7d/checkpoint.json`.
Unpublished changes from the main checkout were not used as the integration base.

All-child acceptance audit (GitHub/Jira read directly on 2026-11-11):

| Child | GitHub / Jira state | Existing implementation/acceptance evidence | Audit limit |
| --- | --- | --- | --- |
| #1889 / HORO-1845 / PLS-004.1 | Closed / Done | ADR-133 names fact, product, server, semantic coordinator, frontend, queue and provider authorities; fixes mutation algebra and trust/retry boundaries. Integrated architecture commit `7ce22fbee` is an ancestor. | ADR remains Proposed. No change to that status or inference that implementations are fully qualified. |
| #1899 / HORO-1855 / PLS-004.2 | Closed / To Do | Achievement coordinator `be483dacd` is an ancestor; existing tests cover registered IDs, authority/progress bounds, terminal duplicate retention, state-query fencing and session replacement. | Existing code/tests inspected, not rerun in this worktree. Actual async provider/application integration is not proven merely by these semantic coordinator tests. |
| #1902 / HORO-1858 / PLS-004.3 | Closed / To Do | Bounded leaderboard implementation `b8a2d5acf` is an ancestor; typed ranked/around-subject/friends requests and explicit unsupported kinds exist in frontend/mock coverage. | Current native execution and real-provider ordering/tie/pagination qualification remain unproven here. |
| #1904 / HORO-1860 / PLS-004.4 | Closed / To Do | Stat coordinator `caa3afe94` is an ancestor; cache/session partition, freshness, corrupt restore, authoritative writes and completion fences have existing regression coverage. | Current native execution and actual protected persistence/provider composition are not established by closed issue state. |
| #1901 / HORO-1857 / PLS-004.5 | Open / To Do | New volatile coalescer, exact-envelope retention and safety/ordering/lifecycle regressions are written. | Uncommitted; native regression, final preflight and durable-owner integration evidence remain outstanding. |
| #1903 / HORO-1859 / PLS-004.6 | Open / To Do | Shared typed/key application entry, real frontend gate, scoped receipt observations and explicit durable qualification handoff are written. | Uncommitted; native consumers, actual world-authority binding and queue-owner integration remain outstanding. |
| #1906 / HORO-1862 / PLS-004.7 | Open / To Do | Null/scripted, ambiguity/timeout, concurrent/reordered and normalized-failure fixtures plus evidence inventory are written. | Unexecuted here; provider/session/credential dependencies and SDK/certification evidence remain open. |

Main #1870 / HORO-1826 remains Open / To Do. Its three integrated outcomes are still
not claimed complete: reject invalid/unregistered intents before any provider submit,
prevent duplicate external effects across retry/replay, and equivalent results across
all enabled adapters. The persistent Codex goal covers all seven children, not only
the three open issue records. Runtime status is separate from acceptance: blocked
does not mean complete.

| Ticket | Implementation | Regression evidence | Outstanding acceptance evidence |
| --- | --- | --- | --- |
| HORO-1857 / #1901 / PLS-004.5 | Bounded volatile `PlatformProgressionCoalescer`; exact duplicate joining, fixed windows, extrema, individual retained outcomes, immutable dispatch, unknown-effect fence | `PlatformProgressionCoalescerTests.cpp`: permutations, concurrent duplicate/completion, cancellation, capacity, ordering, scope, ambiguity | Execute final tests and preflight; real projection integration must pass original envelopes without weakening provider semantics |
| HORO-1859 / #1903 / PLS-004.6 | `HoroEngine::ProgressionApplication` common typed/key admission; registry-derived algebra and authority; borrowed trusted authority and exact provider qualification ports | `ProgressionUseCasesTests.cpp`: common receipt, unknown/out-of-range rejection, Null gate, authority revocation, cancellation | Execute final tests and consumer; actual hosts must inject current world/session authority and qualified projection gate |
| HORO-1862 / #1906 / PLS-004.7 | Public Null/scripted regression slices and evidence inventory | Existing frontend/mock/session/coordinator tests plus new aggregation/application cases | Full production qualification remains open: provider conformance #1887/HORO-1843, session lifecycle #1916/HORO-1872, credential references #1826/HORO-1782, private SDK atomic/dedupe/certification evidence |

No GitHub or Jira issue status is changed by this document. The four closed GitHub
children are existing implementation, not a reason to infer that their Jira status
is Done. ADR-133 is marked **Proposed**; the normative Platform Services architecture
references its algebra. This change follows that architecture without changing ADR status.

## Header ownership and migration

`PlatformProgressionCoalescer.h` belongs solely to `HoroPlatformServices`.
`PlatformProgressionReplay.h` also belongs solely to `HoroPlatformServices`.
`ProgressionUseCases.h` belongs solely to the new narrow `HoroProgressionApplication`
target, whose public dependency is Platform Services. `HoroProgressionPublicConsumer`
compiles through that target's staged headers. Existing headers and call signatures
are unchanged. There is no repository-root public include requirement.

The application-owned `FrontendProgressionProjectionGate` composes the real frontend's
availability, policy and lifecycle checks with a separately qualified semantics gate.
The real frontend remains authoritative for explicit Null and shutdown rejection;
an available generic service never proves atomic progression support. The Null/scripted
profile fixture exercises this adapter without invoking legacy mutation entry points.

Compose one volatile coalescer for a bounded application lifetime, retain immutable
same-project definition snapshots, and inject authority and projection gate instances
that outlive the use cases. Gameplay submits typed facts; text adapters resolve keys
through the same entry. Callers cannot supply authority or select mutation algebra.
The trusted authority port must validate the actual committed fact and current
world/session/access evidence, including revocation. A client world must never receive
the server implementation. A permissive test port is not a production authority proof.

Only the composition owner receives dispatch/completion access. Admission, dispatch
and completion are mutex-serialized; no provider callback runs under this mutex.
Provider completion may occur out of order across targets. Same-target work is fenced
behind an in-flight or unknown effect. A fixed window starts with the first pending
receipt; new arrivals cannot extend it. Only original, unmodified strongest envelopes
are dispatched, so retry/reconciliation retains the original ID and payload. Conditional
replacement and additive mutations always dispatch separately.

The projection gate must reject explicit Null and unsupported atomic/dedupe primitives
before receipt allocation and must recheck current generation at dispatch. The legacy
frontend's unlock/stat/score requests do not carry the full progression envelope;
do not translate these new operations to those calls and claim retry safety. Real
provider integration needs an envelope-preserving qualified path. It is not supplied
by a successful scripted test.

Application callers query the same retained logical receipt through `ProgressionUseCases::Query`;
outcome and normalized failure category are captured atomically. Query and cancellation
are restricted to the use case's captured subject/session/access scope. They do not
grant access to another scope merely because an old mutation ID is known.

The application also retains a finite semantic binding for each accepted
`(captured scope, occurrence, definition kind/ID)`. The existing mutation-ID derivation
includes payload fields; changing it would invalidate stored replay identities, so it
is deliberately unchanged. Instead the semantic binding rejects a changed payload,
precondition, delivery owner or durable provenance for the same committed occurrence.
An exact retry resolves to the original envelope. One gameplay occurrence may still
project to distinct definitions. This small in-memory identity ledger neither
coalesces work nor journals/schedules durable delivery.

The delivery owner is fixed before the first attempt, including failed qualification
or admission. A valid durable preparation failure cannot be retried as volatile
success. This binding is not durable acceptance; only the queue owner can promise
that after its write commit. Fact retention is bounded and never silently evicted.
`ProgressionUseCases` is move-only; consume its successful `Result` once and give
adapters borrowed references to that shared instance. Moving transfers fact bindings
and closes the old view, preventing a copied empty ledger from becoming a second
semantic authority. Constructor allocation failure returns typed CapacityExceeded.

Cancel affects undispatched intent only. An in-flight cancellation/timeout must be
classified by the request/semantic owner; it does not prove remote rollback. Unknown
effects retain their exact dispatch token for conclusive reconciliation, and no
automatic resubmission is authorized here. Close cancels pending work, preserves
terminal receipts and marks in-flight work unknown. Receipt capacity includes terminal
dedupe evidence; exhaustion fails rather than evicting it. There is no durable row,
storage, replay scheduler or second PLS-007 owner.

## Qualification boundaries

### PLS-007 typed handoff and remaining integration

`ProgressionUseCases::Submit` is volatile-only and does not expose a durable
acceptance promise. `PrepareDurableIntent` now shares its exact definition, fact,
authority and gate validation, then invokes an explicitly supplied trusted qualifier.
It returns only a qualified handoff and never calls the volatile coalescer. It does
not admit or persist durable work. No delivery implementation or commit SHA is
available yet; preparation success must not be reported as durable acceptance.

The actual new contract names are:

- `PlatformProgressionReplayRequirements`: exact provider/generation/policy and
  provider-qualification, effective-semantics, project-policy, stable-ID registry,
  owning definition-registry and cooked mapping-generation fingerprints.
- `PlatformProgressionQualifiedReplayIntent`: the single immutable original envelope,
  captured requirements, authored ordering and `PlatformProgressionReplaySafety`.
  Intrinsic idempotence, atomic comparison, conditional revision and durable mutation-ID
  deduplication are distinct classes. The protected sealing helper checks structure
  and algebra only; it does not establish provider conformance or authenticate an issuer.
- `IPlatformProgressionReplayQualifier::Qualify` and `Revalidate`: the trusted actual
  qualification owner's admission and per-dispatch validation port. There is no
  default issuer and no upgrade from a void projection gate. PLS-007 must revalidate
  using its own injected trusted issuer; a C++ type alone is not an authorization proof.

The application checks its actual stable-ID/definition fingerprints, captured policy
and provider generation before qualification, then rejects a returned envelope,
requirements tuple or ordering different from its prepared original. Recovery must
pass the decoded original through the real qualifier again; it cannot manufacture a
sealed value, allocate another mutation ID or replace old generations. Protected
queue partition and current PLS-006 binding evidence remain outside this progression
handoff and must come from their existing owners.

For durable delivery, pass each original `PlatformProgressionMutationEnvelope`
unchanged to the PLS-007 owner, together with definition ordering and qualified replay
evidence. This includes the complete mutation ID, typed value/signedness, exact
precondition, definition kind/ID, subject digest, provider/session/access scope and
authority/policy revision. The old simplified offline lane/operation model is not
equivalent evidence. In particular, never truncate the 32-byte progression subject
digest to obtain a 16-byte storage partition; protected partition binding belongs
to its host/session owner.

Qualified evidence must bind the selected provider qualification, exact operation
algebra, numeric domain, ordering, scope and policy. Intrinsic unlock, atomic
max/min/best, conditional revision and durable AddStatOnce dedupe are distinct replay
classes. A generic Available capability or caller-supplied retryable boolean is
insufficient. The existing projection-gate validation still returns no replay
attestation; the new explicit qualifier port must obtain it from the real owner.

Verified existing provenance sources are `PlatformProjectConfiguration::Fingerprint()`,
`PlatformStableIdRegistry::Fingerprint()`, the owning achievement/stat/leaderboard
definition registry's `Fingerprint()`, and
`PlatformProviderManifestCookOutput::generationFingerprint`. The existing
`PlatformServiceCapabilitySnapshot` has no progression atomic/dedupe qualification
record or effective-semantics fingerprint. Neither this application gate nor PLS-007
should introduce a competing qualification authority or invent such a fingerprint
from generic availability. The actual provider qualification owner must publish that
effective semantics and populate the new qualification port before a production
durable evidence bridge can be reported complete.

Select the owner before first dispatch: volatile work enters this coalescer; durable
work enters PLS-007 directly. Only the selected owner retains and coalesces its logical
receipts. Durable scheduling performs current semantic/session qualification before
provider dispatch, preserves the original envelope on each attempt, and commits the
durable outcome before publishing receipt success. Remote ambiguity remains explicit;
a failed local outcome commit cannot become remote success or a replacement mutation.
The semantic owner may interpret late evidence without rewriting the original ADR-130
request. Same-binding reauthorization must use the actual PLS-006 immutable evidence,
not a locally invented account/profile authority.

The queue admission/dispatch/outcome connection still belongs to PLS-007. This work
adds no second journal or durable scheduler and does not claim the missing producer,
storage-owner connection or provider integration as completed qualification.

### Public and private evidence

Run the public frontend/mock suite as well as the new tests for offline, signed-out,
rate-limit, delayed/reordered completion, timeout and shutdown. Existing tests establish
their own request-level invariants; they do not establish private SDK atomicity,
end-to-end reconnect or production world authority. The new tests retain safe typed
IDs/scopes only; there are no credentials, raw account IDs or provider strings.

The new cross-contract fixture creates a real `PlatformRequestStore` timeout and
then supplies late conclusive progression evidence. It checks that every logical
receipt reconciles while the original request stays TimedOut. Separate cases retain
Offline, NotSignedIn, RateLimited and PermanentFailure on every coalesced receipt;
joining after failure does not create a new dispatch or erase the category.

Private providers must record SDK/gateway versions, capabilities and product profiles,
prove atomic max/min/best and durable AddStatOnce dedupe, and exercise reconnect,
account/session replacement and ambiguous commits through the actual composition.
Never call a missing SDK/device/certification run passed. Do not replace another
workstream's provider, session or credential implementation to manufacture that evidence.

Validation commands, final report paths and incomplete checks are recorded after
execution. No test result is implied by the implementation inventory above.

Current validation checkpoint:

- `clang-format --dry-run --Werror` on the changed C++ inputs and `git diff --check`
  completed without findings.
- Initial headless configure reached the public-header inventory, then rejected the
  undeclared `HoroProgressionApplication` dependency policy. The policy entry was
  added; the corrected configure/build has not yet run.
- The initial doctor lacked CMake/Ninja on PATH and selected the absent default
  VS Code profile. With the verified installed tools and existing portable VS Code
  profile, `scripts/analyze.py doctor --offline --code-user-data <portable-profile>`
  returned `ready_for_local_check`: Sonar CLI 1.10.0, Codacy Analysis CLI 0.25.0,
  VS Code 1.145.3, CMake 4.3.1 and Ninja 1.13.2. Report basename:
  `1794383983919163600-5c374e1c.json`. This is prerequisite evidence, not IDE
  authentication proof, a quality result or a C++ sensor run.
- The separate online doctor's cloud configuration read exited 2 with a request
  to verify Codacy authentication. Cloud drift remains incomplete; local configured
  checks can still run. Report basename: `1794383841855019400-4f560ef6.json`.
- On 2026-11-13, the public `heavy_build_lock(blocking=False)` admission and
  exception-release contract passed its two targeted Python regressions
  (`2 passed, 25 deselected`). The complete cache-tooling module then returned
  `25 passed, 2 skipped`; the two native ccache regressions require unavailable
  ccache/compiler prerequisites. The default remains blocking for existing callers.
- The first resumed configure attempt returned busy on atomic acquisition of the
  canonical physical lock. No native child or waiting helper started. Evidence:
  `build/pls-configure-1794573182927326000.json`. The prepared runner verifies the
  real Windows handle, CPU/RAM/disk admission, one CPU, a 4 GiB native job limit,
  serial builds/CTest and a bounded timeout. Each batch releases the lock.
- A separate final-scope quality admission attempt also returned physical-lock busy
  before launching `analyze.py`; evidence:
  `build/pls-analyze-1794573249471055600.json`. This is an admission failure, not
  a completed full preflight or a scanner report.
- Sonar IDE, CLI secrets and Codacy final-input results remain **incomplete / not
  executed**. No private-provider or production qualification acceptance is claimed.

The 2026-11-13 coordination protocol supersedes the historical main-only grant
requirement. A named grant is no longer needed; actual nonblocking physical lock
ownership and resource admission remain mandatory. Native configure/build, all 27
`[pls004-contract]` cases, broader CTest and the final actual-base preflight remain
pending. The earlier persistent goal was marked blocked under the old hold; the
user explicitly resumed work, although the exposed goal tool has no resume setter.
Provider qualification, queue-owner connection and private SDK/OS evidence also
remain outstanding. Source preparation and closed issue state do not prove them.
