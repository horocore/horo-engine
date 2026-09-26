# Cinematic playback lifecycle qualification (CIN-002.9)

This is headless runtime-service qualification for the playback contracts layered
through PRs #3020, #3024 and #3028. It does not qualify editor or packaged-host
composition, which is tracked separately in #1723.

## Workload and assertions

`HoroCinematicRuntimeTests` covers a player destroyed while playing, shutdown and
replacement-session handle fencing, and cancellation at an owner boundary. The
restore matrix uses two captured targets: both alive, one destroyed, one replaced
at a new generation, and one changed at the same generation with a newer owner
revision. Missing or stale targets must prevent *every* restore write and retain
the surviving target's final value. Exact diagnostic outcomes are checked.

The deterministic soak runs 512 generations of one player through admission,
play, event delivery, cancellation and release in one service. Each cycle asserts
one delivered occurrence, no later delivery from the cancelled player, zero
allocations during frame evaluation, exactly two acquired and released owner
leases, zero retained budget and zero active players after release, and rejection
of the retired generation. This detects leaked service reservations and leases;
it is not a process-wide heap-leak detector. A sanitizer run is needed for a
stronger heap lifetime claim.

## Platform evidence and limits

The tests have no graphics, window, OS, editor or packaged-game dependency and
are intended to run as headless C++ tests on each supported build host. Local
evidence is limited to the platform and configuration recorded below. Linux
results cannot establish macOS or Windows pass status. No GPU smoke test or host
integration is claimed by this qualification.

| Host | Build and run | Result |
| --- | --- | --- |
| Linux x86_64, GCC 15.2.0 | CMake configured with `BUILD_TESTING=ON` and ccache compiler launchers; `cmake --build build/skeleton --target HoroCinematicRuntimeTests --parallel 2`; focused Catch2 filter; `ctest --test-dir build/skeleton -R 'HoroCinematicRuntimeTests::' --output-on-failure` | Build passed; focused 3 cases / 8,788 assertions passed; full runtime suite 58/58 passed |
| macOS | Same headless target and CTest selection | Not run locally |
| Windows | Same headless target and CTest selection | Not run locally |

The frame allocation assertion uses the test executable's allocation probe.
Address/LeakSanitizer was not run locally; process-wide heap leak freedom is
therefore unqualified. The Cinematic Model test target and the repository-wide
suite were not run in this worktree.
Owner adapter callbacks in this scope are synchronous; asynchronous provider
completion and actual scene-component binding are outside the implemented seam
and require the host integration tracked in #1723.
