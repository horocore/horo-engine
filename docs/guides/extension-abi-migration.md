# Native extension ABI negotiation

ABI 1.1 keeps the v1 host prefix and load/unload symbols while adding an optional
inert `horo_extension_query` bootstrap. The host negotiates before calling load;
an incompatible query cannot receive a registration callback. Missing queries
select the legacy 1.0 contract, not an inferred newer ABI.

## Version and size rules

- `HoroExtensionHostApi::abiVersion` remains the major version, currently 1.
  The appended `abiMinorVersion` is 3; `reserved` is zero. Earlier minor
  requirements of 0, 1 and 2 still negotiate against their original prefixes.
- A query fills `HoroExtensionRequirements` in host-owned storage. It must return
  the complete supported requirements size, matching major, a minimum minor no
  greater than the host's, and a host size the host can supply.
- Required function bits are explicit. Unknown bits and nonzero reserved fields
  fail negotiation; requested callbacks must be present.
- Input-table tails may be appended. Modules must test size before reading new
  fields, and must still validate their requirements in load when supporting an
  older host that does not call query. No field is reordered or repurposed.
- Output `structSize` starts as writable capacity. A module must not write past
  that capacity and returns its populated prefix size. Module results may end
  before `moduleId`, before `moduleVersion`, or after the complete current table.
  Absent legacy identities are cleared and remain manifest-owned. Partial fields
  and sizes beyond supplied capacity fail before contribution commit.
- Query is metadata-only: no allocations, lifecycle state, retained pointers,
  callbacks into host services, or registration. Query errors propagate and
  contract-violating exceptions are contained by the host.

## Source compatibility and ownership

The public header now compiles as C11 and C++20. C `typedef` is intentional; the
C++-only Sonar preference for `using` is inapplicable to this shared header.
Status and setting tags use `uint32_t` rather than compiler-sized enum wire
types. Constant names and values are unchanged. Code that explicitly spells the
old enum tags must use the public typedef names instead. Rebuild native modules
with the updated SDK; binaries built with nonstandard short-enum options are not
covered by compatibility guarantees.

The existing example, host adapter, C consumer, and C++ public-header consumer
are affected callers. The C consumer is a registered CTest target, not a C++ test
that merely includes the header.

Importer context ownership requires a module-provided destroy callback whenever
the context is non-null. A rejected registration does not transfer ownership;
the module must clean it up. Accepted contexts retain the module lease and are
destroyed through the originating module, including transaction rollback.

This bootstrap is not a sandbox or package trust decision. It cannot prevent a
malicious native module from ignoring buffer bounds or accessing process memory.

## Platform provider operation profile migration (ABI 1.3)

The host minor is now 3; the major and all 1.0/1.1/1.2 host-table prefixes are
unchanged. Existing 1.2 provider modules keep `abiVersion == 1` and set
`HoroPlatformServicesProviderDescriptor::structSize` to
`offsetof(HoroPlatformServicesProviderDescriptor, operations)`. Recompiling an
old initializer with the new `sizeof` would incorrectly advertise a tail it does
not implement. The host validates that original prefix and never reads the tail
from a version-1 descriptor.

To use the operation lifecycle, a native module's inert query requires host minor
3, then load registers descriptor `abiVersion == 2` with the full size and a
non-null `HoroPlatformProviderOperations` table at its exact version-1 table
size. The host copies the function pointers during registration. The module
retains ownership of its code and candidate. It keeps every host sink pointer
valid only through close-ingress and drain; drain may return BUSY and must be
retried on the owner lane before candidate retire/destroy. Callback payloads are
borrowed only for one call and copied into the host's finite queue. Old
factory-only modules remain loadable but exact operation-host startup reports
`platform.lifecycle.unsupported_profile`.

The affected callers are the extension registration adapter, the exact
capability resolver, provider admission, and the narrow Platform Services
lifecycle host. Native C fixture modes 10 and 11 exercise old-prefix loading
and the appended operation profile respectively; service-host tests cover
startup rollback, BUSY drain, off-thread callback ingress and repeat teardown.
