# Error And Diagnostics Architecture

## Purpose

This document defines how Horo Engine represents, propagates, presents, and
observes failures across C++, GUI, CLI, MCP, Python tooling, background jobs,
and runtime systems.

Errors are typed operation results. Diagnostics explain those results to humans
and tools. Logs provide supporting evidence. These are related signals, but
they are not interchangeable.

## Core Decisions

- Expected failures use typed `Result<T, Error>` values.
- Exceptions do not cross public engine module, host, plugin, C ABI, or thread
  boundaries.
- Every externally visible failure has a stable machine-readable error code.
- Errors preserve a cause chain without requiring callers to parse text.
- Validation may return multiple diagnostics in one pass.
- GUI, CLI, MCP, and Python translate the same application error instead of
  inventing host-specific business errors.
- Logs never become the operation result and are not parsed for control flow.

## Industry Alignment

The Horo error model follows the practices used by production engines, IDEs,
compilers, cloud APIs, and developer tooling:

- expected failures are values, not control-flow exceptions across module,
  thread, host, plugin, or ABI boundaries;
- error identity is machine-readable and stable, similar in intent to
  `std::error_code` categories, HRESULT domains, compiler diagnostic IDs, and
  RFC 9457 Problem Details `type` identifiers;
- human text is presentation fallback, not the branching contract;
- validation can return multiple diagnostics, matching compiler, LSP, and IDE
  workflows instead of failing on the first user-content issue;
- host adapters map the same engine error into GUI state, CLI exit category,
  JSON-RPC error, Python exception, or support bundle payload;
- logs are supporting evidence and correlation data, not the authoritative
  operation result.

Horo deliberately does not copy one external system wholesale. Numeric-only
HRESULT-style codes are compact but poor for extension packages and project
gameplay modules. Exception-only APIs are convenient locally but unsafe across
plugin ABIs, job callbacks, transport boundaries, and process entry points.
String-only ad hoc errors are easy to emit but impossible to validate, localize,
filter, or test reliably. The contract combines typed result values, stable
namespaced codes, structured diagnostics, bounded metadata, and host-specific
translation tables.

## Signal Model

| Signal | Purpose | Owner |
|---|---|---|
| `Error` | Return one failed operation to its caller | Failing operation |
| `Diagnostic` | Describe one or more actionable findings | Validator or operation |
| Log record | Preserve execution evidence | Observability runtime |
| Assertion | Detect an internal invariant violation | Owning module |
| Crash record | Preserve unrecoverable process failure context | Host crash handler |

An operation may return an error containing diagnostics and also emit a
correlated log record. The caller still decides behavior from the typed result.

## Result Contract

Public fallible APIs use a common result type:

```cpp
template<typename ValueT>
using Result = Expected<ValueT, Error>;

using Status = Result<void>;
```

The concrete `Expected` implementation is a foundation detail. Public APIs must
not expose a third-party result type.

Use `Result` for:

- invalid user or project input
- missing files, assets, tools, or capabilities
- unsupported operations
- serialization and validation failures
- recoverable platform and renderer failures
- cancellation, timeout, and resource exhaustion

Do not use `Result` for ordinary query absence when `std::optional<T>` fully
describes the outcome.

## Error Model

```cpp
struct Error {
    ErrorCode code;
    ErrorDomainId domain;
    ErrorSeverity severity;
    std::string message;
    std::vector<Diagnostic> diagnostics;
    ErrorCause cause;
    ErrorMetadata metadata;
};
```

`ErrorCause` is an optional immutable owned reference. Creating a cause edge
allocates one node; copying an `Error` or moving it through `Result` shares the
immutable nodes and does not clone the chain. An error without a cause performs
no cause-storage allocation. This preserves the existing value-copy and aggregate
initialization contract used across module boundaries while preventing mutation
or cycles after publication.

`ErrorDomainId` is a stable namespaced identifier owned by a module, not a
closed engine-wide enum. Built-in domains include:

```text
horo.foundation
horo.platform
horo.configuration
horo.project
horo.asset
horo.scene
horo.physics
horo.render
horo.pipeline
horo.editor
horo.job
horo.extension
horo.gameplay
horo.release
horo.transport
horo.security
```

