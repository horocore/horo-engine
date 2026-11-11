# ACC-001 delivery and qualification

This matrix tracks implementation and acceptance evidence for the complete
initiative. An entry remains incomplete until its integrated acceptance criteria
and required platform evidence exist. Existing Runtime UI semantic tests do not
qualify native assistive technology.

Original baseline: `f51e0c0f44cd0f4c7931d288a5dde794e3316dfd`.
Current local parent: `5d0aff9d63b03b3370f0f09fab49893ec0e4ae08` (C11 admission
fixture correction), supplied by the main coordinator before its origin push. GitHub and Jira bodies
were read on 2026-11-11; all seven implementation children were Open / To Do.
ACC-001.1 / HORO-1805 is already delivered and is not reimplemented.

| Child | Jira / GitHub | Implementation and evidence | Remaining acceptance / dependencies |
|---|---|---|---|
| ACC-001.2 captions | HORO-1806 / #1850 | Added bounded SPSC cue/catalog transport, timeline, frame-captured appearance and safe-area arrangement; regression cases written, not executed | Localization/presentation composition, host settings activation and actual rendered integration |
| ACC-001.3 color | HORO-1807 / #1851 | Added independent numeric target, validated matrices and snapshot-backed synchronous query; regression cases written, not executed | Complete-image GPU pass and real backend evidence; existing ColorPipelinePlan explicitly provides cold admission only |
| ACC-001.4 input | HORO-1808 / #1852 | Added standalone mapping-owned digital/sticky affordance state and deterministic boundary tests, not executed | Connect to router cancellation/release ownership and existing conflict validator; persistence depends on INP-001.6 / HORO-1795 / #1839 owned by Input workstream; gyro device evidence |
| ACC-001.5 visual | HORO-1809 / #1853 | Added UI scale/contrast snapshot with readable typography floor, visual-safety snapshot and typed effect suppression seam; regression cases written, not executed | Both editor theme layers, coherent DPI/font/UI scale and actual Camera/VFX/animation consumers; visual review |
| ACC-001.6 bridge | HORO-1810 / #1854 | Existing Runtime UI semantics; added optional platform transport interface, honest no-op Unsupported adapter and opt-in bounded semantic QA journal; tests written, not executed | Native adapters, editor default shared-control metadata, bypass detection, QA review UI and real assistive-technology qualification |
| ACC-001.7 assists | HORO-1811 / #1855 | Added inert domain contribution, typed tick snapshot for all seven preferences and revision-only hint adapter; lost/stale hint tests written, not executed | Host tick composition, game-owned authority/capability consumers and shared settings UI contribution |
| ACC-001.8 tooling | HORO-1812 / #1856 | Added unrounded sRGB AA/AAA evaluator, registered editor contrast panel and paired localized copy; numeric/headless UI tests written, not executed | Qualify the four tools and execute the new display-independent primary-text theme checks, platform evidence and documented qualification gaps; depends on all above |

## Interpretation of issue wording

The normative accessibility architecture and ADR-015 take precedence over older
issue summaries. Captions originate independently of audio success, mute and
device availability. Audio observations cannot restart an existing semantic cue.
For ACC-001.7 the bus carries only `ConfigurationRevision`; every simulation tick
captures the authoritative configuration even when hints are lost or stale.
Assist values never become bus state and cannot bypass server authority.

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
helpers, not another router/profile/persistence authority. Owning router wiring
must cancel the old incarnation and deliver its release before replacement. That
wiring and the external persistence work remain acceptance gaps.

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
