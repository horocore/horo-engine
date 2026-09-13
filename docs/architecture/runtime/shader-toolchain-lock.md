# Shader Toolchain Lock And Qualification

## Purpose

This document records the reviewed executable set admitted by the production
offline adapter implementing [ADR-035](../../adr/035-shader-source-and-intermediate-representation.md).
The code-owned Linux catalog returned by `ApprovedShaderCompilerTools()` is one
machine-readable authority. Other cook hosts use a host-composed
`IVerifiedShaderCompilerToolCatalog` capability backed by an immutable signed
catalog verified against host-owned trusted roots. An installed SDK default,
matching version text, unverified catalog, or executable with different bytes is
not admitted.

The host supplies absolute executable paths and an isolated scratch root. The
adapter verifies the target's archive identity, the configured executable
identity, and the executable bytes when it is created, before it writes source
or invokes a child. The host keeps the admitted installation immutable for the
adapter lifetime.
It uses the shell-free platform process boundary with an empty environment,
finite output, file and time bounds, cancellation, and per-invocation scratch
cleanup.

## Reviewed Linux host set

The qualified host identity is `linux-x86_64-ubuntu-26.04`. SHA-256 values below
are lowercase canonical digests. Archive/package bytes were retrieved and
verified on 2026-09-13.

| Role | Release and upstream artifact | Archive/package SHA-256 | Executable SHA-256 | License |
|---|---|---|---|---|
| DXC | `v1.9.2607`, `linux_dxc_2026_07_29.x86_x64.tar.gz` from Microsoft DirectXShaderCompiler releases | `55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54` | `dxc`: `b1bfa493d5c780b94c20b8b5f5aed50d1c4d03339cd55f496bd223eedeec1734` | NCSA and MIT files in the release |
| SPIRV-Tools validator | Ubuntu Resolute `spirv-tools_2026.1-1_amd64.deb` | `24e972ed4f2e92ada6f64b32ff40550fda02038385656736af871ca3dcb2b867` | `spirv-val`: `85367fefdb7e93ae45654255ac2b7f8dc7056b6df78a6fdeb03ce395c7477239` | Apache-2.0 |
| SPIRV-Cross | Ubuntu Resolute `spirv-cross_2021.01.15+1.4.335.0-1_amd64.deb` | `50d11b7efc263240d04b015fecfd419377a4a3e2a9cb59387f2229aae03e4e7f` | `spirv-cross`: `335caee5ce86daefc3dee5e13100c2118a1a1cccb183a8c6df817090e0cbb976` | Apache-2.0 |
| DXIL validator | `v1.9.2607`, same Microsoft archive as DXC | same DXC archive digest | `dxv`: `87cc9c1e459a7d6a0dc52c7b319d627d0f6078a2be95621e5a98b3cf12f255cb` | NCSA and MIT files in the release |

## Cross-host catalog contract

Windows, macOS, and additional Linux cook hosts are not constrained to the
built-in Ubuntu artifact set. Their application composition supplies a verified
catalog capability whose immutable snapshot authorizes the complete tuple of
cook-host identity, tool role, release, archive digest, and executable digest.
The verifier must validate the signed catalog before constructing that
capability; the adapter then hashes each selected executable independently and
rejects any mismatch before process execution.

This keeps target production independent from cook-host identity: a qualified
Windows host may cook Vulkan, OpenGL, D3D12, and Null artifacts, while a
qualified macOS host may cook Vulkan, OpenGL, Metal, and Null artifacts. A
target is unavailable only when that host's verified snapshot lacks one of the
exact required tools. There is no fallback to the Ubuntu catalog, an ambient
SDK, another backend, or a locally invented digest.

DXC's same-archive runtime companions are `libdxcompiler.so`
(`be01593d3ff635fca6f20b044aa49c777f6409e50a28fb0242a0c87272620b62`)
and `libdxil.so`
(`e74c02162cd553a0ceab041937d749cfd6ab9adf919142603ff5fc55af222c73`).
The host package must preserve those adjacent release files. DXC additionally
uses the Ubuntu system `zlib`, C/C++, math and loader runtimes. The two Ubuntu
packages declare `libc6 >= 2.38`; SPIRV-Tools declares `libstdc++6 >= 13.1`, and
SPIRV-Cross declares `libgcc-s1 >= 3.3.1` and `libstdc++6 >= 14`.

