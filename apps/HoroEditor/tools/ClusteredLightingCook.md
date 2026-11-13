# Offline Metal clustered lighting producer

`HoroEditorClusteredLightingShaderCook` compiles the current canonical lit, grid,
shadow and light-culling sources through `ExternalShaderCompilerAdapter`. It is
an offline executable; the editor never invokes it.

Build that explicit target, then supply all ten absolute inputs:

```sh
HoroEditorClusteredLightingShaderCook \
  /absolute/source-checkout /absolute/new-package-root /absolute/private-scratch \
  /absolute/approved-catalog.json /absolute/catalog-signature.json \
  /absolute/independently-approved-host-trust.json \
  /absolute/dxc /absolute/spirv-val /absolute/spirv-cross /absolute/xcrun
```

The scratch directory must already exist and be independently owned by the host.
The destination must not exist. Its parent must already exist and be exclusively
owned by this offline operation. The destination must be outside the actual
scratch directory tree. Preflight resolves existing directory identities and
ancestors, including symlink and case aliases, before the adapter can create any
scratch state. Missing scratch, scratch equal to or beneath a new destination,
and destinations inside scratch are rejected without creating those paths. Destination names beginning with
`.clustered-lighting-package-` (ASCII case-insensitive on every host) are reserved
for private candidates and are rejected
before any candidate or output creation. The source argument is the repository root,
containing `assets/shaders`; filenames come from the canonical recipe.

The existing `VerifiedShaderHostCatalog` authenticates the catalog using the
separately supplied host trust document. Its signed tuples must approve all four
tools for the executable's actual macOS architecture. The adapter independently
checks the installed binaries against those approvals. A Linux catalog does not
approve native macOS executables. This producer does not generate keys, catalogs,
trust roots or approvals; those inputs must come from the authorized host owner.

All four programs require actual same-operation payload and reflection output.
For the canonical culling program, bounded validation of the same SPIR-V bytes
passed to SPIRV-Cross establishes the two inputs' `NonWritable` declarations and
rejects input stores, pointer escapes, and unsupported instructions. After exact
native argument and layout checks, the producer adds `const` only to those two
selected Metal input arguments. The qualified source feeds both reflection
capture and Apple compilation; outputs, other entry points, and the shader body
remain unchanged. Missing or invalid proof fails production. Runtime admission
still requires the actual Metal driver to report readonly inputs.

Each source is bounded to one MiB, each payload to sixteen MiB, and canonical
reflection metadata to one MiB. The complete private candidate is admitted by
`LoadPackagedMetalClusteredLighting` before a single directory rename publishes
it. Failures remove only the candidate claimed by this operation. Existing
packages and unrelated candidate directories remain intact.

The package contains the eight runtime payload/reflection files, exact verified
`shader-host-catalog.json` and `shader-host-catalog.signature.json` snapshots,
and `clustered-cook-provenance.json`. The last file is an **informational integrity
receipt**, not another trust authority. It records actual host identity, signed
tool identities and executable digests, canonical source identities/revisions,
pipeline source digests and actual artifact keys/digests. It includes no trust
roots, local executable paths, secrets, or inferred repository SHA/tree.

Successful production establishes compilation and strict package admission.
Actual native renderer preparation and GPU execution require separate runtime
qualification. No successful production run is implied by the executable's
presence or by its negative contract/filesystem tests.
