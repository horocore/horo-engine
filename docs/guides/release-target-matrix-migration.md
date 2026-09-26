# Release target matrix integration

`ReleaseTargetMatrix.h` adds target admission and matrix aggregation alongside
the existing single-target `ReleasePreflight.h` API. Existing callers of
`PreflightRelease` continue to receive one immutable execution plan. Group
callers now supply an explicit group ID, complete required/optional membership,
one preflight request/facts pair per target, distinct service-assigned job IDs,
and read-only toolchain descriptors.

The host must capture a toolchain descriptor after compile/link validation and
bind its digest to `ReleasePreflightFacts::toolchainDigest`. That digest must
cover the compiler/linker identity, target tuple, SDK installation and version,
supported platform range, and package-format tooling. Refresh observations and
re-run admission when any of these inputs changes. Set `crossCompilerAvailable`
only for a validated explicit cross profile; the matrix admission checks the
descriptor itself as well. The owning release service assigns group, job and
target identities; adapters pass them through unchanged.

Each accepted `ReleaseMatrixCellPlan` is an independent job input. Consume its
`plan` and `validatedTarget` together and recheck the frozen identities before
execution. A rejected cell carries its validation issues and must never launch
build or packaging stages. After each admitted job reaches exactly one terminal
result, supply the complete result set with exact group, job and target IDs to
`SummarizeReleaseTargetMatrix`. Retain
every returned member result, including optional failures and validation
rejections. Only a `Succeeded` group with final-verified required candidates is
eligible to be presented as a successful multi-platform candidate.

`ReleaseTargetTerminal` now carries `groupId` as its first field. Callers using
aggregate initialization must prepend the service-assigned ID of the group
that produced the result; adapters must not reconstruct it from labels or
paths. This contract change affects matrix-result producers and their tests,
not single-target preflight callers. Without this binding, a terminal from a
different group with the same job and target labels could falsely satisfy a
required member. The summarizer rejects cross-group results even when those
labels match.
