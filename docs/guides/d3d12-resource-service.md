# D3D12 resource service

HORO-332 implements the resource responsibility independently of D3D12 command
recording and presentation. `D3D12Initialization` owns the native device, direct
queue and `ID3D12Resources` service. After successful startup its `Resources()`
method lends the service on the render owner thread. It implements the existing
`IRenderResourceBackend` contract, so allocation/registry callers retain one
canonical descriptor, placement and resource-generation authority. This does not
advertise a complete interactive D3D12 renderer or register a backend implicitly.

## Ownership and preparation

Resource allocation and transfer preparation are load/preparation operations,
serialized outside frame recording. No native types enter `include/Horo/`.
The native constructor seam is private to `HoroRenderD3D12`; it retains the exact
device and verifies that the supplied direct queue belongs to it. Initialization
builds bounded CPU descriptor heaps and a fence after host/runtime admission.
Failure or cancellation before readiness rolls back the entire startup session.

Call `QueryBufferMemoryCost` or `QueryTextureMemoryCost`, reserve that exact plan
in the frontend memory ledger, then pass its `RenderMemoryPlacement` to creation.
The first implementation uses explicit dedicated heaps and placed resources.
Size/alignment come from the selected device, and the entire heap is charged.
A placement must have a fresh pool, offset zero, matching class, compatibility,
provenance, payload and backing; native failure retains the original HRESULT.
This conservative dedicated strategy avoids introducing an unmeasured pool-size
policy or accepting uncharged slack. It is not a performance claim.

Buffers support device-local storage and CPU-visible upload storage. GPU-written
host-visible buffers are rejected. Uniform buffers require native 256-byte size
alignment and at most 64 KiB; raw storage buffers require four-byte elements.
Texture queries check native format/usage/sample support and field-width limits.
Depth sampling uses typeless storage and explicit depth/stencil shader planes.
Views create SRV, UAV, RTV and DSV descriptors as applicable; sampler creation
uses the canonical immutable filtering/comparison policy. Composite mesh/target
records retain their exact buffer/view dependencies.

Array storage requires array shader views, and cube views with nonzero base faces
require CubeArray. Storage/attachment views expose one mip; sampled-only views
may expose multiple mips. These unsupported interpretations return typed errors.
Depth/MSAA transfer payloads are explicitly unsupported rather than silently
converted. All other uncompressed color transfers support 1D, 2D, arrays and 3D,
with arbitrary row widths and base-level-only initialization matching the public
creation contract. Upper mip contents remain undefined until separately produced.

## Transfers, accounting and retirement

Initial uploads are copied synchronously into bounded owned staging, submitted on
the initialized direct queue, and ordered ahead of subsequent use on that queue.
CPU-visible initial buffer bytes map directly. Default resource transfers use
explicit COMMON to COPY_SOURCE/COPY_DEST to COMMON barriers. Future D3D12 command
recording must use this same boundary; another queue cannot consume these
resources without its own explicit synchronization handoff.

`Upload`/`Readback` operate on private backend instances after canonical frontend
identity resolution. They are resource-service operations, not a second public
resource registry or a replacement for `RenderUploadQueue`/`RenderReadbackQueue`.
The existing frontend transfer queues remain lifecycle/accounting models; host
adapters can use their payload/completion contracts with this native executor.
No new frontend transfer submission API is introduced here.

Every transfer has a unique local identity, dedicated upload/readback backing,
command allocator/list and fence point. Native row pitches are expanded on upload
and stripped into owned bytes on readback. Finite host configuration bounds
requests and *native aligned backing*, including padding; forgotten CPU readbacks
retain their charge and cause backpressure. Cancellation stops publication and
retains submitted work until completion. A failed queue signal retains all
references and is retried by `Poll`; no failed Execute/Signal path frees work that
may still be executing. Device removal remains a distinct error.

The frontend's dependency and submission pins govern `Destroy*`. Internally,
transfer and composite references defer actual native/descriptor reclamation.
Identifiers never recycle; exhausted counters return capacity errors. Descriptor
slots recycle only after the last dependency/transfer reference disappears.
`Snapshot` separates resource/transfer backing, retiring backing, staging and the
estimated fixed descriptor infrastructure charge. The configured four descriptor
heaps have a conservative 64-KiB estimate each (at most 2048 descriptors per heap);
sampler plans conservatively charge 64 KiB per sampler as estimated requirements.
These conservative policy estimates are not claims of exact driver allocation.
DXGI local/nonlocal process usage and budgets are fresh observations through
`QueryResidency`; they never increase the host allowance and may overlap on UMA.
There is no eviction of live resources or hidden fallback under pressure.

Before teardown, finish external submissions and frontend pins, call
`StopAdmission`, and poll `PollDrain`. This abandons CPU results and drains copies
without a CPU/GPU wait. `D3D12Initialization::Shutdown` performs the same protocol,
returning `render.d3d12.drain_pending` until release is safe. Destruction requires
a drained service unless the native device is actually removed. Migration from
the startup-only session therefore requires hosts using resource work to retry
pending shutdown rather than destroying an active session. Existing startup-only
callers have no in-flight resource work and retain synchronous shutdown behavior.
This private service does not modify HORO-326's public submission/drain authority.

## Qualification

`HoroD3D12ResourceTests` runs portable bound validation on every platform and
explicit WARP native tests on Windows x64. WARP is test-selected, with no production
software fallback. Native resource tests construct an actual WARP device/direct
queue independently of Windows 11 workstation/Agility startup admission, allowing
Windows Server CI to qualify resource operations without claiming host startup
support. Bounded test polling is permitted solely for test completion.

Coverage includes buffer and pitched texture byte round trips, arrays/volumes,
aligned placement/duplicate pool rejection, descriptor exhaustion and reuse,
composite dependency retention, sampled depth, samplers/raw/constant buffers,
resource/transfer backpressure, cancellation, owner-thread rejection and drain.
`HoroD3D12InitializationTests` covers resource-stage failure/cancellation rollback.
Build both targets and run their tests on Windows; Linux/macOS cover portable
omission and startup policy only. Hosted build/test results are distinct from
local source checks and from physical-device driver qualification.
