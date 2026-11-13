# Editor accessibility qualification record

Status on 2026-11-11: **not qualified**. This record belongs to HORO-1812 /
#1856 [ACC-001.8] under HORO-1769 / #1813. It inventories the complete editor
target and makes missing evidence explicit; no table entry is a conformance pass.
The [delivery record](accessibility-delivery-qualification.md) owns implementation,
commands and current execution results. No platform or legal approval is inferred
from source inspection, a semantic recorder, numeric checks or a closed issue.

## Assessment basis and boundaries

The criterion inventory uses the 12 December 2024
[WCAG 2.2 Recommendation](https://www.w3.org/TR/2024/REC-WCAG22-20241212/).
Its 55 A/AA identifiers and levels were checked against that published HTML on
2026-11-11. Criterion 4.1.1 is removed from this version; AAA criteria are not
silently included in the AA completion count. Numeric AAA contrast results are
an additional tool capability, not an AAA editor claim.

HoroEditor is native software. The 11 December 2025
[WCAG2ICT Group Note](https://www.w3.org/TR/2025/NOTE-wcag2ict-22-20251211/)
provides informative non-web interpretation; it does not itself impose a new
normative software standard. Product qualification must record each adaptation
to windows, documents, panels and complete workflows. A web scanner cannot
establish native editor conformance. These are the project's engineering targets,
not an assertion that a particular jurisdiction mandates this exact checklist.

Include Welcome, project selection/creation, workspace documents and dock panes,
settings/modals, command/search workflows, file and asset operations, scene and
runtime-UI authoring, build/output/error handling, input mapping, update/recovery
flows and embedded extension surfaces. Review installation and external account,
help and purchase surfaces separately when they are in the shipped user journey.
Do not limit sampling to the new accessibility panels or a default empty project.

For each evaluated workflow record the exact source SHA, package/configuration,
OS and display scaling, renderer/component selection, theme, locale/font, input
method, screen reader/version where applicable, steps, expected/observed behavior,
and artifact path. Shared-control evidence must identify callers and bypasses.
Only an explicit applicability rationale can replace a test with not-applicable;
absence of a test or a native adapter is never such a rationale.

## Evidence available in this worktree

| Code | Inspected source or recorded result | What it establishes and what remains |
|---|---|---|
| C0 | `tests/unit/runtime/ContrastTests.cpp`; `HoroVisualAccessibilityTests` | Numeric unrounded sRGB cases authored; C++ compilation/execution pending. Not a rendered editor assessment. |
| C1 | `tests/unit/editor/AccessibilityContrastPanelTests.cpp`; `HoroEditorPresentationTests` | Primary text on four opaque built-in token surfaces and native WindowBg pairs authored; excludes muted text, controls, selection, hover/focus and custom themes. Unexecuted. |
| I0 | `tests/unit/runtime/InputAccessibilityRouterTests.cpp`; `tests/unit/editor/InputMappingAccessibilityTests.cpp` | Router lifetimes and explicit profile drafts covered by new source tests; C++ execution and actual keyboard/assistive input review pending. |
| S0 | `src/editor/app/EditorTheme.cpp`, `SetUiScalePercent` | Current function explicitly leaves coherent font/layout/viewport scaling disabled. A desired scale snapshot is not delivered scaling. |
| A0 | `tests/unit/runtime/ui/UiAccessibility*Tests.cpp`; `UiNullAccessibilityBridgeTests.cpp` | Semantic and bounded-journal test sources exist. Null reports Unsupported. No native platform/AT interoperability result is available. |
| V0 | `src/runtime/accessibility/VisualAccessibility.cpp`; `VisualSafetySettings.cpp` | Numeric preferences and derived safety policy exist. Full composed-image rendering and actual Camera/VFX consumers remain missing. |
| L0 | Paired accessibility-copy and offline SDK Python invocation in the delivery record: 26 passed | Catalog/SDK structure only; does not prove language pronunciation, focus, presentation or native semantics. |
| M0 | Caption transport/timeline/paint extraction test sources in `HoroRuntimeUiAccessibilityTests` | Independent semantic cues and canonical paint contracts authored. Native playback/presentation and authored media inventory remain unqualified. |

All evidence above is scoped. Earlier unrelated tests do not qualify modified
inputs. Full Sonar IDE/secrets/Codacy checks and every affected native composition
remain required after final edits; their current incomplete status is recorded
separately from this accessibility assessment.

## Complete A/AA inventory

`P` means partial implementation/test source exists, with acceptance unproven.
`G` means an inspected implementation or composition gap remains. `U` means the
workflow/applicability assessment is still missing. No status means passed, failed
by a completed assessment, or not-applicable. The scenario column identifies the
Horo surface to investigate; the linked standard owns the exact criterion.

Owners: `Editor` = editor/shared-controls workstream with ACC-001.5/.6;
`Input` = ACC-001.4 and INP-001.6;
`Media` = ACC-001.2 plus authored media/gameplay owner;
`Visual` = ACC-001.3/.5 plus Render/Camera/VFX;
`Platform` = ACC-001.6 native adapters and platform QA;
`Product` = ACC-001.8 release/qualification owner. These are responsibility
assignments for the remaining work, not new tickets or inferred completion dates.

| Criterion | Level | State | Horo scenario / existing evidence | Owner |
|---|---|---|---|---|
| 1.1.1 | A | G | Icon-only shared controls, viewport tools and inspector previews; A0 | Editor / Platform |
| 1.2.1 | A | U | Inventory audio-only/video-only help, previews and authored playback | Media / Product |
| 1.2.2 | A | P | Recorded dialogue and instructional playback; M0 | Media |
| 1.2.3 | A | U | Instructional/scene media alternatives and authoring responsibility | Media / Product |
| 1.2.4 | AA | U | Inventory any live communication or broadcast surface | Media / Product |
| 1.2.5 | AA | U | Authored visual narrative and instructional media inventory | Media / Product |
| 1.3.1 | A | G | Native structure for properties, trees, tables and modal groups; A0 | Editor / Platform |
| 1.3.2 | A | P | Dock/document reading sequence through semantic/native trees; A0 | Editor / Platform |
| 1.3.3 | A | U | Instructions for placement, color-coded tools and viewport gestures | Editor |
| 1.3.4 | AA | U | Supported window/display orientation policy and resizing | Editor / Product |
| 1.3.5 | AA | U | Inventory user-information entry and any platform autofill semantics | Editor / Product |
| 1.4.1 | A | G | Selection/error/status cues and authored scene indicators; V0 | Editor / Visual |
| 1.4.2 | A | U | Startup/preview sound controls and interruption behavior | Media / Editor |
| 1.4.3 | AA | P | Complete token/native-style state inventory; C0 / C1 | Editor |
| 1.4.4 | AA | G | Coherent DPI, font atlas and layout scaling; S0 | Editor |
| 1.4.5 | AA | U | Text baked into previews, help and other shipped assets | Editor / Media |
| 1.4.10 | AA | G | Narrow panes, docking, modal forms and zoomed layout; S0 | Editor |
| 1.4.11 | AA | U | Focus/selection borders, tool icons, sliders and checkbox states | Editor |
| 1.4.12 | AA | U | Font/layout customization interpretation for native editor text | Editor / Product |
| 1.4.13 | AA | U | Tooltips, popup dismissal, hover transfer and keyboard focus | Editor |
| 2.1.1 | A | P | Complete command/property/viewport keyboard workflow; I0 | Input / Editor |
| 2.1.2 | A | P | Modal, popup, text field, capture and embedded-tool escape; I0 | Input / Editor |
| 2.1.4 | A | P | Shortcut editing, context isolation and text entry; I0 | Input / Editor |
| 2.2.1 | A | U | Update/session/confirmation timers and any authored time limits | Product / Editor |
| 2.2.2 | A | G | Animated preview/playback and Camera/VFX suppression; V0 | Visual / Media |
| 2.3.1 | A | G | Actual editor/runtime flashing corpus and effect consumers; V0 | Visual / Product |
| 2.4.1 | A | U | Navigation between major dock/document regions | Editor |
| 2.4.2 | A | U | Window, document, modal and embedded-tool identity | Editor |
| 2.4.3 | A | P | Dock/modal focus transitions and semantic order; A0 | Editor / Platform |
| 2.4.4 | A | U | Links in help, diagnostics, errors and external resource actions | Editor |
| 2.4.5 | AA | U | Command/search/menu access to complete editor workflows | Editor / Product |
| 2.4.6 | AA | P | Paired labels and actual native navigation groups; L0 / A0 | Editor / Platform |
| 2.4.7 | AA | U | Focus visibility in both theme layers and custom overlays | Editor |
| 2.4.11 | AA | U | Floating panes, modals, popups and scrolling focused inputs | Editor |
| 2.5.1 | A | P | Single-pointer alternatives across editor/runtime-UI tools; I0 | Input / Editor |
| 2.5.2 | A | P | Release/cancel behavior and destructive pointer actions; I0 | Input / Editor |
| 2.5.3 | A | G | Visible text versus native control name; A0 | Editor / Platform |
| 2.5.4 | A | U | Supported gyro/motion inputs and alternative controls | Input / Product |
| 2.5.7 | AA | U | Docking, assets, timeline, node graph and viewport drag alternatives | Input / Editor |
| 2.5.8 | AA | U | Actual tool/button targets at supported DPI and pane widths | Editor |
| 3.1.1 | A | P | Locale activation and native tree language metadata; L0 / A0 | Editor / Platform |
| 3.1.2 | AA | U | Mixed-language authored content and native pronunciation metadata | Editor / Media |
| 3.2.1 | A | U | Focus-driven navigation and inspector side effects | Editor |
| 3.2.2 | A | P | Explicit accessibility-draft Apply plus complete form audit; I0 | Editor |
| 3.2.3 | AA | U | Menus, dock navigation and embedded surfaces across workflows | Editor |
| 3.2.4 | AA | U | Command labels/icons across built-in and extension callers | Editor |
| 3.2.6 | A | U | Help/support entry points across the shipped journey | Product / Editor |
| 3.3.1 | A | P | Invalid drafts, profile conflicts and accessible error delivery; I0 / A0 | Editor / Platform |
| 3.3.2 | A | P | Input Mapping units/labels plus remaining editor forms; I0 / L0 | Editor |
| 3.3.3 | AA | P | Correctable input errors and canonical profile diagnostics; I0 | Editor |
| 3.3.4 | AA | U | Destructive asset/project/save operations and any account transactions | Product / Editor |
| 3.3.7 | A | U | Repeated input in creation/import/recovery/account workflows | Product / Editor |
| 3.3.8 | AA | U | Inventory authentication and external account handoffs | Product |
| 4.1.2 | A | G | All shared-control metadata, bypasses and native adapters; A0 | Editor / Platform |
| 4.1.3 | AA | G | Build/error/save/status announcements through actual AT; A0 | Editor / Platform |

## Platform and regulatory alignment notes

These entries assign the next evidence, not a legal applicability decision.
Keep product/editor delivery, engine capability and games built with Horo as
distinct assessed products. Desired preferences do not grant game/server authority.

| Framework | Source and limited interpretation | Missing Horo decision / evidence and owner |
|---|---|---|
| CVAA / advanced communications | [47 CFR Part 14](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-14) addresses advanced-communications equipment/software and service providers. | Product/legal must inventory shipped chat/voice/video and communications access, distribution and applicable limitations/waivers. Engineering supplies control, caption, communication and native-AT evidence for the actual covered functions. No blanket inclusion/exemption is asserted for the editor or every Horo game. |
| EAA | The [European Commission's EAA overview](https://commission.europa.eu/strategy-and-policy/policies/justice-and-fundamental-rights/disability/european-accessibility-act-eaa_en) lists covered product/service categories, including computers/operating systems, communications and e-commerce. | Product/legal must map actual SKU, consumer journey, market and national implementation to the applicable category and obligations. EUR-Lex's directive text was blocked by a verification page during this check; no article-level compliance conclusion is drawn from the overview. Editor WCAG tests alone cannot establish compliance of a storefront, account service or game. |
| Platform TRC / certification | Platform-specific requirements and approved revisions must come from the authorized platform program. The public [Xbox Accessibility Guidelines](https://learn.microsoft.com/en-us/xbox/accessibility/guidelines) describe best practices and explicitly distinguish them from compliance/legal validation. | Release/platform owner supplies the actual authorized checklist, platform/SDK/revision, allowed evidence storage and certification results. No proprietary TRC clause, waiver or pass is invented; public XAG alignment is not a TRC pass. |

The product owner must retain the signed scope decision and current applicable
requirements alongside qualification evidence. Engineering then maps each
applicable requirement to exact tests and observed platform behavior. A missing
scope decision is open work, not permission to remove a requirement from this
record.

## CI selection and platform gaps

`HoroVisualAccessibilityTests` and the visual/contrast staged consumers are in the
focused Windows native Debug target registry and carry its `ci-windows` selection.
Linux/macOS GUI-enabled native Debug includes `HoroEditorPresentationTests`, which
contains the display-independent primary-text theme-pair cases. The ordinary
Windows focused preset is headless and cannot configure those GUI-linked cases.
The CI summary now states these scopes and directs reviewers to actual CTest
outcomes. This is configuration/source evidence only: no new CI run or native
pass exists for these edits yet. Manual/full rendered and native-AT assessment
remains open on every supported platform.

Before the first allocated local validation, discover exact case names with
`ctest --show-only=json-v1`. Catch cases are prefixed `Target::`, rather than
named only after their executable. The ACC helper now requires every expected
case family and direct consumer to appear in the discovery manifest before
execution, and uses `--no-tests=error`. Its former executable-only filter could
have selected just a direct consumer; no C++ pass was claimed from that filter
and no native execution occurred. Discovery and the subsequent run must use the
same frozen HEAD, composition and filter.
