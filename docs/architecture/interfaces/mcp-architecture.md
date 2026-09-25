# MCP Architecture

## Purpose

This document defines the Model Context Protocol (MCP) integration in Horo
Engine. MCP exposes editor, scene, asset, build, and release operations to
external clients through a stable, typed protocol. The same application use
cases power GUI, CLI, and MCP interactions; MCP does not implement engine
business logic independently.

Normative decision: [ADR-177: MCP Application Capability and Host Boundary](../../adr/177-mcp-application-capability-and-host-boundary.md).

## Core Decisions

- MCP is a transport and in-process adapter over Horo application capabilities;
  it is never an authoritative domain API.
- Every entry path dispatches through one host-owned `McpController` and one
  immutable registry snapshot.
- Tool descriptors classify effects as `Query`, `PresentationSideEffect`, or
  `Mutation`; classification is explicit and mandatory.
- Protocol version, tool-contract version, and application-capability version
  are separate compatibility domains.
- Application errors, cancellation, deadlines, bounds, and operation ownership
  survive adapter translation without being converted into empty success.
- Domain targets never depend on MCP targets or types. Executable composition
  roots construct application owners before MCP adapters and tear them down after
  MCP admission closes and drains.

## Scope

MCP is available in both supported hosts:

- `HoroEditor`: the graphical host
- `horo-engine`: the terminal and headless host

MCP owns:

- transport and session lifecycle
- protocol schema and request/response serialization
- tool registry and dispatch
- request validation and error translation

MCP does **not** own:

- engine business rules
- direct mutation of scene, asset, renderer, or editor state from a transport
  thread
- ImGui or terminal presentation logic

## Ownership

```text
HoroEditor                    horo-engine
    |                             |
    +-- McpServer                 +-- McpServer
            |                             |
            +-- McpController             +-- McpController
                    |                             |
                    +-- Application Use Cases ----+
                            |
                    Scene, Asset, Build, Release, Diagnostics
```

`McpServer` owns the transport lifecycle. `McpController` owns protocol-level
validation and dispatch through a host-owned registry snapshot. Application
capabilities own the actual operations. Both transport and embedded in-process
adapters submit the same transport-neutral request context to that controller;
the embedded path has no privileged handler or service access.

## Transport Layer

The initial transport/session profile is selected by [MCP-001.2]. This document
requires any selected transport to be swappable and independent of engine domain
targets; it does not authorize remote binding or select an HTTP implementation.

```cpp
class IMcpTransport {
public:
    virtual ~IMcpTransport() = default;
    virtual void Start(IMcpMessageHandler& handler) = 0;
    virtual void Send(const nlohmann::json& message) = 0;
    virtual void Stop() = 0;
};
```

The in-process adapter does not implement `IMcpTransport`; it translates its
caller envelope into the same controller request and receives the same controller
result. Session creation, concrete transport selection, and remote policy are
implemented by later lifecycle and security tickets.

## Session Lifecycle

1. Host starts `McpServer` during initialization.
2. `McpServer` creates the configured transport.
3. Client connects and sends `initialize` request.
4. Server responds with capabilities and tool list.
5. Client calls `tools/list` and `tools/call`.
6. On host shutdown, server drains in-flight requests and closes transport.

All engine mutations triggered by MCP are queued onto the main thread in the
GUI host. The CLI host runs them on the host's main thread directly.

## Tool Registry

Tools are registered in `McpController` by name. Every MCP tool is described by a
validated `McpToolDescriptor`. The descriptor is the protocol contract; the
handler is only the adapter that invokes the application use case.

```cpp
struct McpToolDescriptor {
    std::string name;
    std::string description;
    nlohmann::json inputSchema;
    nlohmann::json outputSchema;
    CapabilitySet capabilities;
    HostAvailability hosts;
    McpToolEffect effect;
    CancellationPolicy cancellation;
    TimeoutPolicy timeout;
    RateLimitPolicy rateLimit;
    McpToolVisibility visibility;
};
```

