# Module-script adapters

HORO-171 adds explicit Lua 5.4.8 and QuickJS-NG 0.18.0 module operations. Both use
`ScriptModuleContext`, the same resolved module imports, the actual backend-service
registry and one canonical invocation/controller per call. This does not add
JavaScript object behaviors; existing Lua object behaviors and the scalar
`extension script-run` command retain their previous contracts.

## Product admission

`extension module-run --runtime lua|javascript` requires the signed package root,
exact module ID, source resource, independent host trust file/publisher, owned
staging root and both `--approve-native --approve-script`. Its options otherwise
match `extension script-run`, except `--import` is absent: the selected actual
module's declared imports are the complete import inventory. The shared cold package capture preserves the existing finite direct-entry native
profile (at most 16 non-presentation modules; entry-variant tables are rejected).
The selected source module must actually admit `extension.services`; a missing
actual source grant is a typed failure, never a fallback permission.

Source must be UTF-8
text without NUL, at most 64 KiB, with a matching `.lua` or `.js` extension.

Example invocation after independently approving and signing the package:

```sh
horo-engine extension module-run --runtime javascript \
  --extension-root /absolute/package --extension-id com.example.module \
  --module com.example.module.script --script resources/reference.js \
  --host-trust /absolute/independent-trust.json --publisher com.example.publisher \
  --temporary-root /absolute/owned-staging --approve-native --approve-script --output json
```

The shipped `examples/module-scripts/reference.lua` and `.js` are source resources,
not independently loadable native packages. Add the selected resource and its
signature to a real package. To request the general built-in reference API, the
actual signed ScriptProvider module must explicitly list `horo.script.reference`
in `requiredCapabilities`. The explicit project/context policy must also approve
that capability. The reserved alias `horo.reference` must not be a declared native
import. Unrequested/denied built-ins never appear. Required missing native imports
remain failures even when the built-in is requested.

Editor composition creates the same operation factory over its actual native
host/registry owners. Workspace `BeginModuleProgram` delegates to the actual Play
controller; it admits only a ready Playing/Paused Scene, explicit capture and both
consents. `TakeModuleProgramResult` consumes the terminal outcome. Owner safe-point
polling follows actual Scene commit. Stop, reload and replacement of the actual
Scene/runtime revoke the operation scopes. Neither package activation nor opening
a workspace automatically executes source.

## Equivalent values and outcomes

A namespace is reached through `horo.import("local-alias")`, then the descriptor's
dotted namespace. Functions return an owned outcome with `.values` and `.error`.
A top-level program must return an actual host outcome, not an arbitrary object.
Lua asynchronous methods suspend/resume the real coroutine; JavaScript methods
return real Promises and use `await`. Provider work runs on JobSystem workers only
for an actually admitted AnyThread service. VM delivery and destruction stay on
the owner lane. There is no frame-thread wait.

| Canonical value | Lua | JavaScript |
| --- | --- | --- |
| Boolean, string, finite number | boolean, UTF-8 string, number | boolean, scalar string, Number |
| Signed 64-bit integer | Lua integer | exact signed BigInt |
| Unsigned 64-bit integer | `horo.u64("18446744073709551615")`, `.decimal` | exact nonnegative BigInt |
| Bytes | binary Lua string | dense byte array |
| Array | dense table | dense Array |
| Map | dense `{key,value}` pair array | dense `[key,value]` pair array |
| Named struct | plain field table | plain data object |
| Enum | admitted descriptor integer/constant | admitted BigInt/constant |
| Nullable/handle | `horo.null` or actual host box | null or actual host object |

No numeric wrapping, getter/proxy invocation, cyclic graph traversal, native
pointer extraction or implicit string/object coercion is part of projection.
Named types, defaults, result shapes, bounds and semantic errors retain their
canonical descriptor/value checks. Errors preserve domain/code/message,
retryability, cancellation and bounded details; an error is not a successful
completion or an abandoned controller.

## Limits, updates and teardown

Default limits are 64 KiB source, 4 MiB VM heap, 256 KiB stack, 64 pending calls,
64 owner events/jobs per Poll, and five seconds execution deadline. Host bridge
owned payload boxes have a separate 4 MiB/1,024-value bound. Lua fuel is 100,000
instructions and accepts only positive multiples of the 1,000-instruction hook
quantum. QuickJS cancellation/deadline is cooperative; there is no claim that a
native intrinsic is hard-preempted after an exact opcode count. `JS_SetCanBlock`
is false; shared memory/Atomics and std/os/libc/bytecode/native loaders are absent.

The bounded shared cache stores descriptor-index recipes, not VM values, provider
callbacks or authority. Cache capacity controls residency, not whether a valid API can run.
If all resident plans remain pinned, acquisition returns an equivalent bounded
caller-owned uncached plan. Resident counts/bytes stay unchanged and pinned plans
remain intact. Uncached plans are released with their caller, outside resident
cache statistics; after pins drain, a later acquisition can retain/reuse that
recipe normally. Keys bind canonical descriptor fingerprint, actual compiled
adapter identity, service/API/profile and effective policy. Compatible replacement
requires the same actual invocation registry and exact live operation scopes.
It validates the replacement descriptor/function allowlist before revoking old
contexts and invalidating changed recipes. Required imports cannot disappear or
be weakened. Optional availability changes remain explicit. Revoked generations
cannot execute through retained old ports or handles.

Cold operation admission refreshes the current independent signed policy and
matches source/native evidence by digest/publisher/key against actual activation.
Every product operation also retains the exact selected native module's actually granted activation capability and code lease, even with zero native imports. Each backend invocation acquires the existing actual callback-use guard for that source. No absent permission is synthesized.

Normal Poll and worker calls do not read trust files. The finite admitted operation
uses that snapshot; it does not promise live file-watching revocation. Actual host
closure/provider revocation/operation cancellation remains effective per call.
Close suppresses deliveries and requests cancellation. Jobs own copied values,
controllers and executable leases, never VM, controller, Scene or UI pointers.
Native retirement retains code until actual canonical quiescence permits unload;
restart-required retention is preserved when that cannot be proven.

## Compatibility and qualification

The extension package/C ABI remains **1.8**. The general reference service is a
real built-in C++ backend projection; it does not extend scalar C ABI 1.8 with a
general-value wire protocol. The lower readonly `BackendServiceCall::Descriptor`
and exact scope `Matches` helpers grant no execution rights. Rebuild affected C++
consumers from the matching snapshot; no unchanged C++ binary-layout claim is made.

The implementation uses MIT-licensed QuickJS-NG core only, pinned by the owning
build/dependency manifest. It does not import upstream CLI/libc/install options.
Qualification must distinguish object compilation, actual two-VM semantic tests,
signed native Headless/Play tests, CLI execution, and hosted platform coverage.
The external author draft itself is not evidence that those product paths passed.
