# Proposed source changes

HORO-261 / AIA-002.1 defines inert, untrusted source-change data. It does not add
an agent provider, patch parser, apply executor or mutation owner. Providers translate
their own patch syntax into typed draft values; the core never accepts a raw diff.

`ProjectPolicy::Capture` explicitly binds a stable project identity, host policy
revision and a nonempty finite allowlist of source files/subtrees. `ChangeSet::Capture`
admits an entire draft under that immutable policy and publishes only a complete
const value. There is no wildcard fallback. Source and rename destination paths use
canonical project-relative `SourceDocumentId`, reject absolute/traversal/drive/UNC
syntax, and are checked against the same allowlist. Windows reserved/trailing-dot
names and ASCII case aliases are rejected. These are lexical policy checks, not
physical filesystem capabilities: the later owner must resolve/recheck symlinks,
physical containment, OS-specific Unicode/case aliases and destination absence at
the actual mutation boundary. Capturing a policy never trusts a project or grants
filesystem access.

Operations form a closed variant: create absent file, edit exact base, delete exact
file, and rename to absent destination with optional edits. Recursive directory
removal, implicit overwrite, duplicate touched paths, case-only renames and cyclic
rename graphs are not expressible in an admitted proposal. An adapter must produce
an unambiguous complete proposal rather than rely on operation insertion order.
Files and hunks canonicalize by path and original-base byte offset.

`FileBase::Capture` copies a real immutable `SourceDocumentSnapshot`'s source key,
observation revision, saved-base revision, encoding marker and content/base hashes.
A proposal base is evidence, never current edit authority. `ValidateAgainstSnapshot`
is a pure check against an actual current lease, including expected removed text,
UTF-8 scalar boundaries and the protected BOM. The draft's hunk replacements remain
untrusted source text; validation does not parse or execute a programming language.
All ranges refer to the original base, never offsets shifted by earlier hunks.

The hard profile bounds 64 files, 256 hunks per file, 8 MiB per file/text fragment,
16 MiB aggregate old/new text and 40 MiB wire input. Hosts may narrow these ceilings.
Canonical wire parsing rejects excessive depth, duplicate/unknown fields, malformed
UTF-8/numbers, invalid enums, wrong checksums, unsupported versions and inconsistent
policy identities before returning a model. Failure publishes no partial proposal.

Review, application and save are separate data records:

- `Review(Accept/Reject)` binds the exact proposal ID, revision and digest plus a
  bounded host reviewer identity. It cannot access a mutable buffer or filesystem.
- `ApplicationReceipt` reports no application, complete workspace application,
  pre-mutation failure/cancellation/staleness or explicit partial application. It
  never substitutes a failure for an unreported partial result.
- `DiskSaveReceipt` reports no save, durable save, visible-but-unconfirmed durability
  or failure. Applied source buffers do not imply any disk publication.

Receipt validation checks identities, operation counts and coherent state, not
whether a caller actually performed a mutation. These records remain untrusted
observations. The future host still uses the existing `SourceDocumentService`,
project/file transaction and durable-save owners. This ticket creates no parallel
source buffer, undo stack, filesystem publisher or automatic apply/save transition.

Accept is not `AgentPolicy::GrantToolApproval`. A later host binds operation risk,
proposal revision and current project/trust revision to the existing policy and
consumes its single-use approval immediately before dispatch. A recorded decision
or restored proposal cannot authorize itself. Reject has no buffer/disk side effects.

Recovery wire version 1 stores only the canonical proposal with a checksum. Decode
requires the host's current explicit policy; no open document instance, live review,
AgentPolicy consent/approval, application or save authority is restored. The redacted
version-1 audit wire stores proposal/policy digests, operation counts and optional
review/application/save dispositions. It contains no paths, source/hunk text,
provider/model context, prompts, credentials or unrestricted payloads. Audit decode
returns inert history data. Recovery and audit codecs do not write storage, and do
not reuse asset-bound recovery records with fabricated asset identities.

Validation: macOS arm64, Debug/Ninja. The public header ownership/dependency
configuration passed, both production implementation files compiled in the existing
EditorServices target, and the focused `HoroProposedChangeSetTests` target built
with one job. The full small binary passed 10 JUnit entries and 259 assertions with
zero failures or skips in 0.053 seconds. Its real source snapshots and temporary
native files verify that Accept/Reject and recovery decoding preserve buffers,
history, saved bases and disk bytes; separate AgentPolicy approval remains required.
No whole-engine suite, native Windows/Linux filesystem qualification, provider
transport, apply executor, GUI workflow, Sonar or Codacy scan was run for this change.
These results qualify the inert contract and its codecs, not future physical
containment, multi-file application or durable publication implementations.

## Editor review qualification

The Editor opens the review from a complete model reply tied to source snapshots
that the user explicitly shared. Review cards list admitted operations, and the
source tab presents the diff for per-file or per-hunk decisions. Apply publishes
accepted text to source buffers. Save and Save All persist those buffers.
Delete and rename also require a separate file-operation confirmation.

Build the existing test targets in a configured build directory with
`BUILD_TESTING` and `HORO_ENABLE_IMGUI_UI_TESTS` enabled to include the chat
and source-tab workflows. Substitute the actual build directory for
`build/skeleton`.

```sh
cmake --build build/skeleton --target HoroProposedChangeSetTests HoroEditorLocalAgentPanelRenderTests HoroModelProviderTests --parallel 1
ctest --test-dir build/skeleton --output-on-failure -R '^(HoroProposedChangeSetTests|HoroEditorLocalAgentPanelRenderTests|HoroEditorLocalAgentDockHttpTests|HoroModelProviderContractTests)(::|$)'
```

| Configuration | Evidence to collect |
|---|---|
| Headless, without a model endpoint | Proposal admission and mixed decisions, exact source bases, atomic application at the configured limits, cancellation and group undo. |
| Editor GUI with native project storage | Review UI and workspace ownership, explicit saves, structural confirmation and rollback, concurrent edits, checkpoint recovery and saved-revision validation. |
| Editor GUI with the loopback local-model fixture | The chat panel receives complete replies and dispatches only the review associated with the offered source publication. |
| Provider adapters with the loopback HTTP fixture | The provider contract suite covers local and OpenAI-compatible transport paths using test data. |

The `[concurrency]` cases hold owner-lane publication until the preparation job
has finished. They verify that retiring the workspace or replacing one file
with different bytes of the same size and timestamp prevents the complete
apply. Both buffers and disk files stay as observed just before publication,
and the audit and undo state remain empty.

Run the native storage cases on Windows, Linux and macOS when qualifying those
platforms. Keep results per platform because path identity, exclusive moves and
directory durability depend on the filesystem. The loopback fixture can run
without a live model or real provider credentials. For a deployed provider
configuration, verify model availability and credential approval with that
configuration and record the result separately.

In an Editor session, share two source files and request a change to both.
Open each review card, accept one hunk and reject another, then apply. Check the
diff in the active source tab and confirm that disk bytes change only when the
accepted text is saved. Repeat after editing a reviewed file externally and
verify that the stale review cannot apply. At standard and increased display
scales, verify keyboard focus, readable decisions and a reachable Apply control
in a narrow source tab. Reopen an interrupted review and confirm that it needs
current evidence and a new explicit Apply action.
