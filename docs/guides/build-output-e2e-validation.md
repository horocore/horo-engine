# Build Output end-to-end validation

Use this checklist for EDT-004B.9 after the Build Output producer and panel
changes are applied together. Record the editor build, platform, renderer, UI
scale, locale, and result for each run. A checked item requires an observed
result; the automated tests alone do not qualify the visual cases.

## Automated coverage

Run the following from a configured worktree:

```bash
cmake --build build/skeleton --target HoroGameplayBuildServiceTests HoroAssetCookServiceTests HoroGlobalDockPanelRenderTests HoroEditorWorkspaceControllerTests --parallel
ctest --test-dir build/skeleton -R 'Horo(GameplayBuildService|AssetCookService|GlobalDockPanelRender|EditorWorkspaceController)Tests' --output-on-failure
```

The gameplay test builds a real module, validates the published artifact,
reuses a cached success, then compiles invalid source. It checks that the
resulting typed error identifies the source file and its line and column.
The service tests also exercise active cancellation, timeout, shutdown, and
one terminal result. The asset-cook tests exercise successful, failed, cached,
and cancelled results. Panel and controller tests cover projection, detach,
source command generation, and source-path validation.

## Editor interaction checklist

- [ ] Build valid gameplay source. Confirm configure, build, validation, and one
      successful terminal record appear in Build Output; enter Play and confirm
      the new module loads.
- [ ] Introduce a syntax error in a gameplay source file and rebuild. Click the
      error row; confirm the editor opens the correct file at its reported line
      and column. Confirm a source outside the project cannot be opened through
      this action.
- [ ] Start a slow configure, close the Build Output panel, reopen it while the
      process runs, then cancel. Confirm the same session and records remain,
      cancellation completes, and no child process continues after shutdown.
- [ ] Repeat cancellation during the build phase. Trigger a configure timeout
      and a build timeout separately. Confirm each session has exactly one
      terminal result and its error context remains visible after reopening.
- [ ] Disable observability/telemetry, then repeat a successful and failed
      build. Confirm Build Output and cancellation remain functional.
- [ ] Cook an asset successfully, cook it again to observe a cache hit, and
      trigger a cook failure. Confirm each result has the correct asset,
      diagnostic severity, and navigable source when one exists.
- [ ] At normal and high DPI scales, inspect the panel at wide and narrow dock
      widths in English and Turkish. Check long paths, clipped text tooltips,
      toolbar access, selected rows, filter controls, and action visibility.
- [ ] Scroll away from the tail while new records arrive, then return to the
      tail. Confirm new records do not steal the earlier scroll position and
      resume following when at the tail.
- [ ] Clear the visible output during a build, then append another record.
      Confirm old rows stay hidden, new rows appear, and the underlying
      operation history remains available.

Attach screenshots or a short recording for the narrow-width and high-DPI
checks, and record any failing session ID plus the corresponding Build Output
and Operation snapshots. Do not close EDT-004B.9 or its parent based only on
this unchecked checklist.
