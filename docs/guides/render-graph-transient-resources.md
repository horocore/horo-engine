# Prepared render graph transient resources

HORO-362 / #362 / [RND-010.5] adds explicit frontend realization of the immutable
graph lifetime plan. Existing imported-only `ExecuteGraph` overloads continue to
work. A graph declaring used transient resources requires a prepared set; logical
allocation slots alone grant no native storage or execution permission.

The host compiles the graph, schedule, lifetime plan and execution using the same
graph identity. Outside an active frame it calls
`PrepareTransientGraphResources(lifetime, memoryScope)`. On success it passes the
returned `RenderGraphTransientResourcesHandle` to
`frame.ExecuteGraph(execution, workloads, prepared)`. Hosts retaining Runtime UI
generations use the additional `prepared, ui` overload. Check every result; budget,
capacity, native allocation and unsupported backend errors retain their causes.

## Memory-cost callback migration

Custom `IRenderResourceBackend` implementations must update only
`QueryBufferMemoryCost` and `QueryTextureMemoryCost`: expected invalid, unsupported
or native failures return `Result<RenderMemoryCostPlan>::Failure` with the original
owned error code, domain, severity, message and cause. Replace deliberate
`std::runtime_error` or `std::exception` throws with that typed failure. Neither
method is `noexcept`; only `std::bad_alloc` and `std::length_error` caused by owned
metadata may escape. Release temporary native probes before returning or unwinding.

The public signatures and header ownership remain unchanged: `RenderBackend.h`
belongs to `HoroRenderApi`; consumers use its staged headers through declared
target dependencies. Null and OpenGL already return typed validation failures;
Metal's runtime and planning delegates and Vulkan's requirement probes already
use the same result contract. Their expected failures retain their existing codes.
Custom test backends now exercise typed native errors and the two permitted
metadata exceptions instead of relying on generic exception translation.

Ordinary buffer/texture admission and transient preparation preserve typed backend
errors. Documented metadata exceptions become frontend capacity failures after
rollback. If constructing that owned error also exhausts memory, the metadata
exception can propagate while RAII retains cleanup ownership. No allocation-free
failure or broader backend exception guarantee is introduced. This migration
does not change create, frame, execution or shutdown callback contracts.

Prepare reserves all unique backing slots before creating any resource. Exact
descriptor equality, disjoint scheduled use and the lifetime compiler's queue-role
proof permit one physical native object per compatible slot. Incompatible,
overlapping and cross-role occupants retain separate storage. Unused declarations
consume no storage. This mechanism does not implement distinct native resources
overlapping a placed heap range or grant backend-specific alias-barrier support.

The new public set header belongs exclusively to `HoroRenderFrontend` in the
public header ownership registry. Consumers link that target and its declared
dependencies. New backend implementers default to unsupported until they explicitly
advertise exact-object reuse, validate actual native instances and operations, and
retain the supplied completion lease. Immediate APIs must require the synchronous
`RenderGraphLeaseTransfer` receipt and mark backend ownership before queued work;
never retain the receipt pointer after the call returns. The common request
validator checks immutable operation metadata; backend entry points additionally
require a resource lease and check their native object validity.

A prepared set supports sequential frames, with at most one accepted submission
using its backing at a time. Reuse while its lease remains outstanding returns
`ResourceNotReady`; Present is not completion evidence. Keep separate prepared sets
for overlapping frames. The frontend admits at most eight sets. Releasing a set
invalidates its handle immediately while queued work retains physical backing and
budget charges. Completion drains normal retirement. Renderer destruction closes
the backend native ownership domain before registry and budget destruction.

Supported operation paths are Null's headless instance validation, OpenGL native
buffer copies and prepared whole-color attachments, and Metal's existing native
copy/color encoders on one effective queue. OpenGL restores buffer/draw state and
retains partial immediate commands through fences; no normal-frame wait is added.
OpenGL attachment overhead is an estimated additional 256-byte accounting granule,
not a claim about measured driver allocation. Native requirement classifications
and their estimate provenance stay authoritative.

Vulkan/D3D12 graph translation, compute operations, cross-queue ownership transfer
and placed-resource aliasing remain unsupported and do not silently fall back.
Linux Metal command-model regressions prove admission/order/error semantics only;
native Metal execution needs macOS validation. Injected OpenGL dispatch/fence tests
prove command and lifetime contracts; optional hardware GPU smoke tests are separate
evidence and must be configured and run before reporting a device pass.