Project gameplay modules and extension packages use their stable module ID as
the domain prefix, for example `project.combat` or `extension.mesh_optimizer`.
The host validates domains during module activation so two modules cannot claim
the same error namespace in one process. Internally, frequently used built-in
domains may be mapped to compact numeric IDs after validation, but serialized
diagnostics, logs, MCP payloads, CLI JSON, and Python exceptions carry the stable
textual domain.

`message` is developer-facing fallback text, not a stable API. Branching uses
`ErrorCode`. Presentation layers may localize or enrich messages from the code
and structured metadata.

`ErrorMetadata` is a bounded typed field collection. It may contain safe values
such as an asset ID, normalized project-relative path, target platform, or
operation ID. It must not contain credentials, raw environment dumps, arbitrary
user file contents, or unbounded subprocess output.

## Stable Error Codes

Error codes use a namespaced textual representation at serialization
boundaries:

```text
project.not_found
asset.import.unsupported_format
scene.validation.duplicate_entity_id
render.backend.unavailable
job.cancelled
platform.process.timeout
```

Rules:

- a code retains its meaning after release
- changing the meaning requires a new code
- internal enum values may change, serialized names may not
- one code identifies one failure class, not one source line
- host adapters maintain explicit mappings from codes to protocol or exit status
- module-owned codes live under the module's registered domain and are declared
  as descriptors before activation; dynamically inventing codes from arbitrary
  strings at the failure site is forbidden
- extension and gameplay error codes are versioned with the module contract so
  GUI, CLI, MCP, and Python adapters can render and filter them without knowing
  the module's private C++ types

### Error Code Registry

Every externally visible code is declared by an owning module descriptor before
activation:

```cpp
struct ErrorCodeDescriptor {
    ErrorDomainId domain;
    ErrorCode code;
    ErrorSeverity defaultSeverity;
    std::string_view summary;
    std::string_view remediationHint;
    bool retryable;
    bool userActionable;
    std::optional<ErrorCode> deprecatedBy;
};
```

The registry is host-owned and immutable after module activation. It validates:

- duplicate `(domain, code)` pairs;
- invalid prefixes or module namespace collisions;
- removed codes without a documented replacement or compatibility note;
- extension-provided codes that escape the module's registered domain;
- host translation coverage for public CLI, MCP, Python, and GUI surfaces.

Modules contribute ownership explicitly through
`ModuleDescriptor::errorDomains`; descriptor construction remains inert. During
`ModuleHost::ActivateRegistered`, the host builds a candidate
`ErrorCodeRegistry` before invoking any lifecycle callback. A malformed
namespace, duplicate pair, ownership collision, or invalid deprecation returns a
typed `foundation.error_registry.*` failure and leaves the previously published
snapshot unchanged. Successful incremental activation publishes a new immutable
snapshot; readers retaining the prior snapshot continue to observe its original
contents.

Only `horo`, `project`, and `extension` are valid roots. Project and extension
domains must equal or be nested below their exact module ID. Built-in `horo.*`
modules may claim explicit `horo.*` subsystem domains, but two modules may not
claim equal or overlapping domains. A descriptor's domain must equal the domain
claim, and its code must be a canonical lowercase namespaced identity.

Every adapter that exposes an `Error` outside C++ resolves the exact textual
`(domain, code)` pair through the active snapshot first. A null resolution is an
invalid external contract, not permission to synthesize presentation metadata.
The resolved descriptor supplies stable summary, remediation and behavior
metadata while the operation-specific message remains on the `Error` value.

Runtime errors carry a `severity` field, but that severity is bounded by the code
descriptor. A caller may raise or lower severity within the range allowed by the
descriptor; it may not downgrade `Fatal` or internal invariant severities, and it
may not promote a code beyond the descriptor's declared maximum. This keeps
filtering, alerting, and support-bundle interpretation stable across host
adapters. Strict modes may promote documented warning codes to errors, but only
when the descriptor explicitly allows promotion.

Code descriptors are metadata, not a dispatch mechanism. Runtime hot paths may
store compact interned identifiers after validation, but serialized errors keep
the stable textual domain and code.

### Error Registry Migration

