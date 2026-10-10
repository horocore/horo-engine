# Shader compiler Build Output integration

`HoroEngine::ShaderBuild` owns the application producer. A host supplies its
qualified, concurrently callable `IShaderCompilerAdapter` and the existing
process/project `BuildOutputStore` to `ShaderBuildService`. Tool installations,
verified catalogs and private `ExternalShaderCompilerAdapter::Create` remain
host composition responsibilities. No SDK or renderer fallback is selected.

The graphical host accepts a shared compiler in `RunEditorGuiApp`, creates the
producer beside its existing scheduler, and exposes that producer through its
screen service registry. An absent compiler returns `ToolMissing` in a failed
output session. Feature callers schedule `Compile` on a host worker; editor and
render callbacks must not run it. Compilation returns owned artifacts to its
caller and does not install a renderer generation or publish asset files.

Each admitted call allocates one store session and preserves its optional
application `OperationId` on every record. Compiler phase, target, source
revision, entry point, shader stage and exact tool identity are carried by typed
provenance. Stable Horo category codes remain separate from native tool codes.
Warnings remain warnings even when compilation succeeds. Failure diagnostics
stream before the failed candidate is discarded; exactly one correlated terminal
record distinguishes success, failure, cancellation and timeout.

Hosts supply bounded, unique logical-source-to-absolute-path mappings from the
source snapshot authority. Source and include locations use those mappings;
generated/unmapped diagnostics remain visible without fabricated navigation.
The existing Build Output pane reads the same store and activates its existing
`OpenDiagnosticSource` / `SourceFileOpenService` path. It does not parse tool
messages or own another output store.

Admission is limited to eight concurrent calls. Each call has a cancellation
source linked to its parent; shutdown closes admission, requests cancellation
for all admitted calls and waits for them to return before the host stops its
workers or destroys the store. Hosts must configure finite process deadlines
and honor cancellation in their adapter. No work survives service destruction.

`ShaderBuildService::Compile` borrows its input only to make a complete synchronous
worker-owned copy before admission. Adapter and output callbacks use that owned
copy, including source bytes, manifest, target/tool descriptors, navigation maps
and operation correlation. Copy failure admits no work and creates no session.
Ordinary callers remain source-compatible; member-function-pointer consumers use
`const ShaderBuildRequest&` for the first parameter. Host scheduling must still
capture inputs by value until the calling worker enters Compile; this is not
permission to queue a dangling reference. This offline call may allocate its input
copy and makes no frame-hot or zero-copy performance claim.

The editor's application-private `EditorWorkerShutdown` guard preserves separate
constructor rollback and constructed-host teardown lifetimes. Both close scheduler
admission; the latter shuts down screens before compiler cancellation/drain and
scheduler cancellation/join. Borrowed screen services remain alive throughout.

Existing `CompileShaderTargets` calls remain source-compatible through the
defaulted final sink argument. Function-pointer consumers must add
`IShaderCompilerDiagnosticSink *` to their signature. Invocation and diagnostic
aggregates append optional fields. Streaming adapters publish their complete
bounded sequence exactly once and return the same sequence on success;
non-streaming adapters' returned diagnostics are forwarded after validation.
The pipeline validates live metadata and preserves sink rejection even if an
adapter ignores it. Output-limit truncation remains visible and cannot grow
the diagnostic envelope. This is offline preparation, never frame-hot work.
