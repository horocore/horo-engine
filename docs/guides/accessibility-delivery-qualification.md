# ACC-001 delivery and qualification

This record distinguishes implemented contracts from integrated product acceptance.
The initiative remains incomplete. Headless semantics do not qualify native
assistive technology; compilation does not prove visible output or GPU behavior.

Parent: HORO-1769 / #1813 [ACC-001]. The published target is
`codex/editor-improvements` at `3df476ff430a2ad28cd62a8a0c1780ae0fea71bc`,
verified on 2026-11-13. The initial implementation baseline was
`f51e0c0f44cd0f4c7931d288a5dde794e3316dfd`.
The reviewable delivery is [draft PR #3560](https://github.com/horocore/horo-engine/pull/3560).
Local verification below includes commits beyond its published `55d9991` head;
it must not be attributed to that older remote snapshot.

## Current acceptance matrix

| Child | Jira / GitHub | Implemented and verified | Remaining acceptance |
|---|---|---|---|
| ACC-001.2 captions | HORO-1806 / #1850 | Fixed cue/catalog transport, bounded timeline, appearance, safe-area layout and canonical paint/resource lifetimes; 49-case UI batch passed | Host localization/shaping/presentation composition, settings activation and visible native output |
| ACC-001.3 color | HORO-1807 / #1851 | Foundation-only matrices and authoritative snapshot query; numeric/query/contract cases passed in the 11-case semantic batch | Complete-image GPU execution, cooked shader ABI migration, settings activation and real backend evidence |
| ACC-001.4 input | HORO-1808 / #1852 | Canonical binding policy/codec, router lifetimes, repeat/clock failure propagation; Input 88/88 and CharacterInput 16/16 passed. GUI drafts, SDL sample time and actual editor application compiled; GUI/SDL 92/92 passed | Scoped persistence/capture integration with INP-001.6 / HORO-1795 / #1839; gyro interpretation and device evidence; native interaction qualification |
| ACC-001.5 visual | HORO-1809 / #1853 | Atomic UI scale/contrast projection, readable typography floor and tagged safety projection passed in UI/semantic batches | Both editor theme layers, coherent DPI/font/UI scale, actual Camera/VFX/animation consumers and visual qualification |
| ACC-001.6 bridge | HORO-1810 / #1854 | Runtime UI semantics, truthful Null Unsupported bridge, retained desired preference and bounded opt-in QA journal passed in the UI batch | Native adapters, default shared-control metadata, bypass detection, QA review UI and real assistive-technology qualification |
| ACC-001.7 assists | HORO-1811 / #1855 | Inert contribution, seven typed preferences, authoritative snapshot and revision-only hint/retry contracts passed in the semantic batch | Host tick composition, game-owned supported/authorized consumers and shared settings contribution |
| ACC-001.8 tooling | HORO-1812 / #1856 | Numeric contrast/public consumer passed. Live theme-pair inspection, built-in opaque primary text, paired-locale narrow/wide render and Input Mapping draft tests passed in GUI batch. CI selection and 55-criterion inventory are present | Complete all four tools, native interaction checks and observed applicability/qualification matrix; depends on preceding children |

No parent/child issue state or ADR approval state was changed by this delivery.

## Native verification

Each batch used its exact clean source commit, native MSVC 19.44 Debug with
embedded debug information, the verified canonical physical OS lock, nonblocking
admission and CPU/RAM/storage checks. Builds and CTest ran with one worker.
The lock was released between batches. Discovery required complete Catch
`Target::case` families and explicit consumers; `--no-tests=error` prevented an
empty selection from passing. Reports are local ignored artifacts, not source.

| Scope | Clean commit | Built targets | Executed evidence |
|---|---|---|---|
| Input | `dcf772e37900d8a9ca87d4bcb2a55942ce4ebb2e` | HoroInputAccessibilityTests, HoroInputTests, HoroInputPublicHeaderConsumer, HoroInputAccessibilityPublicHeaderConsumer | **88/88 passed, zero skipped**, 1.36s; `build/acc-input-results.xml` and `build/acc-input-discovery.json` |
| Foundation-only semantic contracts | `6308740ffc1b7167f2ffc7ef98044b1ffbe02837` | Visual/Gameplay accessibility test targets, both staged header targets, Visual/Gameplay/Contrast direct contract consumers | **11/11 passed, zero skipped**, 0.24s; `build/acc-semantic-results.xml` and discovery JSON |
| Character input | `6308740ffc1b7167f2ffc7ef98044b1ffbe02837` | HoroCharacterInputTests, HoroCharacterInputPublicHeaderConsumer | **16/16 passed, zero skipped**, 0.74s; `build/acc-character-results.xml` and discovery JSON |
| Captions / Runtime UI / optional bridge | `df38a89c2006ef57f0e33a7b91e2c37a4b05bd1b` | HoroRuntimeUiAccessibilityTests, RuntimeUi staged/accessibility consumers, CaptionTransport staged/direct consumers, PlatformAccessibility staged/direct consumers | **49/49 passed, zero skipped**, 0.72s; `build/acc-caption-results.xml` and discovery JSON |
| GUI / SDL / editor application | `ba5844883ac76ba135838d70ae5002dec733317b` | HoroEditorPresentationTests, HoroGuiPublicHeaderConsumer, HoroInputSdlTests, **HoroEditor** | **92/92 passed, zero skipped**, 50.99s: 82 presentation and 10 SDL cases; `build/acc-gui-results.xml` and `build/acc-gui-discovery.json` |

All batches configured successfully, including header ownership and dependency
direction checks. Affected staged/public consumers compiled. The GUI batch enabled
GUI, SDL input and OpenGL, compiled the actual editor input-cutoff implementation,
linked `HoroEditor.exe` and staged its runtime assets. It did not launch the editor
or enable GPU smoke tests. Its presentation tests are display-independent ImGui
render/model evidence, not observed native interaction or display output.

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
migration guide records its public layout/wire additions. Six new regression
cases and the extended public consumer are pending execution; none is covered by
the earlier 88-case Input result. Subsequent source adds owned Add/Edit intents
and an exclusive gyro modal through the existing workspace command and modal
hosts. Axis/calibration/sensitivity drafts apply only through canonical SetProfile;
six model/modal/render regression sources are pending. Paired-copy/CI Python
checks passed **43 tests** after those UI edits. Automatic motion-gesture capture,
SDL controller motion collection, native interaction and device qualification
remain open.

### Native bridge, editor semantics and gameplay

`HoroPlatformAccessibility` consumes existing Runtime UI semantics without making
baseline Platform depend on UI. Its public surface exposes no native handles.
Only Null currently exists: Unsupported, zero queues/schema, no effects or
recording, and ShuttingDown after shutdown. Desired preference remains distinct
from actual capability. Runtime UI retains focus/action authority and native
visibility requires actual matching presentation evidence.

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
