# Reference extension modules

The four directories under `examples/modules` are source-free C11 SDK consumers.
Copy one outside the checkout and use its README. Each builds loadable modules,
contract tests and a ZIP containing `extension.json`, `horo-package.toml`, native
binaries and resources. Configure `EXAMPLE_ID` in a clean build to rename every
manifest and compiled identity together.

| Example | Native modules | Actual consumer |
|---|---|---|
| gui-basic | editor | Activity form/action and semantic custom UI |
| backend-library-basic | backend | Checked scalar service registry |
| script-api-basic | backend, script | Admitted Lua import |
| hybrid-basic | backend, script, editor | Shared service used by GUI worker and Lua; editor module excluded headlessly |

The GUI uses copied schema2 nodes and host semantic font/icon/accent/motion roles.
It does not receive ImGui, platform, renderer or mutable document objects. Action
results publish through the existing attributed job/form lifecycle. Theme updates
remain host-owned. Closing UI admission precedes provider/code retirement.

The service profile deliberately exposes only checked signed64 operations.
`add(20,22)` returns42, overflow returns a structured `overflow` failure, and
`fail` returns `requested_failure`. Error codes, not presentation messages, select
behavior. Unknown schema/reserved bits, duplicate methods, excessive arguments,
missing manifest exports/imports or unavailable permissions reject admission.
Cancellation propagates through the canonical script and backend call tokens.
Native callbacks, including shutdown, must support the declared AnyThread lane.

## Real headless Lua invocation

The product CLI uses the same HeadlessExtensionHost and ExtensionManager as other
headless commands. It never executes package code merely because discovery found
it. Supply explicit native and script consent, an independent public publisher
trust document and exact detached signatures for the manifest, every selected
native entry and the Lua source. The document codecs are the existing
HostSignatureDocument schema; signatures are verified by the actual Security
ArtifactVerifier. The package cannot supply its own trust root.

```sh
horo-engine extension script-run \
  --extension-root /absolute/extracted/hybrid \
  --extension-id com.yourcompany.reference \
  --module com.yourcompany.reference.script \
  --import com.yourcompany.reference.backend-import \
  --script resources/reference.lua \
  --host-trust /independent/approved-publishers.json \
  --publisher your-approved-publisher \
  --temporary-root /private/existing/staging-parent \
  --approve-native --approve-script --output json
```

Each artifact has an adjacent `<filename>.signature.json`. Use your actual
publisher signing process; example paths, test keys, observed hashes and a package
manifest are never publisher approval. The CLI freezes signature-verified bounded
bytes before native activation and loads only an owned private staging directory.
It rereads the independent policy and checks current expiry/revocation before
staging, each actual native load admission, and before Lua execution. Captured
bytes and signatures remain unchanged; the captured verifier is not perpetual
trust authority. Revocation after an admission cannot undo a native callback
already admitted; ordinary cancellation and drain still govern that callback.
Foreign staging paths are preserved. Cleanup occurs only after actual native
retirement completes; uncertain startup/retirement retains the staging evidence.

The supported Lua runtime exposes admitted scalar `api` methods plus only
`assert`, `error` and `tostring`. Source is bounded64KiB, VM memory is bounded and
an instruction hook enforces finite fuel/cancellation. Supported instruction
ceilings are exact multiples1000 in [1000,1000000]; partial hook budgets are
rejected before VM allocation rather than rounded up. Filesystem, process,
network, debug and package-loader APIs are absent. Script trust/capability/scopes
are independently checked by the existing canonical context before binding.
There is no second VM implementation or invocation registry.

## Qualification and boundaries

The repository test runner copies all four examples outside the checkout,
configures only against the packaged SDK, builds/tests/packages/extracts each,
then runs the real validator, ABI conformance and actual native manager/registry/
Lua/GUI-worker harness. A separate real product CLI test signs freshly built
artifacts with the existing test signer and runs `horo-engine`; these test keys
are never production trust. Negative tests cover consent, mutation, sandbox/fuel,
ABI prefixes, shape limits, cancellation and retirement. Service-enabled headless
hosts preserve existing offlane destruction behavior: atomic admission closes
immediately, while native owner-lane finalization that cannot finish is retained
as canonical RestartRequired. That is not leak-free teardown or proof of drain. CI must run the same
route on supported Linux x64, macOS arm64/x64 and Windows x64 profiles.

This document describes implementation and test intent. No local build, hosted
matrix, native visual check or release artifact is implied by source presence.
SDK-001.6 provides one supported Lua adapter; it does not close EXT-005.4's two
runtime/rebind acceptance. C ABI1.8 preserves old prefixes; C++ composition users
must rebuild from the matching snapshot. Gameplay SDK25 is unchanged.
