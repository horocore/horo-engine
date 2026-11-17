# Deterministic Prefab reference corpus (PFB-008.5)

These committed, read-only source files are the regression input. `expected.json`
is the explicit semantic manifest: exact canonical source and cooked HPFB SHA-256, AssetId,
instance 41, sparse local identity, nested scope, owning source, parent ordinal
and effective local X translation. Names and file paths never select identity.
All source documents use the current project version `0.2.0`; Scene uses its
supported schema 1, override envelopes schema 1. The generic resource payload is
small deterministic text, wrapped through the actual AssetCook envelope codec.
No generated/native artifact belongs in this directory.

| Input | Contract |
|---|---|
| single.prefab | one root, runtime template and real deferred Scene group publication |
| multi.prefab | root plus sparse child 17, local X=2 |
| resource.prefab / resource*.bin | two ordinary Asset Registry dependencies and verified cooked resource envelopes |
| nested.prefab | placement 7 mounts multi under root at X=3; scopes distinguish both roots |
| variant.prefab | revision-pinned inheritance of multi, with source ownership preserved |
| instance-overrides.json | exact-source transform assignment child 17 X=9; original X=2 remains immutable |
| composition.scene | two independent occurrences, nesting and persisted instance override; five runtime entities |
| malformed.prefab | parser rejects truncated JSON with prefab.document.invalid |
| missing.prefab | graph rejects absent AssetId 99 with prefab.dependency.unavailable |
| stale.prefab | graph rejects wrong parent revision with prefab.dependency.revision_mismatch |

`HoroPrefabReferenceCorpusTests` reads these files through bounded test-only
loading and the production versioned parser. Two independent registry/source
captures use opposite registry/source/resource input orders; resolver identities and canonical HPFB bytes
must agree. Cooked bytes are decoded by the runtime parser and compared with the
independently cooked data. The manifest also pins each complete HPFB artifact digest, captured only after
semantic entity checks and independent reverse-order cooks passed. Tests never
regenerate expected hashes. This is a semantic golden plus canonical artifact
golden, without native outputs or an automatic baseline updater.

Failures project through existing PrefabDiagnosticRecord and Foundation Build
Output, retaining stable error codes and source navigation. The corpus adds no
parallel logger, diagnostic schema, registry, resolver or runtime publisher.

Budget cases use exact source bytes and one byte less, complete nested object
count and one object less, complete derived work allowance and one extra unit,
one override record and a second denied record, exact HPFB bytes and one byte
less, and exact runtime artifact envelope bytes and one byte less. Graph reference count two succeeds and one rejects; closure must contain precisely
multi for nested. Cancellation and failed replacement
leave previously captured immutable expansion/template evidence readable.
Runtime admission closes on provider shutdown; a retained lease is evidence,
not permission to spawn again.

Run headlessly from the repository root:

```sh
cmake --build build/skeleton --target HoroPrefabReferenceCorpusTests --parallel 2
./build/skeleton/tests/HoroPrefabReferenceCorpusTests '[corpus]'
ctest --test-dir build/skeleton -R HoroPrefabReferenceCorpusTests --output-on-failure -j 1
```

The target links production PrefabAuthoring, PrefabRuntime and
SceneRuntimeConversion. It uses real Assets/Scene owners with explicit empty
component projections for the component-free runtime fixture. GPU/backend visual
parity, native component provider registration, packaged executable startup and
performance throughput are separate qualifications and are not claimed here.