```cpp
// mcp/tools/CreateObjectTool.h
struct CreateObjectTool {
    static constexpr std::string_view Name = "create_object";

    static McpToolDescriptor Descriptor() {
        return {
            .name = std::string(Name),
            .description = "Create a scene object of a declared type",
            .inputSchema = R"({
                "type": "object",
                "properties": {
                    "name": { "type": "string" },
                    "primitiveId": {
                        "type": "string",
                        "description": "primitive catalog identifier"
                    }
                },
                "required": ["name", "primitiveId"]
            })"_json,
            .outputSchema = R"({
                "type": "object",
                "properties": {
                    "objectId": { "type": "string" }
                },
                "required": ["objectId"]
            })"_json,
            .capabilities = CapabilitySet{Capability::SceneMutation},
            .hosts = HostAvailability::Both,
            .effect = McpToolEffect::Mutation,
            .cancellation = CancellationPolicy::Cooperative,
            .timeout = TimeoutPolicy::FromDescriptor,
            .rateLimit = RateLimitPolicy::Default,
            .visibility = McpToolVisibility::Public
        };
    }

    static McpCommandResult Execute(IObjectCreationCapability& capability,
                                    const ObjectCreationRequest& request,
                                    const McpRequestContext& context);
};
```

The set of supported `primitiveId` values is generated from the
`PrimitiveCatalog` defined in
[Built-In Scene Primitives](../runtime/built-in-scene-primitives.md). Hardcoded
object type lists in tool schemas are not permitted.

Registration:

```cpp
mcpController.RegisterTool<CreateObjectTool>();
mcpController.RegisterTool<ImportAssetTool>();
mcpController.RegisterTool<BuildProjectTool>();
```

The host validates tool names, schemas, capabilities, effect category,
supported hosts, and permission requirements before advertising the tool to a
client.

The registry implementation and public descriptor types are delivered by
[MCP-001.3]. Their contract is fixed here: every descriptor declares exactly one
closed effect category.

| Effect | Allowed behavior |
|---|---|
| `Query` | Read one authoritative bounded snapshot. It cannot change focus, selection, documents, caches, external systems, or domain state. |
| `PresentationSideEffect` | Request an ephemeral interactive-host action such as reveal, focus, or selection through an application controller. It cannot mutate portable project state. |
| `Mutation` | Change durable state, start externally observable work, or perform an external side effect through an authoritative application capability. |

Discovery exposes this category to clients and authorization policy. Missing or
unknown categories reject registration. Runtime behavior that exceeds the
declared category is a contract violation; a query reports a repair proposal
rather than repairing state implicitly.

## Request Lifecycle

```mermaid
sequenceDiagram
    participant Client as MCP Client
    participant Transport as McpTransport
    participant Controller as McpController
    participant Validator as Schema Validator
    participant App as Application Use Case
    participant History as McpHistoryStore
    participant Bus as EngineDataBus
    participant View as MCP history view

    Client->>Transport: tools/call("create_object", args)
    Transport->>Controller: Dispatch("create_object", args)
    Controller->>Validator: Validate(args, schema)
    alt validation fails
        Validator-->>Controller: ValidationError
        Controller-->>Transport: JSON-RPC error
        Transport-->>Client: error response
    else validation passes
        Controller->>App: Execute use case
        App->>App: Mutate scene through command
        App-->>Controller: McpCommandResult
        Controller->>History: Append request/result record
        Controller->>Bus: Publish(McpToolInvocationEvent)
        Bus-->>View: Invalidate history view
        Controller-->>Transport: success response
        Transport-->>Client: result
    end
```

## Tool Result Envelope

Tool results use a stable envelope unless the tool descriptor declares a custom
schema.

```json
{
  "schemaVersion": 1,
  "requestId": "mcp-42",
  "tool": "build_project",
  "status": "succeeded",
  "result": {},
  "diagnostics": [],
  "operationId": null
}
```

