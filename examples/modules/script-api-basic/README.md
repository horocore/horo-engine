# script-api-basic

Copy this directory outside the engine checkout. Configure against the installed
Horo Extension SDK with host ABI1.8, never a repository include directory:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/absolute/path/to/extension-sdk -DEXAMPLE_ID=com.yourcompany.reference
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Release --target package
```

Change `EXAMPLE_ID` in a clean build to change all manifest and binary identities.
The ZIP contains the manifest, package contribution, actual native binaries and
resources. Package installation/signing and explicit local trust approval remain
host responsibilities; an example path never grants native trust.

Expected backend/Lua result: `20 + 22 = 42`; `fail(1,2)` returns a bounded error;
overflow is rejected without signed arithmetic overflow. Cancellation is checked
before calculation, and retired imports cannot enter a provider. The provider
shutdown callback runs after admitted calls drain and before module unload.

The host must supply actual `extension.services` transport and explicitly approve
the sealed `extension.services.invoke` permission and `horo.extension.services` capability. GUI requires actual activity
and custom-composition hosts and grants. Semantic theme roles are resolved by
the host; no colors, ImGui, SDL or renderer objects cross the SDK.

The optional Lua adapter uses the engine's supported Lua dependency and canonical
script capability/invocation authorities. It exposes only admitted scalar methods;
there is no filesystem/process/network/debug/package-loader grant. This example
does not implement EXT-005.4's full two-runtime adapter or reload acceptance.

Hybrid headless composition selects `.backend` and `.script` in the canonical
module plan and omits `.editor`; the same backend and Lua operation remain usable.
GUI teardown closes input/action admission before providers and native code retire.

See `docs/guides/reference-extension-examples.md` in the matching engine snapshot
for the actual signed `horo-engine extension script-run` invocation, permission
policy, resource bounds and teardown contract. The packaged native C11 contract
test is not an engine runtime or visual qualification result. Supported author CI
profiles are Linux x64, macOS arm64/x64 and Windows x64; binaries are architecture
specific and must be rebuilt with matching SDK and host contracts.
