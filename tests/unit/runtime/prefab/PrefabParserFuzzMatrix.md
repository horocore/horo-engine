# Prefab Parser, Schema and Malformed-Input Fuzzing (PFB-008.6)

The tests consume frozen PFB-008.5 files from `tests/fixtures/prefab-reference-corpus`
without writing to that directory. They use production parsers, schema models,
Asset Registry, dependency graph, canonical cook, runtime provider and Scene owners.
There is no alternate parser, graph wire format, filesystem implementation or
runtime publisher.

The default bounded workload is 512 mutation recipes: 96 per format below (three
fixed SplitMix64 seeds, 32 recipes each), eight runtime admission cases and 24
actual document recovery cases. Every input is at most 1 MiB + 1 byte; canonical
baseline reads are capped at 64 KiB. Each failure shrinks within 96 evaluations,
keeping its exact failure signature and mutation family. Large-input cases test
admission before parsing; they do not create unbounded nesting or allocations.

| Boundary | Valid and independent semantic oracle | Malformed mutation families |
|---|---|---|
| Prefab source | fixed root/child 17, X=2; canonical owned data survives reload; unknown opaque component bytes are preserved | nonfinite numbers, duplicate keys, truncated prefixes, lowered exact byte budget, invalid UTF-8, project version skew |
| Identity sidecar | production identity/type/path match and unchanged registry revision | nonfinite schema, duplicate key, truncation, 1 MiB + 1 input, invalid UTF-8 and escaping path, schema skew, unknown type on `.prefab` |
| Instance overrides | canonical persisted assignment resolves child 17 to X=9; source stays X=2 | nonfinite assignment, duplicate target, truncated source, lowered value/set bytes, invalid UTF-8, unknown schema, unknown value kind |
| Dependency graph | explicit canonical source closure, stable edges under source order reversal; exact two-reference limit | invalid revision version, duplicate sources, missing source/registry node, one-reference limit, revision mismatch, unknown source type |
| Cooked template | actual cook bytes, exact payload/object limits, decoded immutable semantic data | nonfinite typed input, duplicate/trailing payload, truncation, one-byte-smaller payload budget, corrupt magic, unknown HPFB version; unknown opaque members remain preserved |

Graph fuzzing mutates its real typed input boundary. There is no persisted graph
parser to invent. A separate cycle boundary case proves that graph capture
remains finite and the production resolver rejects cyclic expansion while an
earlier valid expansion stays readable. Unknown source/cooked component types are preserved by canonical
policy; runtime preparation rejects missing projections rather than pretending
those opaque bytes activated a native component.

Runtime tests wrap actual canonical HPFB bytes in the production AssetCook envelope,
load through real AssetLoadService/PrefabTemplateProvider and attempt deferred Scene
group publication. Corrupt, truncated, wrong-version, excessive-byte and unprojected
member cases must leave the real Scene empty. Valid publication succeeds, shutdown
revokes admission, and retained template evidence remains readable.

A separate source is registered with the existing Scene document test composition.
All 24 persistence recipes use the same bounded runner, shrinker and exact environment
replay as the five format cohorts. Each constructs a complete typed recovery record
with the actual owner/generation, a nonzero revision, deterministic capture time,
accepted source and state, then publishes it through DocumentRecoveryService.Write.
Inspect must reproduce the exact record before attempting recovery.

Nonfinite, duplicate, truncated, invalid UTF-8 and version-skew payloads reject at
source admission. Two valid-payload families deliberately reach later fences: an
unpublished later revision rejects against the actual durable recovery record, and
an exact persisted record rejects after a real external canonical source edit.
Every rejection preserves the complete PrefabDocumentSnapshot (including source,
sourceRevision, savedState, acceptedSource, all fences, recovery/preview revisions,
stage, dirty/conflict and history state), the persisted recovery record and an
exact bounded recursive snapshot of native files. Filesystem and recovery property
failures return stable identities through the shrink/replay runner.

A valid persisted candidate is a success control: actual Recover admits it without
changing native files, then the ordinary Save publishes its exact canonical bytes
with a durable receipt. Source/accepted/saved state, unchanged identity sidecar and
retained recovery evidence are verified. This tests the actual file/document seam;
pure parser tests do not claim filesystem publication coverage.

Failures retain domain/code identities. A dedicated replay case projects real
parser rejection through PrefabDiagnosticRecord and Foundation Build Output with
source-navigation context. Tests add no diagnostic schema or observer service.

Compact source admission and canonical replay use explicit independent parser byte
caps. If canonical formatting exceeds the original compact-input cap, reparsing
under that original cap must reject with PrefabErrors::PayloadTooLarge. A separate
invocation with a cap equal to the actual canonical bytes must preserve the
independent semantic oracle. The source model policy is unchanged; this does not
promise size-preserving serialization or weaken oversized-input rejection.

## Replay and shrinking

Failure output includes an exact eight-word decimal recipe and the test case to
select. For example:

```sh
HORO_PREFAB_FUZZ_REPLAY='3 0 0 0 0 0 0 0' \
  ./build/skeleton/tests/HoroPrefabParserFuzzTests \
  'Prefab source schema fuzz preserves opaque data and rejects malformed bounded inputs'
```

Replay retains the format-specific production boundary, policy, immutable baseline
and exact mutation. Select one exact test name; applying a replay indiscriminately
to every format changes the question. Counterexample regression tests use the real
source parser to prove byte shrinking reduces an actual rejected prefix, preserves
its typed failure, and reproduces the failure. Recipe shrinking separately proves
its reduced recipe survives encode/decode and reconstructs the same failure.

## Headless execution and limits

```sh
cmake --build build/skeleton --target HoroPrefabParserFuzzTests HoroSceneDocumentTests --parallel 2
./build/skeleton/tests/HoroPrefabParserFuzzTests '[prefab-fuzz]'
./build/skeleton/tests/HoroSceneDocumentTests '[prefab-fuzz]'
ctest --test-dir build/skeleton -R HoroPrefabParserFuzz --output-on-failure -j 1
```

CTest runs the focused corpus serially with a process timeout. No GPU, native
backend, gameplay WorldSpawn, packaged-player, exhaustive state-space or sanitizer
qualification is implied. Sanitizer/platform lanes remain separate validation.