ERR-001.3 migrates modules by adding one `ModuleErrorDomainDescriptor` per owned
domain to `ModuleDescriptor::errorDomains`. The contribution contains pointers
to the module's static `ErrorCodeDescriptor` values; those pointers need remain
valid only through activation because the registry deep-copies all textual
metadata. Existing host adapters then replace local descriptor tables or
failure-site string synthesis with `ModuleHost::ErrorCodes()->Resolve(error)`.

Serialized storage, logs, diagnostics and protocol payloads continue to persist
the textual `domain` and `code` values. Registry ordering, vector indices, hash
values, or any future compact numeric allocation are process-local details and
must never appear in public headers, files or wire formats.

### Platform Services Async Failures

[ADR-130](../../adr/130-platform-services-frontend-request-lifetime-timeout-null-and-error-semantics.md)
applies this model to frontend-owned asynchronous platform requests. Pre-admission
failure returns `Result<PlatformRequestHandle<T>>` with no request record. An admitted
request atomically publishes exactly one `Succeeded`, `Failed`, `Cancelled` or
`TimedOut` terminal state/result; observers are deferred views of that record and do
not create another outcome channel.

The following identities are deliberately distinct: `platform.frontend.unavailable`,
`platform.capability.unavailable`, `platform.provider.null`,
`platform.request.cancelled`, `platform.request.timed_out`,
`platform.request.expired`, `platform.request.stale` and
`platform.provider.failed`. Provider failure additionally carries one typed normalized
category such as Offline, NotSignedIn, Forbidden, RateLimited, PreconditionFailed,
QuotaExceeded, InvalidResponse, TransientFailure or PermanentFailure. Native SDK codes
remain bounded redacted cause/diagnostic evidence and never replace Horo branching
identity.

### Serialized Error Shape

Structured host payloads use one canonical safe shape derived from the C++
`Error`. Adapters may wrap it in their protocol envelope, but they do not invent
new business-error fields:

```json
{
  "domain": "horo.asset",
  "code": "asset.import.unsupported_format",
  "severity": "error",
  "message": "Unsupported asset format",
  "diagnostics": [],
  "metadata": {
    "asset_id": "asset-42",
    "path": "assets/source/tree.fbx"
  },
  "cause": null
}
```

For JSON-RPC, the envelope uses the protocol error code while this Horo payload
lives inside `error.data`. For CLI machine output, it is emitted as the command's
declared JSON schema. For Python, typed exceptions expose the same fields. For
GUI, the same payload drives inline diagnostics, notifications, and details
drawers.

The serialized payload follows Problem Details principles: stable type identity,
safe detail fields, and extension metadata. Horo keeps engine-specific names
instead of adopting HTTP field names directly because most failures are local
engine, editor, runtime, or toolchain failures rather than HTTP resources.

## Diagnostics

Diagnostics are suitable for validation that should report several findings:

```cpp
struct Diagnostic {
    DiagnosticCode code;
    DiagnosticSeverity severity;
    std::string message;
    SourceLocation location;
    std::string path;
    std::vector<DiagnosticNote> notes;
    SuggestedAction action;
};
```

`path` is an optional, machine-readable location within the source, such as a
JSON field path or configuration key. Adapters consume this typed value and do
not recover it by parsing the human-facing message.

`SourceLocation` may identify:

- a project-relative file and line/column
- a scene object and component property
- an asset ID and metadata field
- a configuration source and key
- a build stage and tool invocation

Diagnostics are ordered deterministically so CLI output, tests, and support
bundles remain stable.

### Multi-Diagnostic Validation Results

A full validation pass returns `Result<ValidationResult>`. Success means the
pass completed; its value may contain note, warning, error, or fatal findings.
Failure means the pass itself could not be performed. This distinction keeps
the ordinary single-failure `Result<T>` contract unchanged and avoids treating
an error-severity content finding as an operation failure.

Cook and import validators use a `ValidationResultBuilder` created with one
immutable `ErrorCodeRegistry` snapshot and an explicit maximum submission count.
They inspect the complete admitted input and call `Add` for each finding before
calling `Complete`. Every finding identity must resolve through that snapshot;
the registry supplies the canonical severity and fallback message. Source text
is required, while zero line and column identify the entire source. A non-zero
column without a line is invalid.