## Admitted routes and options

- Vulkan: HLSL 2021 through DXC `vs_6_0`, `ps_6_0`, or `cs_6_0`, explicit
  Vulkan 1.3 / SPIR-V 1.6 target, GL buffer layout, column-major matrices and no
  automatic depth remap; every module is checked by `spirv-val` for Vulkan 1.3.
- OpenGL: the same validated SPIR-V is translated by SPIRV-Cross to desktop GLSL
  4.10 with no `420pack` binding extension. The adapter rejects output without
  the exact `#version 410` contract.
- D3D12: the same HLSL is compiled directly by DXC to Shader Model 6.0 DXIL and
  checked by the same-release `dxv`. It never round-trips through SPIR-V.
- Headless Null: emits only a deterministic validation package derived from the
  already validated source/manifest identity. It makes no native compilation or
  visual claim.
- Metal: the implementation requires DXC, SPIRV-Tools, SPIRV-Cross and a locked
  Apple `xcrun`/Metal toolchain, uses MSL 2.4 and macOS 14 deployment, and checks
  the resulting `metallib`. No Apple tool artifact is in the built-in catalog;
  a macOS product host must supply its signed, verified catalog snapshot. An
  absent or unverified snapshot returns typed `tool_not_approved`/`tool_missing`
  rather than using ambient Xcode or placeholder output.

Release payloads never contain compiler debug information. A debug request
creates a separate companion package, so enabling diagnostics does not mutate
the admitted payload identity. Fast-math and strict IEEE modes are explicit and
participate in the artifact key.

## Immutable includes and package format

`ShaderCompilerDependency` now owns the immutable include bytes as well as its
logical path and digest. Validation hashes every include, enforces per-file and
aggregate limits, and rejects a mismatch before adapter invocation. This is an
additive source-level migration: producers that previously supplied only a digest
must also supply the exact admitted bytes from the Asset Pipeline snapshot.

The adapter returns a versioned `HOROSHDR` envelope containing the backend,
payload format, ordered stage records, and bounded native bytes. SPIR-V records
must have valid word alignment, magic and version; DXIL records must be validated
containers; GLSL must be NUL-free 4.10 text; Metal must be a compiled `MTLB`
library. Reflection and binding-map normalization remain owned by RND-011.4 and
are not invented here.

## Qualification evidence

`HoroShaderCompilerPipelineTests` always covers lock rejection, immutable include
validation, deterministic Null packaging, cancellation, output bounds and
transactional multi-target failure. Its `[integration]` case uses the real
portable fixture under `tests/fixtures/shaders/` and runs each available locked
Linux route twice, requiring byte-identical payloads and keys.

The integration case is explicitly unavailable unless all four reviewed Linux
paths are supplied through `HORO_TEST_SHADER_DXC`,
`HORO_TEST_SHADER_SPIRV_VAL`, `HORO_TEST_SHADER_SPIRV_CROSS`, and
`HORO_TEST_SHADER_DXIL_VALIDATOR`. A skipped route is not qualification evidence.
Each production host remains unqualified until its exact tool artifacts and
successful repeatability results are present in its signed catalog and CI
matrix. Contract tests run on the repository Windows, macOS, and Linux lanes;
real-tool qualification evidence remains a separate release input and is never
inferred from a mocked process test. This document must not be edited to imply a
host-specific qualification gate passed when its signed snapshot is absent.

## Lock updates

A lock change requires review of the exact upstream release, archive digest,
executable digest, dependencies and license; focused real-fixture compilation;
repeat compilation on every admitted host; native validator success; and
artifact comparison. Catalog edits, this record, fixtures, SBOM inputs and CI
evidence ship together. Floating downloads and locally rebuilt binaries cannot
replace a reviewed entry.
