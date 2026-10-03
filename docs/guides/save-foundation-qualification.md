# Runtime Save Foundation Qualification (HORO-1402)

This qualification covers the implemented foundation contracts: participant
registry and capture, safe-point lifecycle fencing, and asynchronous operation
completion. It does not claim a complete `RuntimeSaveService` or durable archive
pipeline. The host-level service in
[Save Game and Persistence Architecture](../architecture/runtime/save-game-and-persistence.md)
is still schematic.

## Regression coverage

- Hostile optional participants returning an invalid disposition cannot be
  silently omitted. Rejected immutable payloads are released and prior writes
  are rolled back.
- Capture plan and sealed participant projection are stable across registration
  order. Existing registry tests cover required/optional dependencies, cycles,
  missing adapters, duplicate ownership, and bounded admission.
- A headless safe-point fixture detaches borrowed gameplay bytes, replaces the
  scene, closes the registry, and completes on a worker while checking that no
  live borrow or stale callback survives. The adapter lease retires with the
  snapshot.
- Concurrent completions reject duplicate terminal publication. Restore
  completion racing a scene transition cannot apply stale state. Shutdown
  racing detached completion retains exactly one immutable terminal outcome.
  Existing operation tests cover cancellation before/after the commit gate,
  callback delivery, exception containment, and abandonment.

## Local evidence

On Linux x86_64 (Ubuntu kernel 7.0.0-31-generic, AMD Ryzen 7 9800X3D), using
GCC 15.2.0 and CMake `Release` mode:

```bash
cmake -S . -B build/skeleton -DBUILD_TESTING=ON
cmake --build build/skeleton --target HoroRuntimeSaveCaptureSnapshotTests HoroRuntimeSaveSafePointCoordinatorTests HoroRuntimeSaveOperationTests --parallel 2
ctest --test-dir build/skeleton -R 'HoroRuntimeSave(CaptureSnapshot|SafePointCoordinator|Operation)Tests' --output-on-failure
build/skeleton/tests/HoroRuntimeSaveSafePointCoordinatorTests 'Representative gameplay capture handoff evidence'
```

The three C++ targets built and 57/57 CTest cases passed. The seven new
qualification cases were present in that CTest selection. CMake reported
`pytest` unavailable, so repository Python tests were not configured.

The opt-in handoff fixture prebuilds three immutable, concurrently readable
roots representing 2,048 scene entities at 32 bytes each, 2,048 physics motion
states at 64 bytes each, and 256 player inventory entries at 32 bytes each:
200 KiB total. It measures owner-thread builder creation, participant callback
handoff, admission, canonical ordering and sealing over 200 iterations after
one warm-up. The 2 ms p95 budget applies to this handoff only; root preparation,
serialization and storage are outside the timed region. Measured p50 was 1 µs,
p95 was 2 µs, and maximum was 3 µs. These are local release-build observations,
not a cross-platform performance guarantee.
