# Network transport qualification

`HoroNetworkTransportNullTests` and (when `HORO_BUILD_NETWORK_GNS=ON`)
`HoroNetworkTransportGnsTests` compile the same
`TransportQualificationContract.h` assertions. Test-only adapters project the
different public contracts into packet, admission and shutdown observations;
they do not install a production fallback or expose GNS native types to the
network API. Loopback and unimpeded simulated runs split a nine-byte message
into four-byte fragments and reassemble it; production GNS delivers one native
message and supports only channel zero and at most 1200 bytes. The common
contract checks exact payload ownership, coalesced submissions, channel
admission, size bounds and shutdown rejection without pretending the backend
capabilities are identical.

The deterministic tests additionally distinguish a scheduled copy from an
explicit simulated loss, duplicate, or reliable backpressure result. A
successful native GNS `Send` means that GNS accepted the bytes, not that the
remote application acknowledged them; a terminal connection event is the
observable backend-loss boundary. Neither native UDP delivery nor a product
network is qualified by the deterministic runs.

The existing backend suites supply cancellation, stale-generation, malformed
native-packet and idempotent shutdown cases. The qualification additions
explicitly retain a completion producer beyond destruction of its dependent
`NetworkIoService`, then require typed publication rejection. A production
peer-loss case requires one terminal event and no subsequent packet callback.
With a one-event GNS poll bound, eight coalesced accepted sends must all drain;
the deterministic one-event owner output test checks the same no-silent-loss
invariant across fragmented deliveries and later ticks.

The measurement cases print one line per declared workload to CTest output:

| Backend | Workload | Measured domain |
| --- | --- | --- |
| Loopback | 128 reliable, 32-byte messages; one owner-thread tick for sending and one for draining | C++ `new`/`new[]` count, send and drain elapsed microseconds, send throughput, and shutdown elapsed microseconds |
| GNS | 32 reliable, 256-byte messages over `127.0.0.1`, in eight bounded batches of four | Process C++ `new`/`new[]` count, total elapsed and delivered throughput, maximum batch latency, and native shutdown elapsed microseconds |

Measurements are observational rather than hard performance gates: host CPU,
OS scheduling, compiler mode, and GNS native/OS allocation paths differ. The
allocation probe counts C++ global allocation operators in the test process;
it cannot count C `malloc`, GNS-internal custom allocators, or socket buffers.
The GNS batch-latency figure includes test assertions, owner polling and OS
scheduling; it is not a one-way network latency estimate.
Run the same configurations and workloads before comparing numbers. The
ordinary regression cases remain correctness gates for every accepted or
explicitly rejected operation.
