# Release preflight migration

`Horo/Release/ReleasePreflight.h` is a new public contract owned by
`HoroApplication`. There is no previous release-plan API to migrate from.

GUI, CLI, MCP, and CI adapters should collect read-only `ReleasePreflightFacts`
for the exact `ReleasePreflightRequest` and call `PreflightRelease` before
submitting work. They must present every returned issue and submit only the
returned `ReleaseExecutionPlan`. The facts must echo the requested project and
output roots before canonical resolution; preflight rejects observations for a
different request.

The executor must call
`ValidateReleaseInputFreeze` with newly observed identities before consuming
each stage's inputs. Credential values remain with the injected provider;
only opaque handles cross the preflight boundary. The canonical machine
snapshot is internal job data and must not be published as an artifact or log.

This header is staged only for `HoroApplication` consumers. Existing callers
of release profile and version APIs need no source change.
