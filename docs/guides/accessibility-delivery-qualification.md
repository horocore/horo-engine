# ACC-001 delivery and qualification

This matrix tracks implementation and acceptance evidence for the complete
initiative. An entry remains incomplete until its integrated acceptance criteria
and required platform evidence exist. Existing Runtime UI semantic tests do not
qualify native assistive technology.

Original baseline: `f51e0c0f44cd0f4c7931d288a5dde794e3316dfd`.
Current published parent: `3df476ff430a2ad28cd62a8a0c1780ae0fea71bc` on
`origin/codex/editor-improvements`, verified from the remote on 2026-11-13.
The earlier local parent was `5d0aff9d63b03b3370f0f09fab49893ec0e4ae08` (C11 admission
fixture correction). GitHub and Jira bodies
were read on 2026-11-11; all seven implementation children were Open / To Do.
ACC-001.1 / HORO-1805 is already delivered and is not reimplemented.

The 2026-11-13 user restart supersedes the historical named-grant and global
source-freeze restrictions described in older evidence below. Own-worktree
changes and authorized normal push/draft delivery may proceed independently.
Heavy work still requires atomic nonblocking admission through the real shared
OS lock, verified physical handle, resource guards and one worker; a busy lock
does not permit a background waiter or interrupting another job.

| Child | Jira / GitHub | Implementation and evidence | Remaining acceptance / dependencies |
|---|---|---|---|
| ACC-001.2 captions | HORO-1806 / #1850 | Added bounded SPSC cue/catalog transport, timeline, frame-captured appearance, safe-area arrangement and canonical paint extraction; regression cases written, not executed | Host localization/shaping/presentation composition, settings activation and actual native rendered evidence |
| ACC-001.3 color | HORO-1807 / #1851 | Added independent numeric target, validated matrices and snapshot-backed synchronous query; regression cases written, not executed | Complete-image GPU pass and real backend evidence; existing ColorPipelinePlan explicitly provides cold admission only |
| ACC-001.4 input | HORO-1808 / #1852 | Added bounded router interpretation, explicit time and owned cancellation, profile-v2 migration and conflict validation; existing Input Mapping panel exposes exact-duration policy drafts; C++ regressions not executed | Compile/run router and editor regressions/consumers; compose host withdrawal; integrate scoped persistence/capture from INP-001.6 / HORO-1795 / #1839 owned by Input workstream; gyro device evidence |
| ACC-001.5 visual | HORO-1809 / #1853 | Added UI scale/contrast snapshot with readable typography floor, visual-safety snapshot and typed effect suppression seam; regression cases written, not executed | Both editor theme layers, coherent DPI/font/UI scale and actual Camera/VFX/animation consumers; visual review |
| ACC-001.6 bridge | HORO-1810 / #1854 | Existing Runtime UI semantics; added optional platform transport interface, honest no-op Unsupported adapter and opt-in bounded semantic QA journal; tests written, not executed | Native adapters, editor default shared-control metadata, bypass detection, QA review UI and real assistive-technology qualification |
| ACC-001.7 assists | HORO-1811 / #1855 | Added inert domain contribution, typed tick snapshot for all seven preferences and revision-only hint adapter; lost/stale hint tests written, not executed | Host tick composition, game-owned authority/capability consumers and shared settings UI contribution |
| ACC-001.8 tooling | HORO-1812 / #1856 | Added evaluator/contrast panel, Windows CI contrast selection, explicit CI evidence-scope reporting and complete 55-criterion open editor inventory with regulatory/platform scope notes; C++ tests unexecuted | Qualify all four tools, execute numeric/theme/native checks and complete the applicability/observed evidence matrix; depends on all above |

## Interpretation of issue wording

The normative accessibility architecture and ADR-015 take precedence over older
issue summaries. Captions originate independently of audio success, mute and
device availability. Audio observations cannot restart an existing semantic cue.
For ACC-001.7 the bus carries only `ConfigurationRevision`; every simulation tick
captures the authoritative configuration even when hints are lost or stale.
Assist values never become bus state and cannot bypass server authority.