- `schemaVersion`, `requestId`, `tool`, and `status` are always present.
- `result` follows the tool's declared output schema.
- `diagnostics` contain safe, structured diagnostics.
- `operationId` is set when the tool created a long-running operation.

Failures use JSON-RPC error envelopes. The `error.data` field contains the
canonical Horo error payload. Protocol-level errors use the JSON-RPC error code;
engine-level errors use `-32000` with the structured error payload.

## Long-Running Tool Lifecycle

MCP tools that start long-running operations return an operation-aware result.
A tool may either:

1. complete synchronously within the request budget, or
2. create an application operation and return an `operationId`.

Long-running tools expose:

- request ID
- operation ID
- cancellation token
- timeout policy
- progress subscription or polling tool
- final result query

Example response for a long-running tool:

```json
{
  "schemaVersion": 1,
  "requestId": "mcp-42",
  "tool": "build_project",
  "status": "running",
  "result": null,
  "diagnostics": [],
  "operationId": "op-123"
}
```

Transport threads must not block indefinitely waiting for long-running engine
work. The application use case creates jobs and operation records; MCP reports
operation progress through declared protocol messages or explicit query tools.

[ADR-106](../../adr/106-navigation-bake-ownership-transaction-and-cache.md)
requires the navigation-bake tool to validate project-write/cook capability and
project-contained typed IDs, then submit the same application
`NavigationBakeService` request as GUI, CLI and release cook. It returns the
accepted or joined `operationId` and observes the shared operation result. The MCP
controller owns only protocol/request lifetime; disconnect follows the declared
detach/cancel policy and cannot destroy a detached bake, release its publication
lock, mutate cache/staging or call `INavigationMeshBuilder` directly.

## Request Cancellation

Every accepted MCP request owns or joins a cancellation token. If the client
sends a cancellation notification, disconnects, or the request timeout expires,
the controller requests cooperative cancellation on the associated application
operation or task group.

Cancellation maps to the canonical Horo `job.cancelled` or timeout/platform
error codes and is returned through the JSON-RPC error/data payload or the
operation status query.

Cancellation rules:

- a cancelled synchronous tool returns a JSON-RPC error with `job.cancelled`;
- a cancelled long-running tool updates its operation state and exposes the
  cancellation through the operation status tool;
- forced termination after cancellation maps to `platform.process.terminated`;
- client disconnect detaches or cancels according to the tool's detach policy.

## Threading

MCP transport threads must not mutate engine or editor state directly.

In `HoroEditor`:

- transport receives requests on its own thread
- requests are queued to the GUI/editor main thread
- use cases execute on the main thread
- responses are sent from the transport thread

In `horo-engine`:

- transport receives requests on its own thread
- the CLI host has a single main thread that processes the queue
- use cases execute on the main thread

```cpp
McpCommandResult McpController::ExecuteCommand(std::string_view toolName,
                                               const nlohmann::json& args) {
    // Transport thread
    auto future = m_mainThreadQueue.Post([this, toolName, args]() {
        return DispatchOnMainThread(toolName, args);
    });

    auto result = future.get();
    const McpHistoryRevision revision = m_history->Append(toolName, args, result);

    m_engineBus->Publish(McpToolInvocationEvent{
        .requestId = result.requestId,
        .toolName = std::string(toolName),
        .state = result.ok ? OperationState::Succeeded : OperationState::Failed,
        .historyRevision = revision
    });

    return result;
}
```

## Error Handling

MCP errors are translated into JSON-RPC error objects:

| Error            | Code   | Meaning                            |
| ---------------- | ------ | ---------------------------------- |
| Parse error      | -32700 | Invalid JSON                       |
| Invalid request  | -32600 | Malformed JSON-RPC                 |
| Method not found | -32601 | Unknown tool                       |
| Invalid params   | -32602 | Schema validation failed           |
| Internal error   | -32603 | Unexpected engine failure          |
| Engine error     | -32000 | Use case returned structured error |

Application use cases return typed `Result<T, Error>` values. `McpController`
translates engine errors into JSON-RPC payloads without leaking internal
implementation details.