Completion orders findings by `(domain, code, severity, source, line, column,
message)` and removes only exact tuple duplicates. Distinct messages or source
locations are retained. The submission bound includes duplicates so hostile
input cannot turn deduplication into unbounded work. Reaching the bound fails the
pass with a typed error rather than returning a truncated report. A builder owns
its registry snapshot, is confined to one validation owner/thread, and becomes
terminal after completion or admission failure.

### Build Output Projection

Build and cook producers publish immutable records into one composition-root-owned,
fixed-capacity `BuildOutputStore`. Each admitted producer operation receives a
store-issued, non-zero, monotonically allocated session identity and may also carry
its application `OperationId`. Records keep diagnostic severity separate from an
optional terminal result, and include a stable diagnostic code, stage, UTC timestamp,
store sequence, message, and optional source location. One producer operation emits
at most one terminal result; progress, compiler diagnostics, and cache observations
remain result-less unless they represent an independently correlated terminal scope.

The store assigns sequence and revision under its lock, accounts for overwritten
records, and returns owned snapshots only when the revision changes. Producers may
append from any thread and never depend on editor or GUI lifetime. The composition
root stops producers before destroying the store; panels detach their borrowed query
capability and never retain store-owned references or become output authorities.

## Severity

| Severity | Meaning |
|---|---|
| `Info` | Useful non-failing finding |
| `Warning` | Operation may continue with an explicit degraded condition |
| `Error` | Requested operation failed |
| `Fatal` | Process or isolated runtime cannot continue safely |

Warnings are not silently promoted or discarded by host adapters. Strict modes
may deliberately promote documented warning codes to errors.

## Propagation

Add context at ownership boundaries, not at every stack frame:

```cpp
auto result = importer.Import(request);
if (!result) {
    return Result<ImportedAsset>::Failure(
        WrapError(AssetErrors::ImportFailed,
                  result.ErrorValue(),
                  "Could not import requested asset"));
}
```

Context wrapping preserves the original code or adds a new outer code with the
original error as `cause`. It does not flatten the chain into one string.
Callers inspect a chain with `ErrorChainContains` using exact `ErrorDomainId` and
`ErrorCode` values; message parsing is never a branching contract.

### Cause-Chain Migration

ERR-001.2 adds the final `ErrorCause cause` member to `Error`. Existing aggregate
initializers and `MakeError` callers remain source-compatible because an omitted
cause defaults to empty. A boundary that previously replaced or concatenated an
inner error message migrates to `WrapError(outerDescriptor, innerError, context)`.
Consumers that need to classify nested failures use `ErrorChainContains`; host
presentation adapters alone may flatten the chain for human output. Because the
public `Error` layout changes, binary consumers must rebuild against the updated
Foundation contract even though their source remains compatible.

Background jobs store their terminal `Result` in the authoritative job record.
Completion events carry job identity and terminal state; subscribers query the
job store for full diagnostics.

## Exception Policy

Exceptions may be used privately when required by a standard or third-party
library, but they are caught at the nearest owned adapter and converted to
`Error`.

The following boundaries are exception-free:

- public engine module APIs
- application use cases
- GUI, CLI, and MCP adapters
- job queue callbacks
- renderer backend interfaces
- process entry points and C ABI surfaces

Destructors do not throw. Out-of-memory and corrupted-process conditions may
enter the fatal path when recovery cannot be guaranteed.

## Assertions And Invariants

Assertions indicate programmer errors, not invalid user content. `HORO_ASSERT`
checks programmer preconditions only when `NDEBUG` is not defined;
`HORO_INVARIANT` checks an internal invariant in every build. Both route failure
through the Foundation assertion boundary, which writes a `Critical` emergency
record with source location and terminates. `HORO_ASSERT` is compiled out in
release configurations without evaluating its condition or message.

The standard CMake configuration policy is:

| CMake build type | `NDEBUG` | `HORO_ASSERT` | `HORO_INVARIANT` |
|---|---:|---|---|
| `Debug` | unset | Evaluated; failure reports and terminates | Evaluated; failure reports and terminates |
| `Release` | set | Compiled out | Evaluated; failure reports and terminates |
| `RelWithDebInfo` | set | Compiled out | Evaluated; failure reports and terminates |
| `MinSizeRel` | set | Compiled out | Evaluated; failure reports and terminates |

