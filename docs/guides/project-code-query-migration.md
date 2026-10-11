# Project code query composition

This additive pack provides read-only observations. It does not install itself
in an editor or CLI host, grant authority, open documents, index code, or start
builds/tests. Existing host publication remains explicit.

1. On the composition boundary, admit an absolute, explicitly authorized root
   with `Platform::CreateNativeProjectReadFiles(root, identity, generation)`.
   All root components must already have their intended non-link spelling.
   Do not accept a root from tool arguments. Keep failure visible to the host.
2. Create `Application::ProjectCodeQuery` with that lease and reduced finite
   policy if desired. Compose existing project-scoped read capabilities with
   `MakeCodeQueryStoreProviders`. The stores must already have bounded capacity
   and payload policies; the query adapter checks projection budgets before
   constructing result rows and does not mutate stores.
3. For editor composition, supply `MakeSourceDocumentCodeQueryProviders` with
   existing source/identity owners; move its `existingText` provider into the
   combined provider set. Create the adapter and call it on those owners' thread;
   off-owner calls fail before reading the identity registry. It uses
   `Find`/`Snapshot`, never `Open`, `InspectDisk`, `Edit` or `Reload`. A source
   owner failure is preserved rather than falling back to disk.
4. Inject bounded language symbol and typed test-status reads when their
   producer exists. Missing capabilities remain typed Unavailable. The existing
   `OperationKind` has no test category; never infer one from a title.
5. Build registrations with `Mcp::MakeProjectCodeQueryTools` and a retained
   read-only `currentGeneration` callback. Zero means closed. Construction does
   not invoke this callback or any provider. Publish through the existing MCP
   registry with explicit capability availability and authenticated grants.
   Use Background for file-only producers or Editor for source-owner reads;
   these are separate host compositions, not automatic thread dispatch. An
   Editor composition passes no native file capability, so a missing open
   source cannot perform filesystem I/O on the editor thread. A Background
   composition may consume already handed-off immutable source observations
   through its own bounded read provider, rather than calling the editor owner.
6. On project close/reload, revoke admission and change the generation, then
   drain admitted calls before releasing producer owners. Retained responses
   remain valid. No query shutdown job or query cache needs disposal.

`Text` offsets count UTF-8 bytes and pages end at scalar boundaries. Other
offsets count rows. A nonzero offset requires the exact prior revision. Reusing
a cursor across queries/projects or after an edit returns Stale. Search columns
count one-based UTF-8 bytes; LF advances the line. Search is case-sensitive
literal matching, including overlapping matches. Budget exhaustion is a failure,
not a silently truncated successful observation. Producer retention loss is
reported separately as `droppedRecords`.

The concrete filesystem target uses POSIX `openat` with no-follow flags and
Windows retained-handle relative opens. Windows directory enumeration reopens
the retained object by file ID to obtain an independent cursor and verifies its
identity. A volume lacking that facility returns Unavailable; there is no
pathname fallback. Root/ancestor renaming does not change an admitted root.
Hard links, symlinks, reparse points, devices and FIFOs are rejected. Cooperative
deadlines do not forcibly interrupt an operating-system call; hosts needing
stronger isolation supply their own restricted `IProjectReadFiles` implementation.
Filesystem manifests are bounded captured observations, not atomic directory
transactions. Text captures fence native mutation metadata around the read.

Public ownership is explicit: ProjectReadFilesApi owns the neutral Platform
contract; PlatformProjectReadFiles owns its concrete factory; ProjectCodeQuery
owns Application query/store APIs; McpProjectCodeQuery owns protocol registration;
SourceDocumentCodeQuery owns the editor projection. Domain targets have no MCP
dependency and Platform has no Application dependency. Consumers link the
specific owning target; no repository-wide include/source path is required.

`ProjectCodeQuery::Create` borrows `const CodeQueryLimits &` during admission and
copies the policy into the returned capability. Normal calls, default arguments
and temporary policies keep the same behavior. Function-pointer declarations
must use the const-reference parameter and consumers must rebuild for the changed
signature. This avoids copying the complete policy before admission; it does not
retain caller-owned configuration. The constructor remains private and the
factory moves a fully admitted value into one shared allocation. Public consumer
coverage and the caller-policy lifetime regression protect this boundary.

Focused regression targets are `HoroProjectCodeQueryTests`,
`HoroProjectReadFilesTests`, `HoroMcpProjectCodeQueryTests`, and
`HoroSourceDocumentCodeQueryTests`. `HoroProjectCodeQueryPublicHeaderConsumer`
and generated isolated public-header consumers compile the declared surface.
Native tests qualify the actual filesystem; unsupported Windows cursor or
symlink privileges are explicitly reported, never claimed as passed coverage.