### ACC-001.1 decision audit (2026-11-13)

[ACC-001.1 / #1849](https://github.com/horocore/horo-engine/issues/1849)
is a closed ownership decision, not delivery of the seven runtime children. Its
current body retains unchecked acceptance boxes and older Audio/PostProcessing
wording. The controlling sources are
[ADR-015](../adr/015-accessibility-ownership-typed-transport-and-non-gating-policy.md)
and the normative accessibility architecture. ADR-015 still labels itself
`Proposed`; this audit does not change its approval state or reopen the issue.
The normative architecture governs the implementation independently of that
metadata.

| Decision obligation | Current source evidence | Audit boundary |
|---|---|---|
| Name each authority and typed transport | ADR-015's ownership table covers captions, color, input, native bridge, gameplay, UI scale/contrast, motion/flash and developer validation | Present as a written contract; does not establish host activation |
| Keep settings authoritative outside the bus | Domain contributions are inert; `GameplayAssistTickSnapshot`, `ColorAccessibilitySnapshot`, caption/UI/safety and platform views retain immutable configuration. `GameplayAccessibilityStateEvent` contains only a revision, and its sole production publisher is `GameplayAssistSettingsAdapter::NotifyCommitted` | The mandatory host tick capture is not integrated; no claim that event delivery activates assists |
| Preserve independent consumers and non-gating | `src/CMakeLists.txt` creates CaptionTransport, VisualAccessibility, GameplayAccessibility and PlatformAccessibility without quality/tier selection. `cmake/HoroDependencyPolicy.cmake` keeps visual/gameplay targets Foundation-only and caption transport dependency-free; the bridge consumes RuntimeUi semantics | Source inspection supports independent library composition. Native/backend/product support still requires configured builds and actual host capability evidence |

The caption contract therefore must not be reduced to an Audio event consumer,
and color queries must not obtain their authority from an executing renderer.
The final SDR shader currently encodes composed display-linear color but has no
accessibility matrix input; `ColorPipelinePlan` remains a cold metadata/pin
candidate without an execution or publication owner. A new enum, CPU transform,
or shader-only edit would not prove ACC-001.3 complete. Implementation must also
provide the real composed-image execution, cooked shader ABI migration and
backend/native evidence. No existing decision is reimplemented by this audit.

## Evidence boundaries

Native Windows, macOS and Linux support requires the corresponding adapter and
actual assistive-technology interoperability evidence. Null is Unsupported;
recording is test-only. Headless tests, code review and compilation prove their
specific contracts only. GPU smoke tests, gyro hardware and visual interaction
checks are unverified until their actual commands and observations are recorded.
WCAG targets and CVAA/EAA/TRC alignment are qualification goals, not a legal
certification claim. External platform assessment remains product-owner work.

Required final validation: targeted regression and public consumer builds,
supported affected CMake compositions, worktree-aware local analysis, then full
Sonar IDE / CLI secrets / Codacy preflight against the verified target after final
edits. Heavy checks use the existing shared `heavy_build_lock`, serial build and
CTest execution.

## Current local verification

The contrast tool now exposes eleven explicit live theme pairs: primary text on
the four token surfaces, muted text on root/panel, dim text on raised, native
text on window/popup/frame, and native disabled text on window. The custom opaque
color draft remains available. The selected pair is read on each evaluation;
unknown selectors or non-opaque colors cannot produce a cached success. New
regression sources check pair selection, live native style changes, and rejection
of unknown composition. These C++ cases remain uncompiled/unexecuted. The paired
catalog and CI selection Python pass after this change is **42 passed**.
Disabled-text results are diagnostic; [WCAG 2.2 contrast criteria](https://www.w3.org/TR/WCAG22/#contrast-minimum)
exempt text in inactive components. The inspector does not infer applicability
from a color role or certify the complete editor.

`git diff --check` passes after current edits. A lightweight static registration
check also verified each new public header has exactly one registry entry, every
new implementation unit is listed once, and new UI error references are declared.
This is not a substitute for CMake configure or staged consumer compilation.
New C++ tests have **not** run.
The display-independent offline SDK compatibility pass ran with a short worktree
temporary root: `python -m pytest -q --tb=short --basetemp build/acc-pytest
tests/python/test_extension_sdk_compatibility.py`: **24 passed**. The earlier
sandbox-temp attempt had 21 fixture path errors and 3 passes; the short-root repeat
completed all 24. This is SDK metadata coverage, not new C++ behavior coverage.
The first serial skeleton configure acquired `scripts.dev.heavy_build_lock` in the
initial sandbox, then failed in the compiler probe with MSVC C1902 (PDB manager
mismatch). Its physical lock handle was not verified, so that attempt is not
evidence of correct cross-sandbox serialization.
The later elevated helper had no compiler children and was stopped while waiting
for that same lock, to give the main CI workstream its requested priority. No new
heavy check will start until the coordinator releases that priority window.
The next helper must also verify its actual open physical heavy-build lock handle
against the coordinator's reference before configure/build/test, not merely compare
the Git common directory or textual cache path. A mismatched sandbox/AC handle is
a stop condition, with no private-lock or permission fallback.

`rtk proxy <python> scripts/analyze.py doctor --offline` completed with status
incomplete (exit 2): Sonar CLI 1.10.0, Codacy 0.25.0 and VS Code 1.145.3 found;
CMake/Ninja were absent from that command's PATH. Cloud drift was deliberately
not checked by this offline prerequisite diagnosis. This is neither a Sonar IDE
analysis nor CLI secrets/Codacy execution. The next check must use the initialized
native tool environment and the actual final worktree scope.

A subsequent offline doctor with the existing portable VS Code user-data profile
and explicit native CMake/Ninja PATH completed **ready_for_local_check (exit 0)**.
The earlier default-profile check was incomplete because that default settings
file did not exist; no authentication or trust policy was changed. Tool discovery
now includes CMake 4.3.1 and Ninja 1.13.2. This prerequisite success still does not
prove connected-mode authentication, Workspace Trust, C++ sensor execution,
CLI secrets results, Codacy findings or cloud drift. Those remain final-check work.

The subsequent online `doctor --timeout 45 --code-user-data <existing-portable-profile>`
still reported local prerequisites ready and exit 0, but its independent
`cloudConfiguration` was **incomplete**: cloud configuration read failed (exit 2),
requiring Codacy authentication verification. Cloud drift is therefore unverified;
the top-level exit 0 is not treated as cloud success. The cached report is
`1794386611801302300-fa4293c8.json` in this worktree's external quality-report cache.
No credentials, trust policy, rules or cloud configuration were changed.

The baseline `scripts/analyze.py` prepares compiler context with a hard-coded
`--parallel 2`, even when reusing a database. The current machine policy requires
every native build to use `--parallel 1`, under the verified physical shared lock.
Do not run this conflicting preparation path or substitute another analyzer.
Resolve the serial preparation contract with the owning CI/tooling workstream
before full final analysis; ACC does not silently patch or monkey-patch that helper.
The coordinator has reported that its serial-worker correction is in preparation;
no release of the shared build slot has been received.

## New contract boundaries and migration

Fixed caption payloads are owned by the header-only `HoroCaptionTransport`, whose
standalone consumer has no UI, Audio, renderer or Foundation dependency. UI
arrangement/catalog transport are owned by `HoroRuntimeUi`; its staged public
consumer includes the caption/settings interfaces. The SPSC queue pins one catalog before the
producer starts. The control owner stops/joins that producer before destruction.
The UI timeline owns independent catalog leases after dequeue, and uses a bounded
64-sequence replay window. A host assigns session-qualified monotonically
increasing IDs before fan-out. Timeline session generations cannot be reused,
including after cancellation or backwards-clock invalidation. Queue overflow
drops newest and increments a saturating diagnostic counter. Audio observations cannot replace accepted text or
restart a cue. Headless arrangement is not proof of visible caption rendering.

`HoroVisualAccessibility` and `HoroGameplayAccessibility` depend only on Foundation.
Their isolated public consumers do not link renderer, UI implementation, native
input or simulation. No existing caller is removed or compatibility alias added.
These new domains expose inert contributions; application composition must
register them explicitly. Capture validates domain ranges before deriving a new
consumer view; a failed projection cannot replace the consumer's last-good view.

The contributions are not yet wired into a shipped host's settings activation.
Foundation's current SettingDescriptor has type/source/reload metadata but no
domain range-validation callback. Host integration must validate candidate domain
projections **before** configuration publication, not merely let consumers reject
an already committed invalid value. The current projection tests prove rejection
at the typed consumer boundary only. This remains a shared CFG integration gap;
the ACC change does not install a competing resolver or modify CFG-owned schema
infrastructure.

Foundation currently represents numeric configuration as integers. New domain wire
encodings are explicit fixed point, preserving typed runtime floats without
changing shared CFG infrastructure or adding string parsing: color severity and
gameplay strengths/multipliers use thousandths; color coefficients use millionths;
the gameplay invulnerability window uses milliseconds. For example severity 500
means 0.5 and a damage multiplier of 1000 means 1.0. Keys have one live encoding,
not parallel aliases. The UI domain alone owns high contrast; the color query
reads that key from its retained authoritative snapshot without registering it.

The separate `HoroPlatformAccessibility` adapter boundary depends on the existing
Runtime UI semantic owner; baseline `HoroPlatform` gains no reverse UI dependency.
It exposes no native handles. Only the no-op implementation currently exists:
its capability is Unsupported, its queues/schema range are zero, and Submit
returns Unsupported (ShuttingDown after explicit shutdown). It produces no actions,
native effects or recordings. No Windows/macOS/Linux native support is claimed.

`UiAccessibilityReviewLog` is an independent opt-in semantic diagnostic journal,
not the null adapter and not a speech delivery queue. It copies 64 owned event
records, counts overwritten diagnostics and omits oversized text whole. Content
recording is separately explicit; changing recording policy scrubs previous rows.
It consumes publisher changes rather than polling elements and never acknowledges
speech. A journal row is not evidence of native delivery.
The additive `UiAccessibilityChangePublisher::IsCurrentSnapshot` contract checks
the exact adopted immutable storage lease. Equal numeric descriptors from another
extractor do not establish publication identity. The journal uses this check before
mutation; existing publisher callers require no migration. New regression coverage
includes independent snapshots with equal revisions, copied leases and retirement.

Input's new `DigitalAffordance` and `StickyModifierLatch` are narrow mapping
helpers, not another router/profile/persistence authority. The existing router now
owns bounded per-incarnation interpretation and old-owner cancellation delivery.
Ended contexts expose owned scoped withdrawals through an explicit host drain,
whose application teardown composition and the external persistence work remain
acceptance gaps. The [migration guide](input-accessibility-binding-migration.md)
records bounds, public consumer changes and unexecuted lifecycle regressions.
The later [binding migration](input-accessibility-binding-migration.md) adds the
policy to the canonical typed binding/profile path and extends conflict validation.
It also fixes the manual Input accessibility consumer to move the successful
Result before invoking mutable `Cancel`; the earlier const `Value()` call could
not compile. Native validation remains pending rather than inferred from that
static correction.

UI's own contribution declares `accessibility.visual.ui.*`; safety owns only
`accessibility.visual.safety.*`. The UI view projects readable text sizes with a
16-DIP floor, but it does not pretend a new font atlas or coherent DPI layout was
published. The existing editor `Theme::SetUiScalePercent` explicitly remains a
no-op pending coherent font/layout scaling. Connecting both theme layers and the
actual frame owner, and reconciling the pre-existing editor appearance scale
preference with one accessibility authority, remain required integration work.
No legacy flat `visual.ui_scale`, `visual.high_contrast`, `visual.reduce_motion` or
`visual.flash_suppression` keys were found in active source/assets on this baseline.

The existing Input Mapping panel already invokes the canonical binding validator
and displays its diagnostics. This is useful existing evidence for the essential
action tool; it has not been counted as fresh UI qualification. Extension forms
also already export `EditorUiAccessibleNode` metadata. Neither establishes
default metadata for every built-in shared control or native editor exposure.

## Numeric references

Full-strength color matrices follow the
[Machado/Oliveira/Fernandes model](https://profs.ic.uff.br/~laffernandes/content/publications/journal/2009_tvcg_15(6)/machado_oliveira_fernandes-tvcg-15(6)-2009-corrected.pdf),
cross-checked against the published
[Colour v0.4.7 reference dataset](https://github.com/colour-science/colour/blob/v0.4.7/colour/blindness/datasets/machado2010.py).
Strength interpolates from identity; it does not claim physiological severity
modeling. The matrix space is display-linear sRGB; HDR/other primaries need an
explicit space adaptation in the renderer. These transforms are simulation/reference
modes, not a claim to restore a person's color perception.

The contrast evaluator follows the W3C
[AA minimum](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html) and
[AAA enhanced](https://www.w3.org/WAI/WCAG22/Understanding/contrast-enhanced.html)
thresholds. Inputs must be actual composed sRGB colors; alpha with an unknown
backdrop is not silently treated as opaque. Typography classification is supplied
from actual font metrics. Numeric contrast passing is one criterion, not complete
WCAG compliance or legal certification.

## Local delivery commits

These are traceable local implementation slices, not completed acceptance claims.
No branch push or pull request has been performed.

| Slice | Local commit |
|---|---|
| Caption transport/settings/layout | `8d4dd2b2a`, `2a478e471` |
| Color query/reference transforms | `22e402137` |
| Input affordance state and neutral-boundary fix | `21823b65b`, `8b4329747` |
| UI and visual safety preferences | `1a6aff479` |
| Bridge capability and semantic journal | `cca891531` |
| Gameplay preference snapshots and revision hints | `293d86528` |
| Contrast evaluator / qualification matrix | `929b465e7` |
| Exact semantic publication QA journal check | `456758017` |
| Editor contrast workspace panel | `bd5e7134a` |

The tooling slice also corrects the semantic journal header registration, so the
canonical Runtime UI public-header consumer includes that new public contract.

## Editor contrast tool integration

The default workspace registry now exposes `horo.accessibility_contrast` in the
right dock. Its shared color controls inspect exact opaque #RRGGBB custom drafts;
invalid intermediate text has no cached passing result. Theme inspection reads
current primary-text and root-surface tokens without 8-bit quantization, and
rejects non-opaque/invalid pairs with a localized composition diagnostic. The
standard/large category is explicit and documents the actual-font-metric rule.
Visible ratios are rounded for display; pass/fail comparisons are not.

`HoroGui` gains a private dependency on `HoroVisualAccessibility`; no public GUI
header or public dependency is added. Regression cases cover invalid edits,
unquantized theme inputs, unknown composition, default registry integration, and
English/Turkish narrow/wide headless rendering. A headless test checks standard AA
for primary text on the four opaque built-in panel surfaces and native ImGui text
on WindowBg. Those deliberately bounded pairs do not qualify dim/muted text,
interactive states, arbitrary project themes, or complete editor WCAG compliance.
They require the GUI test composition; headless skeleton configuration alone
cannot run them. Compilation, execution and manual popup/focus interaction remain
pending. A lightweight paired-catalog Python check plus the SDK metadata tests
completed with **25 passed** after the panel integration.

## Local parent integration

The coordinator explicitly authorized rebasing onto the unpublished local parent
`5d0aff9d63b03b3370f0f09fab49893ec0e4ae08`. The clean-worktree rebase replayed all
12 ACC commits without conflicts. `git range-diff` against the previous stack at
`cc109b4b3` showed every patch unchanged (`=` for all 12 entries). The paired-copy
and offline SDK metadata tests repeated after rebase: **25 passed**. Native ACC
compilation, GUI interaction and full local quality analysis remain unexecuted.
No heavy slot was granted by that integration message; the main regression queue
retains priority. No remote fetch or push was used to locate the local parent.

## Resumed initiative audit and remaining ownership

Live GitHub acceptance bodies were re-read for parent #1813 and every child
#1849--#1856. Parent and implementation children remain open; #1849 is closed.
The closed decision is supported by the normative architecture's explicit owners,
typed transports and non-gating rule, but its status does not qualify all later
product compositions. A persistent active goal now covers every remaining child,
integrated acceptance, regression and qualification rather than stopping at a local
checkpoint. No Jira or issue state was changed by this audit.

| Dependency/evidence | Owner and required next input |
|---|---|
| ACC C++ regression and staged consumer execution | Main CI coordinator grants one bounded serial headless configure/build/CTest slot; ACC verifies the physical canonical lock handle before using it. No waiter is queued. |
| GUI contrast render and theme checks | ACC needs a subsequent GUI-enabled build/test composition under the same serialized slot, then actual popup/focus/narrow-state interaction checks. |
| Pre-publication domain validation and shared settings menu | CFG owning workstream provides candidate validation/activation integration; ACC composes its inert domain contributions without another resolver. |
| Binding lifetime/conflict/persistence integration | Input owning workstream supplies the standard persistence/capture path; ACC extends affordance policy through that same authority. Gyro additionally needs actual supported hardware. |
| Complete-image color execution | Renderer/ACC integration must retain composed UI through the final display-linear transform and verify available real backends; cold admission is insufficient. |
| Native bridge interoperability | ACC/Platform implements adapters; platform QA supplies real Windows UIA, macOS accessibility and Linux AT-SPI interoperability evidence. None is inferred from Null or headless tests. |
| Cloud configuration comparison | Codacy account authentication must permit the read-only configuration fetch; its current failure remains independent of local tool readiness. |

## Caption paint extraction integration

`CaptionRenderer` now reserves bounded scratch and the ordinary `UiRenderExtractor`
at setup. It uses the host's existing retained elements, prepared localized/shaped
glyphs and stable font generations; it creates no alternate UI tree, localization
resolver or native handles. It requires the exact queue catalog lease and coherent
frame appearance and the host's exact locale-selection/localization-snapshot
commitment. The copied commitment is evidence supplied by the existing text owner,
not a new authority or proof of translation correctness. Future cues do not paint, Sound/Music respect the display
preference, and disabling publishes an empty paint snapshot. It emits canonical
background/text commands with safe-area clipping, decodes encoded sRGB appearance
colors into linear UI paint, and combines background alpha with opacity.

New regressions exercise the actual canonical extraction boundary: allocation-free
successful paint, independent semantic time, settings coherence/retry, catalog
identity, duplicate rejection, invalid glyph/resource ranges, capacity exhaustion,
snapshot lifetime after source/presenter destruction, and three reference aspect
ratios. The staged Runtime UI consumer includes the new public header. These C++
tests have not run. Application localization/shaping and native presentation still
need composition and real rendered evidence; this extraction is not reported as
completed caption acceptance or a platform qualification.
The additive public API is owned by Runtime UI with no dependency expansion or
existing caller removal. Animated hosts must pass the actual canonical frame lease;
the presenter forwards it to the existing extractor's lineage/pinning overload,
preserving queued-reader retirement. It never fabricates a frame or completion
receipt. Existing non-animated extraction remains explicit and separately bounded.

## Input router integration checkpoint

The Input-owned router now applies the profile's typed affordance policy through
bounded transactional mapping, explicit host sample time and scoped cancellation
delivery. The migration guide records the added repeat/status/withdrawal API,
non-reusable identities, neutral admission and fixed limits. Actual router test
sources cover holds/repeat, sticky chords, skipped reads, configuration changes,
focus/modal/time/device/player lifetimes, owned withdrawals, capacity rollback
and allocation-free reads. The staged consumer exercises the new public drain.
None of these C++ tests or consumers has compiled or run yet.

After these edits, the paired localization and SDK Python command recorded above
passed again: **25 passed**. `git diff --check` passed and changed C++ was formatted.
These checks do not validate C++ behavior. The first requested native slice remains
Input tests and its two staged consumers; the main CI board grants another owner
the sole heavy slot. No ACC heavy process or background waiter was launched.
Full Sonar IDE, CLI secrets and Codacy preflight remains incomplete pending the
serialized analysis slot and supported preparation path; cloud configuration
authentication remains a separate previously recorded gap. Host withdrawal,
rebinding UI execution, scoped persistence integration and gyro are still acceptance work.

## Input Mapping accessibility editing

The existing digital-binding view now opens a typed property draft for toggle,
sticky modifiers, hold and repeat durations. It commits only on Apply through
the existing full-profile validation/cancellation owner, preserves other bindings
and uses the ordinary save/reload controls. Exact millisecond text preserves
microseconds; invalid input is retained without implicit clamping or substitution.
Configuration revision and full original binding checks reject stale drafts,
including replacement/restoration of an otherwise equal mapping. The form uses
shared controls, wrapped labels and paired English/Turkish copy.

New model/GUI regression sources are registered with `HoroEditorPresentationTests`.
They remain uncompiled/unexecuted; mouse, keyboard, focus, hover/disabled and
native theme interaction still need the allocated GUI composition and manual
review. The paired-copy/SDK Python pass after these edits is **26 passed**.
`git diff --check` and formatting passed. Native and full quality validation are
still incomplete under the main coordinator's heavy-slot hold. The own Input
Mapping code fork still needs authorized integration with Input's unpublished
scoped capture/persistence changes; no sibling worktree source was changed.

## Editor qualification inventory and CI selection audit

The [editor qualification record](accessibility-editor-qualification.md) now
inventories every current WCAG 2.2 A/AA criterion, maps Horo workflow/evidence
gaps to owners, and distinguishes source evidence from observed native results.
A read-only comparison with the pinned official Recommendation HTML confirmed
**55 exact unique identifiers and levels**. No entry is passed or silently
not-applicable. The record uses WCAG2ICT to frame native-software interpretation
and cites official CVAA/EAA/public platform guidance for limited scope notes;
actual legal applicability and authorized TRC qualification remain open decisions.
The EUR-Lex directive text was unavailable behind a verification page, so no
article-level conclusion is asserted from the Commission's overview.

CI source inspection found that Windows' focused Debug registry omitted the new
numeric contrast tests. It now includes `HoroVisualAccessibilityTests` and the
two visual/contrast staged consumers; direct consumers also carry `ci-windows`.
Linux/macOS GUI-enabled Debug already includes the presentation target containing
the opaque built-in primary-text pair cases. CI summary copy explicitly reports
Windows' missing GUI-linked theme coverage and the narrow scope of other checks.
No CI or native result is inferred from these selection edits. CMake configure,
actual discovered labels, CTest execution and final quality preflight are pending.

The local ACC helper's old filter named executable targets, but Catch discovery
registers `Target::case` names. It could therefore have selected only the direct
consumer. This was found before any ACC native test execution or claimed pass.
The helper now matches the case-family prefixes, requires every expected family
and direct consumer in a JSON discovery manifest, and uses `--no-tests=error`.
Its syntax was checked; it was not launched under the coordinator's HOLD.

The paired-copy, offline SDK and existing CI grouping Python suites ran after
these edits with **65 passed**. This validates their catalog/metadata/registry
contracts, not generated CTest membership, C++ behavior or conformance. The
changed-source diff check passed. All heavy analysis remains incomplete.

Gameplay integration inspection separately confirmed two unresolved boundaries:
the current `GameRuntimeContext` / behavior callback context has no accessibility
tick binding, and `ConfigurationService::Snapshot` currently takes `m_mutex`.
There are no aim-assist, damage or QTE behavior consumers in the active source
tree beyond the new desired-preference model. Native SDK boundary migration,
an explicitly owned tick-start acquisition path satisfying the non-blocking
contract, and game-owned supported/authorized behavior consumers remain required.
No ambient settings lookup, bus-state cache or silent authority grant was added.

## 2026-11-13 restart and published-base integration

The six preserved CI/qualification paths were committed as `366451048` after the
paired-copy/SDK/CI Python suites repeated with **65 passed**. The remote parent
was verified with `git ls-remote` and fetched. Authorized rebase then replayed
all 19 ACC commits onto `3df476ff` without conflicts. Range-diff recorded 18
unchanged patches and one context-only change around the parent's expanded CI
inventory; the new parent entries remain intact. The pre-rebase stack is retained
by a local backup ref. This evidence concerns patch integration, not native behavior.

The known CharacterInput missing-clock diagnostic now retains the typed Input
cause and clears pending intent; its focused native regression is authored.
`heavy_build_lock(repo, blocking=False)` now forwards nonblocking admission to
the existing OS lock. Default callers retain their blocking behavior. A regression
checks refusal under a live owner, preservation of that owner's lock, and admission
after release, using isolated temporary fixture caches. No fallback execution lock
is introduced. The first cache/runner Python pass recorded **107 passed, 4 skipped**.
Three skips require Windows symlink privileges and one requires a real
compiler/ccache fixture. A same-scope repeat hit host disk exhaustion:
**100 passed, 4 skipped, 2 failed, 5 setup errors**, with ENOSPC in Git fixture/temp
writes. That repeat is incomplete infrastructure evidence. The system drive was
verified at zero free bytes; this worktree's build outputs were only about 1 MiB.
No shared cache, sibling files or live job was removed. A fresh cache/runner run
and native/quality execution remain pending adequate actual storage and resources.
After published-base integration, the paired-copy/SDK/CI grouping suites passed
again with **66 passed**, recorded in the local ignored JUnit report
`build/acc-current-python.xml`; they do not exercise the new C++ behavior.
The local ACC batch helper must use this nonblocking API and the current resource
and physical-handle guards; it no longer requires a named main-chat grant.

## Current delivery preparation on 2026-11-13

The parent identity was verified in Jira and GitHub as
`HORO-1769 / #1813 [ACC-001] Accessibility Subsystem`. The initial caption-named
local branch is retained; the complete topic now uses
`feat/HORO-1769_accessibility_subsystem` for normal delivery. The published target
remains `codex/editor-improvements` at `3df476ff`. No merge, force push, history
rewrite of published ACC commits or issue-state mutation is authorized by this
record.

The final cache/runner retry completed **107 passed, 4 skipped**, with the same
three Windows symlink-privilege skips and one real compiler/ccache fixture skip.
Its local ignored report is `build/acc-cache-python.xml`. This supersedes the
failed ENOSPC repeat for that exact Python scope; it does not prove C++ behavior.
The paired-copy/SDK/CI grouping pass remains **66 passed** in
`build/acc-current-python.xml`.

Two V2 Input batch attempts returned busy at the real OS lock before compiler or
analyzer startup, without a waiter. The full final-base preflight command was
also requested through nonblocking physical-lock admission, which returned busy.
`build/acc-preflight-admission-20261113.json` is explicitly an **admission receipt,
not an analyzer report**: Sonar IDE, CLI secrets and Codacy are all `not_run`.
They remain required. No clean security, quality or native result is inferred.

The new doctor run found local tools ready but its independent Codacy cloud
configuration fetch remained incomplete (exit 2/authentication). The official
doctor report basename is `1794573359424626900-cddeab7f.json` in the external
worktree quality cache. This is prerequisite evidence only; it does not prove
connected-mode authentication or C++ sensor execution.

A reviewable draft may expose this work and run remote CI under the explicit
restart authorization, with these unresolved checks and all seven implementation
gaps visible. It must remain incomplete and must not close parent/child issues
or claim acceptance. Full native, public-consumer, GUI/GPU/AT/hardware and final
local quality evidence must still be completed for the entire initiative.
