# MCP authentication and approval migration

HORO-244 / #244 / [MCP-001.5] deliberately removes name-only authority. Local
streams and embedded callers use the same policy; localhost and in-process access
are not authentication or trust grants.

## Host composition

Construct `McpAuthorization` with the host's OS secure entropy source. Assign a
nonzero, monotonically increasing project trust revision using `SetTrust`. Share
that exact policy with `McpToolRegistry`, `McpController::Create` and `McpSessionManager::Create`.
Missing entropy cannot issue a credential; missing policy cannot create either
owner. No platform backend, listener or credential store is selected internally.

Issue a 32-byte one-use credential for the exact approved client, capability set,
project, authorization revision and registry revision. Deliver the move-only
`SecureBytes` through a private, explicitly approved local channel. Never place
these bytes in JSON-RPC arguments, project data, configuration, diagnostics or UI
history. Authenticate out of band before `Open`, and attach the emitted immutable
principal to `McpSessionAdmission`. The policy stores SHA-256 credential digests,
not credential bytes. A known credential attempt consumes it even if its claimed
scope is wrong. Secrets and scratch entropy are zeroized on release.

## Approval and lifecycle

Every non-query descriptor requires local approval. The host creates a challenge
for the visible exact `tools/call` envelope and authenticated context, then submits
its explicit decision through `Decide`. Approval binds the decoded request ID,
tool and argument bytes through a canonical SHA-256 fingerprint, plus the principal
and its immutable project/authorization/registry scope. It expires independently
and is consumed exactly once immediately before registry adapter invocation.
Admission does not consume it. Edited arguments, a different request ID, another
principal, denial, expiry or revocation cannot reuse it. Direct registry invocation
also checks the same policy; an embedded client has no privileged path.
Every snapshot retains its registry's immutable host policy identity and rejects
caller-supplied foreign policy evidence before adapter invocation, even if the
foreign principal has identically named capabilities and a valid approval.

Approval consumption is the execution-admission linearization point under the
policy mutex, not a lock held while arbitrary application code runs. Revocation
ordered before that point denies admission; revocation ordered after it signals
the principal's cancellation chain. An admitted application invocation must
observe that authority before committing deferred work. This does not promise
rollback of an already committed transaction. Direct synchronous and asynchronous
snapshot completions recheck live authority before releasing results, and async
progress exposes numeric progress without retaining application free text.

Trust replacement revokes existing project principals and their derived
cancellation tokens, invitations and challenges. A project switch cancels old work
and clears session authority: the advanced handle only supports lifecycle cleanup.
Disconnect and authenticate a fresh session for the new project. Existing callers
must migrate; there is no name-only or trusted-boolean compatibility fallback.

Application owners still enforce filesystem roots, argument arrays, domain
authority, transactions and commit-time checks. Approval is not a path sandbox and
does not make native application code safe. Application owners must observe the
context cancellation and authority lifetime before committing deferred work.
The cancellation token observes explicit revocation and shutdown, not the passage
of time: there is no expiry timer or forced termination of application threads.
Session and snapshot invocation deadlines are bounded by immutable credential
expiry. Retained jobs must observe `McpRequestContext::IsStopRequested()` at
bounded cooperative checkpoints and before application commits, rather than
treating a bare cancellation token as continuing authorization. Expired authority
also rejects late completion and progress independently of job cooperation.

Controller history contains no argument payload. Queued arguments are released on
terminalization; running arguments are released after the adapter start returns.
Adapters must copy values they retain asynchronously. Free-text progress phases
are no longer retained or exposed because they may contain private input; numeric
progress and typed operation states remain available. Application output remains
subject to the registered schema and the owning application's disclosure policy.

## Qualification

Build `HoroMcpSessionTests`, `HoroMcpRegistryTests`, `HoroMcpControllerTests` and
the existing generated `HoroMcpSessionPublicHeaderConsumer`,
`HoroMcpRegistryPublicHeaderConsumer`, `HoroMcpControllerPublicHeaderConsumer`,
then run the three MCP test suites serially. These consumers are OBJECT
compilation evidence, not executed tests. Security regressions
use deterministic test-only entropy; production hosts must use OS entropy.
Local builds, runtime cases and quality preflight are not yet executed for this
draft. Hosted gates cannot be claimed before analysis of the published exact head.