## Adding A New MCP Tool

1. Define the tool struct in `mcp/tools/<Name>Tool.h`.
2. Implement `Execute` in `<Name>Tool.cpp` using application use cases only.
3. Register the tool in `McpController::RegisterDefaultTools()`.
4. Add a contract test in `tests/contract/mcp/`.
5. Add a protocol test in `tests/mcp/`.
6. Update MCP tool documentation if the tool is user-facing.

## Capabilities

The server advertises the following capabilities:

- `tools`: list and call tools
- `logging`: stream authorized, filtered, and redacted structured log messages
- `prompts`: optional prompt templates for common workflows

Resources are not exposed through MCP; scene and asset data are accessed
through tools, not resource URIs.

## Bounded Query Results

Tools that return scene, asset, diagnostics, log, or operation data must use
bounded result sets.

Query tools support:

- `limit`
- `cursor` or `pageToken`
- revision number where applicable
- stable ordering
- partial result metadata

Tools must not return unbounded scene graphs, logs, profiler data, or asset
lists in one response. Streaming JSONL tools declare a per-record schema and
bounded chunk size.

## MCP History Retention And Redaction

`McpHistoryStore` stores bounded request summaries, not arbitrary full payloads.

The store records:

- request ID
- tool name
- timing
- status
- operation ID
- safe diagnostics summary

Tool arguments and results are redacted according to the tool descriptor.
Secrets, credentials, raw file contents, and large payloads are never retained
in history by default. History retention is policy-bounded by count, age, and
storage budget.

## Save Tool Boundary

MCP save tools follow
[ADR-116](../../adr/116-save-data-threat-model-and-trust-policy.md). Inspect, import,
export, migrate, load, delete and conflict resolution declare separate capabilities,
side-effect policy, confirmation policy and shipping-profile availability. Project
trust or an authenticated MCP session does not imply access to another product,
environment, user, profile, server-world or slot namespace.

Tool handlers pass typed addresses or explicitly admitted external-file handles to
application-owned save use cases. They do not call archive codecs, read arbitrary
paths, select trust roots, access provider/signing credentials, retain leases or
publish runtime/catalog state from an MCP thread. Every source, including a local save
path or authenticated cloud result, follows bounded untrusted-input admission.

Inspection results contain bounded allowlisted metadata, verification state and safe
diagnostics. Raw archive/participant content, unrestricted paths, credentials and
private provider identity are excluded from request history and ordinary results.
Development-only raw export requires a distinct local capability and cannot be
enabled by a remote caller or archive field.

## Security

[ADR-172](../../adr/172-immersive-agent-ownership-authoring-mode-and-risk.md)
specializes immersive editor-agent tools. Multimodal input is bounded evidence,
not MCP authorization: voice, gaze, pointing, contact and physical interaction
cannot approve a tool plan. Mutating plans require a local proposal-bound approval
and still pass through application validation and an atomic editor transaction.
Mode, document, project or XR-session replacement invalidates approval; remote MCP
identity cannot impersonate locally present physical approval.

[ADR-122](../../adr/122-cinematic-trigger-sources-and-capability-policy.md)
specializes cinematic start/control tools. A registered tool remains an MCP adapter
over the common application `ICinematicPlaybackCapability`; it never receives the
runtime service. Mutating start requires authenticated/scoped caller identity where
applicable, trusted project, exact capability, target-world authority and the existing
operation approval. Authentication, localhost access or project trust alone is not
authorization, and approval is bound to the visible request/revision rather than
stored in sequence/project content.

Cinematic MCP start tools are Editor/local-development tooling by default and are
absent from Retail Shipping builds. Remote policy below still applies; a non-loopback
transport cannot widen the tool set. Denial uses the application typed result, creates
no player/effects and is redacted/audited without disclosing unavailable sequence or
world details.

- MCP transport does not accept anonymous remote connections in production.
- Any network transport requires authentication when exposed beyond localhost.
- Tool arguments are validated against JSON Schema before execution.
- Long-running tools support cooperative cancellation.
- Secrets are never returned in tool results or logs.

