# ACC-001 delivery and qualification

This record distinguishes implemented contracts from integrated product acceptance.
The initiative remains incomplete. Headless semantics do not qualify native
assistive technology; compilation does not prove visible output or GPU behavior.

Parent: HORO-1769 / #1813 [ACC-001]. The published target is
`codex/editor-improvements` at `139f8a5ed63ca8698bb3852c9e0cb1eff421dc8d`,
verified and fetched on 2026-11-13. The source snapshots below still descend from
the earlier `3df476ff430a2ad28cd62a8a0c1780ae0fea71bc` base; they do not prove
integration with the newly published target. No merge/rebase or history rewrite
is part of that ref inspection. The initial implementation baseline was
`f51e0c0f44cd0f4c7931d288a5dde794e3316dfd`.
The reviewable delivery is [draft PR #3560](https://github.com/horocore/horo-engine/pull/3560).
Local verification below includes commits beyond its published `55d9991` head;
it must not be attributed to that older remote snapshot.

## Current acceptance matrix

| Child | Jira / GitHub | Implemented and verified | Remaining acceptance |
|---|---|---|---|
| ACC-001.2 captions | HORO-1806 / #1850 | Fixed cue/catalog transport, bounded timeline, appearance, safe-area layout and canonical paint/resource lifetimes; 49-case UI batch passed | Host localization/shaping/presentation composition, settings activation and visible native output |
| ACC-001.3 color | HORO-1807 / #1851 | Foundation-only matrices and authoritative snapshot query; numeric/query/contract cases passed in the 11-case semantic batch | Complete-image GPU execution, cooked shader ABI migration, settings activation and real backend evidence |
| ACC-001.4 input | HORO-1808 / #1852 | Canonical binding policy/codec, router lifetimes, calibrated platform gyro and atomic persistence; **99/99 Input** passed on the exact patch committed as 80f09f5, including chord-set identity and finite-scale cancellation. **98/98 GUI/SDL** and the editor application built at clean 80f09f5; direct Input consumers passed **62/62** at d460d27 | Scoped persistence/capture integration with INP-001.6 / HORO-1795 / #1839; controller motion collection, device evidence and native interaction qualification |
| ACC-001.5 visual | HORO-1809 / #1853 | Atomic UI scale/contrast projection, readable typography floor and tagged safety projection passed in UI/semantic batches | Both editor theme layers, coherent DPI/font/UI scale, actual Camera/VFX/animation consumers and visual qualification |
| ACC-001.6 bridge | HORO-1810 / #1854 | Runtime UI semantics, truthful Null Unsupported bridge, retained desired preference and bounded opt-in QA journal passed in the UI batch | Native adapters, default shared-control metadata, bypass detection, QA review UI and real assistive-technology qualification |
| ACC-001.7 assists | HORO-1811 / #1855 | Inert contribution, seven typed preferences, authoritative snapshot and revision-only hint/retry contracts passed in the semantic batch | Host tick composition, game-owned supported/authorized consumers and shared settings contribution |
| ACC-001.8 tooling | HORO-1812 / #1856 | Numeric contrast/public consumer passed. Live theme-pair inspection, built-in opaque primary text, paired-locale narrow/wide render and Input Mapping draft tests passed in GUI batch. CI selection and 55-criterion inventory are present | Complete all four tools, native interaction checks and observed applicability/qualification matrix; depends on preceding children |

No parent/child issue state or ADR approval state was changed by this delivery.

## Native verification

Each batch used its exact recorded source snapshot, native MSVC 19.44 Debug with
embedded debug information, the verified canonical physical OS lock, nonblocking
admission and CPU/RAM/storage checks. Builds and CTest ran with one worker.
The lock was released between batches. Discovery required complete Catch
`Target::case` families and explicit consumers; `--no-tests=error` prevented an
empty selection from passing. Reports are local ignored artifacts, not source.

| Scope | Source commit | Built targets | Executed evidence |
|---|---|---|---|
| Input controller core | `ec59e05e9` | HoroInputAccessibilityTests, HoroInputTests, HoroInputPublicHeaderConsumer, HoroInputAccessibilityPublicHeaderConsumer | **102/102 passed, zero skipped**, 1.57s; `build/acc-controller-core-ec59e05-results.xml` and corresponding discovery JSON. Later source changes require a fresh run. |
| Foundation-only semantic contracts | `6308740ffc1b7167f2ffc7ef98044b1ffbe02837` | Visual/Gameplay accessibility test targets, both staged header targets, Visual/Gameplay/Contrast direct contract consumers | **11/11 passed, zero skipped**, 0.24s; `build/acc-semantic-results.xml` and discovery JSON |
| Direct Input consumers | `d460d270be3a71c914c001d5302674c2f599672e` | Eleven targets: Character/Input UI/debugger/portable Android tests; staged Character/RuntimeUiInput/Android/XR consumers; direct navigation/pointer/debugger consumers | **62/62 passed, zero skipped**, 1.42s; `build/acc-input-consumers-d460d27-results.xml` and corresponding discovery JSON |
| Captions / Runtime UI / optional bridge | `0b1f6eb7e223b0ece0d6137bf85a617be066d9d0` | HoroRuntimeUiAccessibilityTests, RuntimeUi staged/accessibility consumers, CaptionTransport staged/direct consumers, PlatformAccessibility staged/direct consumers | **49/49 passed, zero skipped**, 0.72s; `build/acc-caption-completion-0b1f6eb-results.xml` and corresponding discovery JSON |
| GUI / SDL / editor application | `80f09f52b5f94f69b87c0bc9ed921e99540c278b` | HoroEditorPresentationTests, HoroGuiPublicHeaderConsumer, HoroInputSdlTests, **HoroEditor** | **98/98 passed, zero skipped**, 53.66s: 88 presentation and 10 SDL cases; `build/acc-gui-overflow-80f09f5-results.xml` and corresponding discovery JSON |

All batches configured successfully, including header ownership and dependency
direction checks. Affected staged/public consumers compiled. The GUI batch enabled
GUI, SDL input and OpenGL, compiled the actual editor input-cutoff implementation,
linked `HoroEditor.exe` and staged its runtime assets. It did not launch the editor
or enable GPU smoke tests. Its presentation tests are display-independent ImGui
render/model evidence, not observed native interaction or display output.

The initial controller core adds explicitly selected device-owned SI samples,
ordinary player filtering, independent transition consumption, source/device
evidence, strict optional profile source fields and stale-handle rejection.
Its three new regressions and the direct public consumer executed at `ec59e05`.
The later `Gamepad` source spelling, zero-threshold neutrality and fixed-tick
capture regression are not covered by that older compile. SDL opt-in/window-focus
collection, seven SDL virtual-device regression sources, editor source controls
and affected direct consumers still need native execution at the final snapshot.
The old GUI/direct-consumer rows do not qualify the changed Input layouts or
controller adapter. Physical controller and native interaction checks remain.

The direct-consumer batch refreshed callers after the public binding layout
change: Character **16**, Runtime UI input **19**, input debugger **15**, portable
Android input **9**, and three direct consumer executables. All four staged
consumer targets also compiled. This supersedes the earlier Character-only pass
for current input integration; the Android cases are portable producer/lifecycle
tests, not NDK/device qualification. Offline SDK compatibility repeated at
`d460d27` with **24/24 passed** (0.22 seconds), independently of these C++ tests.

The internal chord-set identity correction at `4a7ae9b` rejects reordered or
repeated-key aliases through both map and profile admission. Its new regression
executed successfully in the 96-case Input batch at `eac1e8f`, including unchanged
map/profile revision on rejection and preservation of a genuinely different chord.

The later finite-scale correction at `80f09f5` keeps products and binding totals
in double precision until physical and bounded collected sources have joined,
then applies the existing final saturation and radial filtering. Before the fix,
the new regression observed +/-1 instead of +/-0.25: **96 passed, one failed**;
`build/acc-overflow-4d4faa5-red-results.xml` retains that failure. The first green
build attempt found a test-only C2662 ownership error, corrected by moving the
collection frame out of its Result; no test pass is attributed to that attempt.
The final three-file frozen patch passed all 99 cases before commit. Its SHA256
receipt was checked against the committed source; no executable code changed
between that run and commit. New cases cover sum/product overflow, both axis
components, radial filtering, allocation-free collection merging and digital
edges. The subsequent GUI batch ran at the clean commit and relinked the actual
editor with both this fix and the announcement-completion bridge contract.

The GUI batch executed all ten new Contrast/Input Mapping cases: exact duration
round trips, malformed drafts, canonical Apply with peer preservation and stale
revision rejection, sticky-conflict rejection, valid/invalid localized narrow
forms, live theme-pair selection, opaque composition validation, paired-locale
narrow/wide contrast rendering, and built-in primary-text contrast across both
theme layers. These do not prove every theme role or UI state is accessible.

Input regression evidence includes bounded mapping capacity and rollback,
allocation-free reads, held/toggle/repeat/sticky semantics, context/device/player
retirement, monotonic host time and cancellation provenance. Character tests
include repeat across zero/catch-up ticks and clock failure clearing old intent.
UI evidence includes SPSC concurrency/no callback allocation, catalog/text lifetime,
replay/session bounds, safe areas, immutable paint/resource ownership, reading and
focus order, retained announcements, QA journal identity/privacy and Null behavior.

### Failures retained and corrected

At `b9d39cc`, Input compiled but the new test source failed because Catch2 could
not decompose `REQUIRE(a && b)`. Commit `20a19f6` split the assertions; no test
execution was claimed for that first attempt.

At `e3afd1f`, all four Input targets built and 87 cases ran: **86 passed, one
failed**. The deferred-release fast path returned `Resolved` after missing host
time, losing the original `ClockUnavailable` cause. Commits `f7129c7` and
`49a1780` retain typed cancellation provenance with the old mapping, including an
unread physical release and a removed action map. The subsequent 88/88 batch
validated the fix. Original failure output/discovery remain in
`build/acc-input-e3afd-failure.log` and `build/acc-input-e3afd-discovery.json`.

The early MSVC C1902 compiler-probe failure was superseded by native embedded-debug
builds above. An earlier cache-test repeat failed with ENOSPC; the later exact
retry passed. Neither failed attempt is counted as successful verification.

## Python and quality evidence

The paired accessibility-copy, offline SDK and CI grouping suites passed with
**66 tests** after published-base integration (`build/acc-current-python.xml`).
After adding eleven theme pairs, the affected paired-copy/CI suites passed again
with **42 tests**. The independent offline SDK suite passed **24 tests** with a
short temporary root. These cover catalog/metadata contracts, not C++ behavior.

The final cache/runner retry passed **107 tests, four skipped**
(`build/acc-cache-python.xml`). Three skips require Windows symlink privileges;
one requires a real compiler/ccache fixture. An earlier same-scope ENOSPC run is
superseded only for that exact Python scope. Formatting and `git diff --check`
were checked after source edits.

Full final-base Sonar IDE, CLI secrets and Codacy results remain required and must
be reported separately in PR validation, including findings, errors, skipped files,
C++ sensor execution, submitted-file scope, freshness and the official report path.
An empty issue list or exit 2 is not a pass. The supported command is:

```sh
python scripts/analyze.py check --worktree . --base origin/codex/editor-improvements --timeout 900 --code-user-data <existing-portable-profile>
```

Use the initialized compiler environment, verified own compile database and the
same physical lock/resource guards. The current published preparation path builds
with one worker. Earlier two-worker preparation and named-main-grant restrictions
are historical and do not block this version. A busy lock defers the batch without
a waiter; it does not prohibit independent own-worktree edits. Yield between
bounded batches to other ready owners.

The 2026-11-13 doctor found local prerequisites ready: CMake 4.3.1, Ninja 1.13.2,
VS Code 1.145.3, Sonar CLI 1.10 and Codacy 0.25. Its independent Codacy cloud
configuration comparison remained incomplete (exit 2/authentication). The official
external-cache report basename is `1794573359424626900-cddeab7f.json`. Readiness
is not connected-mode authentication, Workspace Trust or C++ sensor evidence.
No credentials, trust policy, rules or cloud configuration were changed.
`build/acc-preflight-admission-20261113.json` records admission attempts only;
it is explicitly **not an analyzer report** and cannot establish provider success.

A full official attempt at `4a7ae9b` was admitted with the verified physical
handle, CPU 4% and available RAM 35%. It was deliberately cancelled after 1,226
seconds during header-context preparation. A stack-only 97-sample capture located
89 samples in Windows path canonicalization at `scripts/analyze.py:2225`;
`build/acc-preflight-preparation-diagnostic.json` retains that diagnostic. All
three owned processes were confirmed missing after cancellation. No official
report was written and no provider pass is inferred. The cancellation record is
`build/acc-preflight-cancelled-4a7ae9b.json`, not an analyzer result.

Include search now probes missing/non-directory candidates before canonicalizing
existing paths. Search order, symlink validation, staged-header content checks and
native-build/sensor requirements remain in place. The helper regression suite
passed **54 tests, one skipped**
(`build/acc-quality-header-probe-results.xml`). The skip requires Windows symlink
creation privilege. These unit results do not replace the required full-scope
rerun or establish any C++ sensor result.
The separate Windows compiler/CLI and OS-adapter suites passed **68 tests** in
10.35 seconds (`build/acc-quality-platform-results.xml`).

The subsequent full attempt at `eac1e8f` completed the 59 translation-unit and
27 header contexts, including CMake/object preparation, then reached managed IDE
project synchronization. The source VS Code profile had no `horocore` connection;
the managed log explicitly reported that the connection ID did not exist. The
check was stopped under the working contract, and its owned processes and managed
window retired. `build/acc-preflight-connection-incomplete-eac1e8f.json` records
this incomplete attempt; it is not an official analyzer report or C++ sensor pass.
The existing VS Code connection needs configuration and the next managed window
needs review. No credentials or Workspace Trust settings were changed.

Independent checks at that same clean commit used the complete final-base
106-file selection and verified source freshness. The supported CLI secrets
function (`sonar analyze secrets`) returned exit 0 with no findings;
`build/acc-cli-secrets-eac1e8f.json` is explicitly a secrets-only provider result.
The official `check --only codacy --base origin/codex/editor-improvements
--timeout 900` returned **incomplete / exit 2**, not stale. Its report is
`1794594068440665100-2c9a2a1f.json` in the external report cache, retained locally
as `build/acc-codacy-eac1e8f.json`. Neither partial check replaces full preflight.

That Codacy snapshot contains **85 findings: 62 Info and 23 Warning**. All findings,
including contextual ones, remain in the report without claims about their age.
It also records two Semgrep partial C++ parses, seven skipped large files and
missing/incomplete Jackson execution. Six CMake paths have no configured local
analyzer. Raw complexity and CPD completed as advisory estimates, with 59 clone
groups and their unchanged partners retained. Markdown markup and redundant test
imports have been corrected; a same-scope rerun is required after those edits.
No rules, exclusions or cloud configuration were changed to hide incomplete work.

For published head `55d9991`,
[SonarCloud run 43427932437](https://github.com/horocore/horo-engine/actions/runs/43427932437)
failed during instrumented Debug configuration: Lua FetchContent download returned
HTTP 500. Coverage build/tests and analysis did not execute; the later Sonar job
correctly refused missing coverage/native inputs. Authentication had passed its
earlier check, so this is not reported as a missing-token diagnosis or a quality
gate result. The inspected local log is
`build/acc-sonar-43427932437-configure.log`. Main CI and Android were still queued
at the same observation; no replacement push or run cancellation was performed.

Windows interaction automation initialization failed before any UI action with
`failed to write kernel assets: The system cannot find the path specified.
(os error 3)`. Manual hover, active, disabled, keyboard focus, popup and native
narrow/localized interaction remain unverified. No GPU smoke, gyro hardware,
Windows UIA, macOS accessibility or Linux AT-SPI interoperability pass is claimed.

## Ownership and integration boundaries

### Captions

`HoroCaptionTransport` is header-only and independent of UI, Audio, renderer and
Foundation. Its fixed cue payload is semantic and session-qualified. The UI-owned
SPSC queue pins a catalog before the producer starts; its control owner stops and
joins the producer before destruction. Overflow drops newest and increments a
saturating diagnostic. Timeline leases survive dequeue and use a bounded 64-sequence
replay window; session generations cannot be reused after cancellation or backwards
time. Audio observations never replace text or restart an accepted cue.

Runtime UI owns arrangement/settings and canonical `CaptionRenderer` extraction.
The host must supply existing retained elements, actually localized/shaped glyphs,
font/resource generations, exact catalog/appearance and the locale-snapshot
commitment. Animated hosts forward their real canonical frame lease. Extraction
creates no alternate UI tree, resolver or fabricated presentation receipt.
Host text preparation and actual composition/presentation are still missing.

### Color and visual safety

`HoroVisualAccessibility` depends only on Foundation. It captures authoritative
configuration independently of a renderer. Full-severity matrices use the Machado
reference, bounded row-major coefficients and identity interpolation. Numeric wire
encodings are explicit: severity/strengths use thousandths and matrix coefficients
use millionths. UI alone owns high contrast; color queries do not register a second
key. Full-strength matrices follow the
[Machado/Oliveira/Fernandes model](https://profs.ic.uff.br/~laffernandes/content/publications/journal/2009_tvcg_15(6)/machado_oliveira_fernandes-tvcg-15(6)-2009-corrected.pdf),
cross-checked against the pinned
[Colour v0.4.7 dataset](https://github.com/colour-science/colour/blob/v0.4.7/colour/blindness/datasets/machado2010.py).
Strength interpolates from identity; it does not model physiological severity or
claim to restore color perception. The matrix space is display-linear sRGB;
HDR/other primaries require explicit renderer space adaptation.

The cold `ColorPipelinePlan` provides admission metadata, not execution. The real
editor viewport owner is `EditorViewportColorPipeline`; its programs/materials and
`MetalViewportClusteredFrame::AppendColorPasses` surround display-linear overlays
with actual scene and SDR encode work. The current final shader has only
`OutputControl`, no accessibility matrix. Its canonical recipe is Metal-only.
A new encoder ABI requires genuine cooking/reflection/package outputs, preserving
original locked captures and deferred resource retirement. Required ordering is
scene/exposure/look/ACES, display-linear UI, complete-composite accessibility,
containment/dither/one encoding, then present. Viewport evidence alone would not
qualify whole-editor GUI composition or other backends.

UI owns `accessibility.visual.ui.*`; safety owns
`accessibility.visual.safety.*`. Snapshot projection enforces a 16-DIP typography
floor but does not publish a font atlas or coherent DPI layout. Existing
`Theme::SetUiScalePercent` remains a no-op pending that integration. Camera, VFX
and animation must consume policy at their actual owner boundaries; numeric
suppression helpers alone do not implement those behaviors.

### Input

The canonical typed binding/profile path owns digital affordance policy, conflict
validation and v1-neutral/v2 persistence. The router owns bounded per-incarnation
interpretation and scoped withdrawals; it never changes raw facts or uses frame
numbers as elapsed time. The actual editor drains ended-context records at its
input cutoff without dispatching dead callbacks or retargeting releases.
Gameplay and Character tick captures preserve repeats and clear held/pending
intent on unavailable clocks/capacity while retaining the typed cause.

The existing Input Mapping tool uses shared controls, paired localization, exact
microsecond-preserving decimal millisecond drafts and canonical Apply/Cancel.
Configuration revision plus the complete original binding fences stale drafts.
The [binding migration guide](input-accessibility-binding-migration.md) records
API/layout changes, limits and rebuild requirements. Integration with
[Input PR #3550](https://github.com/horocore/horo-engine/pull/3550) must preserve
both scoped persistence/capture and the accessibility policy; it is not delivered
by these independent passing tests. Gyro still needs interpretation and hardware.

Subsequent source work adds platform GyroscopeAxis rate bindings through the same
router/profile owner, with typed SI calibration, ordinary sensitivity/deadzone,
availability/focus neutralization and physical-axis conflict validation. The
migration guide records its public layout/wire additions. The native batch at
`f1f97b7` built all four Input targets and passed **94/94 cases**, including all six
new gyro regressions, the extended explicit consumer and atomic save/load with
spaces/non-ASCII paths. It supersedes the earlier 88-case Input scope without
claiming gyro hardware or GUI behavior. Subsequent source adds owned Add/Edit intents
and an exclusive gyro modal through the existing workspace command and modal
hosts. Axis/calibration/sensitivity drafts apply only through canonical SetProfile;
all six model/modal/render cases passed in the **98/98 GUI/SDL** batch at
`8212db8`, along with the actual editor and staged GUI header builds. Paired-copy/CI
Python checks passed **43 tests** after those UI edits. Automatic motion-gesture capture,
SDL controller motion collection, native interaction and device qualification
remain open.

Post-batch source review found that inactive binding fields could disguise the
same physical control from duplicate/chord checks. Commit `830ac15` selects
physical identity by the active kind for all nine controls. Commit `2b2175c`
retains analog axis exclusivity with chords while admitting mutually exclusive
ordinary modifier modes. Commit `a6174d8` shares one bounded gyro draft parser
between frame validity and explicit error-producing Apply, and strengthens the
modal-gating precondition. Both native reruns used clean `2b2175c`: **95/95 Input**
and **98/98 GUI/SDL passed, zero skipped**. The all-kind duplicate/sticky/chord
regression, strengthened modal-gating check and actual editor/public consumers
ran successfully. Paired-copy/CI Python checks repeated with **43 passed** in
0.17 seconds. These results do not establish hardware or native interaction.

### Native bridge, editor semantics and gameplay

`HoroPlatformAccessibility` consumes existing Runtime UI semantics without making
baseline Platform depend on UI. Its public surface exposes no native handles.
Only Null currently exists: Unsupported, zero queues/schema, no effects or
recording, and ShuttingDown after shutdown. Desired preference remains distinct
from actual capability. Runtime UI retains focus/action authority and native
visibility requires actual matching presentation evidence.

The bridge now declares a separate announcement-completion cursor channel so
queue admission cannot silently acknowledge the UI publisher. Null produces no
completion, including after shutdown. The [transport migration](platform-accessibility-bridge-migration.md)
records the vtable rebuild, exact publisher ownership and regression scope.
Its Null/base-interface regression and seven affected targets passed in the
49-case native batch at `0b1f6eb`; real asynchronous adapters and host draining
remain pending.

`UiAccessibilityReviewLog` is an independent opt-in 64-record journal, not a speech
queue or native adapter. Text recording requires separate consent, oversized text
is omitted whole and policy changes scrub old rows. It consumes the exact adopted
publisher storage lease; equal numeric revisions are insufficient. It never
acknowledges speech. Extension forms already export accessible metadata; that
does not supply default metadata for all built-in shared controls.

`HoroGameplayAccessibility` is Foundation-only. Each tick must capture the
configuration authority regardless of dropped/stale revision hints. The adapter
publishes only `ConfigurationRevision`; preferences never become bus state or a
server grant. Current `GameRuntimeContext` has no accessibility tick binding and
`ConfigurationService::Snapshot` takes a mutex. An explicit SDK migration,
nonblocking tick-start composition and game-owned authorized consumers remain
required; no ambient lookup, alternate resolver or event-state cache was added.

All domain contributions are inert. Current Foundation descriptors lack domain
range validation before publication. The published CFG
[draft PR #3556](https://github.com/horocore/horo-engine/pull/3556), head
`e7b809b3326f493d6642aa4e5e4600fe4d003b2a` inspected on 2026-11-13, introduces
canonical descriptor validation and a shared settings session. Host activation must
compose that authority and validate every candidate before commit; rejecting an
already committed value in a typed consumer is insufficient. That integration has
not been applied here, and no sibling worktree was edited.

## Decision audit and qualification limits

[ACC-001.1 / #1849](https://github.com/horocore/horo-engine/issues/1849) is a closed
ownership decision, not completion of the seven runtime children. Its body retains
unchecked boxes and older Audio/PostProcessing wording. Normative accessibility
architecture controls implementation. ADR-015 still says Proposed; this audit
does not change its approval status or reimplement the decision. Its ownership,
inert contributions, independent targets and revision-only transport are reflected
in this change, while actual host activation remains open.

The [editor qualification inventory](accessibility-editor-qualification.md) covers
all 55 exact WCAG 2.2 A/AA identifiers and levels, checked against the official
Recommendation. No criterion is passed or silently marked not applicable.
WCAG2ICT frames native-software interpretation; CVAA/EAA/TRC notes are scoped
qualification goals, not legal certification. The Commission overview did not
establish article-level conclusions when EUR-Lex required verification.

Contrast evaluates actual opaque composed sRGB colors without rounding borderline
results. Unknown composition produces no pass. Eleven explicit live theme pairs
cover token primary/muted/dim and native text/disabled roles; arbitrary opaque
custom colors remain available. Disabled-text results are diagnostic because
[WCAG 2.2 contrast criteria](https://www.w3.org/TR/WCAG22/#contrast-minimum) exempt
inactive components. The tool cannot infer applicability from a color role or
certify the complete editor.

CI selection includes numeric contrast and public consumers in focused Windows
Debug. GUI-linked theme cases belong to GUI-enabled Linux/macOS Debug, and were
also executed locally in the Windows GUI batch above. Selection changes and
headless output do not qualify GPU, native AT, all interactive states or every
supported host composition. Final acceptance requires their actual observed
evidence and complete final-base quality results.
