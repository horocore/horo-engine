# Generic material binding migration

HORO-378 adds a renderer-owned reflected binding boundary. All three public
headers (`MaterialBinding.h`, `MaterialBindingBackend.h`,
`MaterialBindingErrors.h`) belong to `HoroEngine::RenderApi`; consumers receive
only the owning target's staged header surface. `RenderBackend.h` includes only
the small adapter interface, keeping packed PBR/reflection/vector/variant inputs
outside its direct dependency surface.

Existing scene semantic tables, `PrepareStandardPbrMaterial`, pass planning,
shader reflection and pipeline preparation contracts remain their sources of
truth. No caller changes are mandatory. A RenderFrontend composition creates a
`MaterialBindingTable` with its selected `IRenderBackend` and finite limits.
After cooked variant and product-profile admission, normalize the exact final
reflection, provide every active resource array element and owned packed bytes,
and publish at a render-capable preparation safe point. Backend adapters must
resolve exact pipeline/resource generations and usage/range compatibility
before creating a native table; the returned lease owns the corresponding
registry pins. Concrete native realization is delivered by each backend ticket,
including HORO-319 for Metal. The default boundary fails with typed Unsupported;
this change does not claim any installed backend now renders these bindings.

Frame consumers acquire an exact published ID. They borrow immutable descriptor
storage and the adapter lease for their submission lifetime. An adapter retains
native command-use references until native completion even if preparation
leases are released. To replace a binding, publish the new generation, switch
future extraction to its ID, then release the old table reference. Consumer-held
old generations still consume the same finite budgets; a full table returns
CapacityExceeded before adapter work. A failed candidate leaves the old entry
available.

Required materials cannot request a fallback. An optional fallback names an
already resident entry with identical interface and target layout. Only exact
adapter Unsupported permits it; the returned selection records the original
error and fallback flag. Stale generations, malformed input and other backend
errors remain failures, with no hidden shader compilation or variant change.

All methods and final acquired-lease destruction run on the creating render
thread. Stop producers before Shutdown; it closes discovery and releases table
references, while already acquired leases remain valid. Retire/abandon native
submissions and drain those leases before destroying the backend or registry.
No destructor waits for normal-frame GPU completion. Cancellation belongs to
host admission before this synchronous preparation call; no worker captures,
background jobs or callbacks are retained.

`HoroMaterialBindingTests` covers validation, publication, failure preservation,
fallback decisions, immutable leases, finite retained budgets and shutdown.
`HoroMaterialBindingPublicHeaderConsumer` builds through RenderApi only. Both
are included in the reduced Windows CI build/discovery closure; Linux/macOS
full suites and hosted quality checks must qualify the actual C++ sources.

The string-backed `MaterialBindingId` in `Mesh.h` remains the logical extraction
identity (including `CoreDefaultMaterial`). `MaterialBindingGenerationId` names
an exact resident table generation with frontend ownership; it is not a logical
material name. The descriptor carries the established cooked `MaterialRuntimeId`;
host extraction resolves logical names to cooked materials before realization.

### Bounded caller-owned packing

The additive `PackMaterialParameterBinding(..., span<byte>, limits)` overload writes
only the validated required prefix. Existing allocating callers retain their
signature and delegate to this same implementation. All declarations, counts,
strides, extents, occupied-byte overlap and input/output aliasing are checked before
any output write; invalid/undersized input leaves the whole destination unchanged.
Byte storage needs no stricter pointer alignment because scalar representations are
copied as bytes. Padding is zero and an oversized destination suffix is preserved.
The implementation uses a fixed 8 KiB occupied-byte bitmap under the existing
64 KiB block ceiling. A frame-hot caller retains bounded per-slot storage through
actual upload/recording; no vector resize or packing allocation belongs in that
path. `PackMultiviewShaderParameters` exposes the corresponding span overload and
uses the same canonical scalar/matrix mapping. Public consumer coverage and tests
compare exact bytes, rejection atomicity, source aliasing and allocation requests;
measurements apply only to the tested packing calls, not a complete native frame.