## Remote Transport Policy

Any future network transport binds to loopback by default. Binding to non-loopback
interfaces requires explicit configuration, authentication, and project trust
approval. The concrete remote transport and session policy are deferred to the
transport and security tickets.

Remote transports enforce:

- authentication
- optional origin allowlist
- request body size limits
- per-client concurrency limits
- per-tool rate limits
- idle timeout
- redacted audit logging

See [Release Security](../release/release-security.md) for signing and transport trust.

## Testing

MCP tests are organized into three layers:

1. **Protocol tests**: validate JSON-RPC framing and error codes.
2. **Tool tests**: validate individual tool behavior in isolation.
3. **Contract tests**: validate that GUI, CLI, and MCP produce the same results
   for the same operation.

Required additional coverage:

- client disconnect cancels or detaches according to tool policy
- cancellation of long-running build/import/release tools
- timeout maps to stable Horo error payload
- remote transport authentication and origin policy
- rate-limit and body-size enforcement
- bounded query pagination and stable ordering
- history redaction of secrets and large payloads
- save inspect/import/export/migrate/load/delete/conflict capability separation,
  hostile-input budgets, shipping remote denial and no raw save/path/credential history

Run MCP tests:

```bash
python3 scripts/dev.py test -- test_mcp
python3 scripts/dev.py test -- contract
```

## Debugging

Enable focused MCP protocol logging through the shared observability runtime:

```bash
HORO_LOG_LEVEL=info \
HORO_LOG_LEVELS=mcp.protocol=trace \
build/debug/bin/horo-engine mcp serve
```

The canonical user-facing command is `horo-engine mcp serve`; legacy aliases may
exist only as compatibility shims and must resolve to the same typed command
request.

`HORO_MCP_LOG=1` may remain as a compatibility alias for
`mcp.protocol=trace`. MCP records use the canonical structured schema, log
directory, rotation, context propagation, and redaction rules. Protocol logs
record frame boundaries, request IDs, tool names, timings, sizes, and result
status; they do not blindly persist complete request or response payloads.

## Adapter Equivalence

GUI, CLI, and MCP adapters must call the same application use cases for the same
business operation. Differences are limited to:

- input parsing and validation envelope
- presentation format
- transport/protocol error envelope
- interactive prompting policy
- progress delivery mechanism

They must not implement separate business rules, scene mutation paths, asset
import logic, build behavior, or release policy.

## Related Documents

- [MCP Panel](../../../mock-studio/designs.md#architecture-interfaces-mcp-panel): React mock design for MCP sessions,
  tool-call history, approval queue, request inspection, and audit surface.
- [System Design](../foundation/system-design.md): host boundaries and dependency direction.
- [Engine Data Bus](../foundation/engine-data-bus.md): how MCP publishes history
  revision notifications.
- [CLI Architecture](./cli-architecture.md): shared headless host behavior and
  protocol-safe output separation.
- [Built-In Scene Primitives](../runtime/built-in-scene-primitives.md): object
  creation primitive catalog and MCP schema source.
- [Error And Diagnostics](../foundation/error-and-diagnostics.md): application error and
  protocol error mapping.
- [Concurrency And Job System](../foundation/concurrency-and-jobs.md): request cancellation,
  task ownership, and owner-thread handoff.
- [Testing Architecture](../delivery/testing-architecture.md): MCP test layers and
  harnesses.
- [Release Security](../release/release-security.md): transport and signing trust.
- [Application Security](../security/application-security.md): runtime authentication,
  capability, project trust, rate-limit, and remote-binding policy.
- [Save Game And Persistence](../runtime/save-game-and-persistence.md): save authority,
  archive admission, transactional publication and cloud coordination.
- [ADR-116: Save Data Threat Model and Trust Policy](../../adr/116-save-data-threat-model-and-trust-policy.md)
- [Observability Architecture](../observability/observability.md): levels, categories, MDC,
  storage, and protocol-log redaction.
