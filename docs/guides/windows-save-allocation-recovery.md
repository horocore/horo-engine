# Windows Save allocation recovery

HORO-2421 / #3393 / [SAV-BUG-001]

## Observed failure

The opt-in diagnostic PR #3433 ran commit
`c843e1a6da651eed04226e69689f030de6f3604a` in
[dispatch 38714871435](https://github.com/horocore/horo-engine/actions/runs/38714871435).
Both existing Save lifecycle and retention allocation tests timed out at their
unchanged 90-second deadlines. The signed installed Microsoft CDB reader exited
successfully in both cases and captured all four threads, rather than failing to
find a debugger. The test failures remain distinct from diagnostic capture success.

[Stack artifact 11964346580](https://github.com/horocore/horo-engine/actions/runs/38714871435/artifacts/11964346580)
contains the status, exact-source provenance and all-thread frames.
[Binary artifact 11963976934](https://github.com/horocore/horo-engine/actions/runs/38714871435/artifacts/11963976934)
contains the matching executable and PDB. Independent EXE CodeView and PDB MSF
stream inspection confirmed GUID `edca32f2-269a-49b8-953e-93d2711dd7f2`, age 2.
The executable SHA-256 is
`968783150e4f84f6aaa9d931ef60d069361537d86f23e7ac859591ab5e125d67`;
the PDB SHA-256 is
`e9c4c3242fe450873ec1bed78141b1162cf85f12ba4021311b84b74ddb43ca1a`.
Both hashes match both test provenance records. CDB requested exact symbols
(`0x400`) and reported private PDB symbols and lines loaded successfully.

Both main threads show the same relevant sequence:

```text
NtUserWaitMessage / user32 / ucrtbased / VCRUNTIME
AllocationMeter::RecordAllocation / operator new
std::_Container_proxy allocation / std::string default construction
SaveIdentityDetail::FormatUuid / GenerationName
SaveFilesystemStorage::VerifyLifecycleGenerationAbsent
SaveSlotLifecycle::VerifySelected / Cleanup / CleanupAfterPublication
```

The other three threads contain Windows runtime waiting frames. This evidence does
not establish that the separate Package lifecycle or source-operation timeouts
have the same cause.

## Cause and correction

`FormatUuid` previously default-constructed a string before reserving its final
36 bytes. The observed allocation failure occurs in the MSVC Debug iterator
proxy created by that default constructor, before `reserve` executes. The default
constructor is `noexcept`; an allocation exception there cannot reach the
existing `CleanupAfterPublication` catch that preserves publication knowledge.
The CRT reporting path consequently waits for UI on the headless Windows runner.

The primary
[Microsoft STL VS 2022 17.14 source](https://github.com/microsoft/STL/blob/vs-2022-17.14/stl/inc/xstring#L708)
shows the default constructor's conditional `noexcept` and its
`_Construct_empty` call; `_Construct_empty` allocates the iterator proxy. This is
the source family corresponding to the observed compiler 19.44.35229. The runner's
installed STL header hash was not captured, so that source mapping is an inference;
the exact binary/PDB identity and named stack sequence are directly verified.

Formatting now fills a fixed 36-character array, then constructs the owned string
with its throwing pointer-and-length constructor. Canonical lowercase digits,
dash positions and identity bytes remain unchanged. Allocation failure can now
propagate to the existing owner recovery boundary. No public API, serialized
format, caller migration or recovery predicate changes are required.

## Regression and readiness evidence

The direct identity regression measures the real formatting allocation count,
injects one failure at every measured allocation position and checks that
`std::bad_alloc` reaches its caller. It also checks successful one-shot recovery
and the exact canonical text. This includes the MSVC Debug proxy allocation
without exempting allocation sizes, lowering iterator checking or disabling CRT
reporting.

The existing lifecycle and retention tests retain both `GENERATE(false, true)`
variants, their publication/lease/reopen/reconcile assertions and 90-second limit.
Final-head Windows execution of both existing cases is required to demonstrate
recovery on the affected platform. Local macOS execution, when available, is
separate evidence and cannot establish the Windows fix by itself.

## Second matched capture and supported qualification

[Run 38725539810](https://github.com/horocore/horo-engine/actions/runs/38725539810)
built recovery head `35d82a375038c1be0e663821234e44af1be212aa` through merge
`122a1e6f2893d1da45896e7cf2c828dff7eaa067`. Its
[stack artifact 11970117596](https://github.com/horocore/horo-engine/actions/runs/38725539810/artifacts/11970117596)
and [binaries 11969453372](https://github.com/horocore/horo-engine/actions/runs/38725539810/artifacts/11969453372)
match GUID `d1c5d2ee-1f1a-41a2-8f5a-0c645497c732`, age 2. EXE SHA-256 is
`13dbfe00259b8b4de7733b4c718c3abaf8c38d33e0b0e71bf2071b6af7f39372`;
PDB SHA-256 is
`888b9c3df9c83f6d383735097f89ae469d4a045755332aa10ef98c51a4bbca8d`.
Both captures used exact private symbols and CDB exited zero.

Both sync-success variants now return, reopen, reconcile and exit their fixture
scopes. Both sync-failure variants subsequently stop in the injected failure's
`std::string` construction inside `ErrorCode::ErrorCode` / `Error::Error`, reached
from the fixture's `Failure(std::move(syncError))` transport. `FormatUuid` is absent
from these stacks. This verifies the formatter correction's recovery edge; it
does not demonstrate complete Debug qualification or a production cleanup lock.
The precise STL move/noexcept mapping remains a source-family inference because
the installed runner header hash was not captured.

MSVC with iterator debugging cannot faithfully exercise this global allocation
probe through the existing Error transport. The two cases explicitly `SKIP` in
that configuration, before their generators or fixture allocations. Pinned Catch
returns code 4 for an entirely skipped case, and its CMake discovery registers
`SKIP_RETURN_CODE 4`, preserving an actual CTest/JUnit skip. No skip counts as a
passing recovery test. Debug formatter, provider and direct I/O tests still run.

Windows Release qualification builds the same lifecycle test target and runs
exactly the two existing cases with the unchanged 90-second timeout. Their complete
bodies, `GENERATE(false, true)`, assertions and allocation predicates remain intact
in supported configurations. The mandatory JUnit verifier rejects missing cases,
skips, failures and errors, and requires ordered markers for both sync variants,
reopen/reconcile and both completed fixture scopes. Fresh-head Windows Release
evidence is required before claiming these allocation cases qualified. Temporary
recovery-branch Debug capture gates are retired; the original diagnostic branch
gates and historical artifacts remain available.

The exact `fef58df4` Windows qualification ran both Release cases successfully,
but CTest truncated each passed case's JUnit output at its default 1024-byte
limit. The failure-variant reopen/reconcile and fixture-exit markers were cut,
so the strict verifier correctly rejected incomplete evidence. The dedicated
two-case command now retains at most 16384 bytes per passed case. The owning
CTest fixture reproduces default truncation with a late marker and verifies
that the bounded larger capture retains it from the same spaced test directory.
This changes evidence capture only; the verifier, allocation probes, generator
variants, assertions and deadlines remain unchanged. Full Windows variant proof
still requires the new head's actual untruncated JUnit to pass verification.