Custom configurations follow their `NDEBUG` definition. Keep assertion
expressions free of side effects. Use the mechanisms below according to the
condition being represented:

| Condition | Debug | Release configurations | Contract |
|---|---|---|---|
| Invalid project, asset, scene, network, or protocol input | Return typed `Result`/`Error` or validation diagnostics | Same | Validate at the input boundary; never assert on user-controlled data. |
| Expected operation failure | Return typed `Result`/`Error`; log at the actionable owner when useful | Same | Callers branch on typed identity and status, never text. |
| Programmer precondition violation | `HORO_ASSERT` reports source context and terminates | `HORO_ASSERT` is absent | A release caller must not depend on the check for memory or persistent-data safety. |
| Internal invariant whose violation makes continued use unsafe | `HORO_INVARIANT` reports and terminates | Same | Keep the check active in every build. |
| Non-fatal execution evidence | Structured diagnostics/logs | Same | Reporting does not become an operation result or control-flow input. |

Release safety checks belong in typed validation and always-on invariants. A
debug-only assertion cannot guard memory safety, resource lifetime, persisted
state, or another property required to continue safely. `Result` accessors use
always-on invariants for invalid `Value()`/`ErrorValue()` access, avoiding a
release-only standard-library exception or unchecked dereference.

`Logger::WriteEmergency` bypasses filtering and queued sinks, writes directly to
standard error, flushes it, and returns no status. It is used for fail-fast
reporting; normal `LOG_*` calls remain supporting evidence and never decide
operation success, failure, or recovery.

Expected severities convert exactly as follows:

| `ErrorSeverity` | `DiagnosticSeverity` |
|---|---|
| `Info` | `Note` |
| `Warning` | `Warning` |
| `Error` | `Error` |
| `Critical` | `Fatal` |

Logging levels remain a separate evidence vocabulary. Similar names do not
create an implicit conversion or control-flow contract.

## Host Translation

### GUI

The GUI maps errors into inline field diagnostics, non-blocking notifications,
workflow error states, or explicit fatal dialogs. Presentation code does not
inspect log text.

### CLI

Human output is written to `stderr`. Machine output uses the command's declared
structured schema. Exit codes are stable categories documented by
[CLI Architecture](../interfaces/cli-architecture.md).

### MCP

Protocol errors are distinct from application errors. Valid requests that fail
in the application return a structured application error payload with the
stable Horo code and safe diagnostics.

### Python

Python adapters raise typed Horo exceptions only at the Python API edge. Each
exception exposes the same code, domain, metadata, and diagnostic list as the
C++ result.

## Logging Relationship

The owner of a failed operation logs once at the boundary where the failure
becomes actionable. Intermediate functions propagate context without repeatedly
logging the same failure.

Error records include `operation_id`, `job_id`, `request_id`, or other active
diagnostic context when available. Sensitive fields follow the redaction rules
in [Logging, Context, And Diagnostics](../observability/observability-logging.md).

## Testing

Required tests cover:

- stable code serialization and deserialization
- error-code registry duplicate rejection and domain ownership validation
- host serialized payload shape for CLI, MCP, GUI details, and Python exceptions
- cause-chain preservation
- deterministic diagnostic ordering
- exact diagnostic deduplication without loss of distinct source findings
- registry rejection, source validation, capacity, and completed-pass lifecycle
- exception-to-error adapters
- GUI, CLI, MCP, and Python mappings
- cancellation and timeout distinction
- redaction of error metadata
- no success event after a failed operation
- release-build invariant checks that protect memory or persistent data
- build-policy helper behavior: debug assertions follow `NDEBUG`, always-on
  invariants remain evaluated, and error-to-diagnostic severity mapping is exact

## Related Documents

- [System Design](./system-design.md)
- [Observability Architecture](../observability/observability.md)
- [Engine Data Bus](./engine-data-bus.md)
- [MCP Architecture](../interfaces/mcp-architecture.md)
- [Testing Architecture](../delivery/testing-architecture.md)
