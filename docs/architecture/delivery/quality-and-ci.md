# Quality And CI

## Goals

CI provides fast feedback while preserving full confidence across:

- engine modules
- GUI editor
- CLI host
- MCP in both GUI and CLI hosts
- supported platforms and renderer configurations

The pipeline is parallel, cache-aware, deterministic, and based on CMake
presets.

## Pipeline Shape

```text
change classification
        |
        +-- formatting and static checks
        +-- architecture and public-header checks
        +-- affected module builds
        +-- affected unit tests
        +-- affected fast contract tests
        |
        v
full integration and cross-host contract tests
        |
        +-- CLI
        +-- MCP
        +-- scene/assets/pipeline
        |
        v
GUI automation and visual tests
        |
        v
platform and configuration matrix
        |
        v
package, reports, and artifacts
```

Independent jobs run in parallel. Fast-lane jobs report without waiting for
merge-gate or protected-branch jobs. The diagram shows increasing validation
depth, not a requirement that every job wait for the preceding stage.

## Fast Feedback And SLAs

CI enforces explicit duration Service Level Agreements (SLAs) to prevent
pipeline bloat:

- **Fast Lane (PRs):** Automated analysis and affected-test results must report
  within **10 minutes**. This is a feedback SLA, not a promise that a PR becomes
  merge-ready within ten minutes.
- **Protected Matrix (`main`, `release/*`):** The bounded acceptance matrix must
  complete within **45 minutes** through parallel platform jobs and
  duration-based test sharding.

Human decisions are outside the Fast Lane runtime SLA. In particular, SonarQube
analysis may report an unreviewed Security Hotspot within ten minutes and fail
the quality gate immediately; an authorized reviewer must then classify the
hotspot before merge. The PR remains blocked until the New Code hotspot review
condition reaches 100%, but the CI feedback SLA has still been met.

Pull requests run a lightweight fast lane containing:

- formatting verification
- static analysis and **SonarQube** code quality gates. The project enforces
  strict conditions on **New Code**:
  - Reliability, Security, and Maintainability ratings must be **A**.
  - **100%** of Security Hotspots must be reviewed.
  - Aggregate New Code coverage must be **>= 80.0%**.
  - Duplicated Lines must be **<= 3.0%**.
- architecture dependency checks
- public-header compile checks
- affected CMake target builds
- affected unit tests and fast contract tests for changed modules

Fast contract tests are deterministic, non-GUI checks of a changed module's
public typed outcomes and complete within the per-test fast-lane budget. Full
CLI/MCP cross-host contracts, cross-module integration scenarios, GUI-backed
contracts, and `[slow]` tests run in later merge-gate or protected-branch jobs.

The fast lane uses:

- Ninja
- compiler caching through `sccache` or `ccache`
- dependency and toolchain caches keyed by platform and lock inputs
- test sharding based on measured duration
- build artifacts shared between compatible downstream jobs
- path and target impact analysis that may skip unrelated expensive jobs

Impact analysis can reduce work, but protected branches also run the complete
required matrix before release.

## Architecture Checks

Static architecture checks enforce:

- GUI source contains no raw color literals or one-off standard control
  dimensions outside theme resources and approved test fixtures
- shared primitives do not depend on editor models, application capabilities,
  `EditorDataBus`, or `EngineDataBus`
- tabs and modals do not receive an omnibus application service
- application, editor-model, CLI, and MCP targets do not depend on ImGui
- theme files pass schema, unique-ID, inheritance-cycle, and required-token
  validation
- event types remain in their declared process or editor-session scope
- logging call sites use stable categories and do not bypass the common C++ or
  Python facade
- metric descriptors use stable names, declared units, bounded dimensions, and
  approved product-profile availability
- profiler instrumentation uses the shared facade and shipping restrictions
- uploaded log and diagnostic artifacts contain no known secret patterns

The visual-literal check is token-aware rather than a blind number ban. Numeric
values used for domain data, geometry, algorithms, and test inputs remain valid;
only styling values in GUI rendering code must resolve through design tokens.

**Enforcement Mechanisms**:

- **Target dependency rules:** CMake target linkage and project-owned
  architecture checks enforce allowed module and public-header dependencies.
- **Source-level module boundaries:** project-owned architecture checks and
  CMake target validation enforce forbidden includes, dependency direction,
  and typed boundary constraints.
- **Omnibus service injection:** A project-owned semantic check inspects
  constructor and factory signatures for tabs and modals. It rejects application
  service aggregates and requires the narrow capability interfaces declared by
  the component. CMake linkage alone is not considered sufficient enforcement.
- **Visual literals and logging:** Custom token-aware source analyzers in
  `scripts/check_architecture.py` run independently before CMake configuration.
- **Theme validation:** A standalone Python schema validator runs during the
  static check phase.
- **Secret scanning:** `gitleaks` runs on every commit in the PR with a curated
  `.gitleaks.toml` ruleset.

## Branch Strategy And Build Matrix

The pipeline execution adapts based on the target branch:

- **Feature branches:** Run the Fast Lane on one reference platform and primary
  renderer, plus affected merge gates required by the change classification.
- **Protected branches (`main`, `release/*`):** Run the complete supported
  platform/configuration matrix with a bounded acceptance suite.
- **Hotfix branches:** Run the protected matrix but receive runner-queue
  priority.

The protected matrix covers every required configuration tuple:

- GUI host across all supported production renderers (Vulkan, OpenGL, Metal)
- CLI host without GUI
- MCP inside the GUI host
- MCP inside the CLI host
- headless runtime with the null renderer
- bounded Debug and sanitizer smoke validation
- release configuration, package creation, and package smoke validation

Platform jobs cover every supported operating system. Platform-specific tests
run on their native environment. Renderers are tested in parallel jobs per
platform (for example, macOS+Metal, Windows+Vulkan, and Windows+OpenGL). Visual
baselines are versioned strictly per renderer and platform combination to
account for driver-level rasterization differences.

"Complete matrix" describes configuration coverage, not exhaustive test depth.
Each protected tuple runs a measured acceptance shard selected to keep the
45-minute critical path. The scheduled matrix revisits the same tuples with
cold state and exhaustive suites; this is intentional depth validation rather
than redundant execution of the same job.

## Test Layers

### Unit Tests

Unit tests validate isolated module behavior and are linked against production
targets. They must not compile private copies of production source files.

### Integration Tests

Integration tests validate boundaries such as:

- editor document to runtime scene conversion
- asset import and package output
- runtime lifecycle and renderer frontend
- project build and release operations
- platform services behind testable interfaces

### Contract Tests

Application use cases are exercised through GUI-facing adapters, CLI commands,
and MCP tools. Equivalent operations must produce equivalent typed outcomes.

MCP contract tests run against both hosts:

- MCP connected to the GUI host
- MCP connected to the CLI/headless host

### CLI Tests

CLI tests validate:

- argument and command parsing
- exit codes
- structured and human-readable output
- cancellation and signal handling
- filesystem side effects
- use of shared application services

### GUI Tests

GUI quality is measured through:

- component behavior matrices for size, variant, icon, loading, disabled, and
  focus states
- panel and workflow automation
- keyboard and focus navigation tests
- modal input blocking, focus trapping, and close-frame click-through tests
- representative screenshot comparisons
- packaged/custom theme discovery, override precedence, invalid reload fallback,
  and scaling scenarios
- editor and engine data-bus performance/regression checks for affected paths
- Performance tab CPU/memory/frame scenarios and profiler capture controls
- editor lifecycle and persistence scenarios

Visual baselines are versioned strictly per renderer and platform. Screenshot tolerances are explicit and kept narrow.

Renderer scene references follow
[ADR-050](../../adr/050-cross-backend-reference-image-tests.md) and remain
separate from editor screenshots. Pull-request fast lanes run comparator/schema/
Null fixtures and an affected native subset only when a stable documented GPU
lane is available. Protected branches require the bounded backend/platform scene
matrix; scheduled qualification expands device/driver classes and cold-cache
coverage.

Required lane absence is an infrastructure failure or blocked check, never a
visual pass. Image mismatch, timeout, device loss and unexpected unsupported
capability are not retried. Failure artifacts include canonical reference/actual,
diff/heatmap and a manifest identifying case, backend, environment, capability,
capture operation and thresholds. Baseline promotion is a separate reviewed
change; CI cannot overwrite or automatically approve candidate output.

### Flaky Test Policy

GUI and visual tests are susceptible to flakiness. The CI policy is strict:

- Any test that flips between pass/fail on the same commit is automatically marked as **Flaky** by the CI runner.
- Flaky tests are immediately **quarantined** (they run, but do not fail the pipeline).
- A tracking ticket is automatically generated for the owning team.
- Quarantined tests must be fixed within 7 days. If a test proves consistently unreliable and cannot be stabilized within this window, it must be permanently deleted rather than left rotting in quarantine.

For ADR-050 native renderer references, quarantine is explicit missing
qualification rather than a pass. The affected backend/platform tuple remains
`NotScheduled`/unqualified in packaged qualification reports until a stable
required case replaces or restores the quarantined case. CI never retries an
image mismatch to choose a passing sample or promotes either sample as a baseline.

### Performance Regression Testing

Build-time and test-duration trends continue to alert on target compilation
increases above 10% and material integration-suite degradation.

Renderer runtime gates follow
[ADR-051](../../adr/051-renderer-benchmark-and-regression-gates.md). They compare
only exact workload/environment cohorts against the median of 5–20 eligible
protected-branch runs from the preceding seven days. Candidate/failed/quarantined
runs never enter their own baseline; insufficient, expired or unstable baselines
block required qualification.

Default frame/memory gates combine relative and absolute bounds: CPU/GPU p50/p95
regress above 5% and 0.20 ms, p99 above 8% and 0.50 ms, and declared peak memory
above 5% and 16 MiB. Workload descriptors may tighten these values; loosening
requires reviewed same-cohort evidence. Slow samples are not trimmed, regressions
are not retried, and missing hardware/metrics are not pass.

Pull requests always run Null/gate-math fixtures and may run affected native gates
on stable lanes. Protected/scheduled matrices own required native cohorts and
publish candidate distributions, baseline source values, exact gate math and
typed environment/measurement failures.

## Coverage Model

Line and branch coverage measure executable logic; they do not prove that a GUI
looks or behaves correctly. GUI correctness therefore has both code coverage
and scenario coverage.

Included in code coverage:

- foundation, assets, scene, physics, renderer frontend, and pipeline logic
- application use cases
- editor model and editor services
- CLI parsing and adapters
- MCP protocol and adapters
- UI state, presentation logic, validation, and component result behavior

Excluded from the required line-coverage denominator:

- third-party and generated code
- platform SDK headers and generated bindings
- thin ImGui drawing adapters that contain no domain or presentation decisions
- renderer backend glue that can only be validated meaningfully by dedicated
  backend or integration tests

An entire GUI directory must not be excluded merely because it uses ImGui.
Testable state and presentation logic belongs outside thin drawing functions and
remains part of coverage.

GUI automation produces a separate scenario report containing:

- workflows executed
- component states exercised
- screenshots compared
- themes and UI scales tested
- platforms and renderers covered

Coverage profiles from compatible unit, integration, CLI, and MCP jobs are
merged into one report. GUI automation may also contribute line coverage when
instrumentation is stable, but visual and interaction acceptance remains a
separate quality gate.

The hosted Sonar workflow separates instrumented build/test work from the full
CFamily scan. The coverage producer has a 240-minute budget; the `SonarCloud`
consumer has an independent 360-minute budget for a cold analysis cache. These
are execution caps for the full merge gate, not fast-lane feedback targets.

The producer publishes the original compilation database, generated/dependency
headers and coverage reports under its actual artifact ID. The consumer checks
the exact source commit/tree, original workspace/SDK paths and SHA-256 identities
of captured inputs and system dependencies before restoring generated files.
Tracked source is supplied by the same checkout and cannot be replaced by the
artifact. Missing coverage evidence fails the required `SonarCloud` check.
This retains the native environment and absolute-path requirements of
[CFamily compilation-database analysis](https://docs.sonarsource.com/sonarqube-cloud/analyzing-source-code/languages/c-family/prerequisites).

Coverage has two independent gates:

- SonarQube applies an aggregate **New Code** regression floor of 80% to the
  lines introduced or changed in the PR. This is not a project-wide overall
  coverage target.
- The coverage report applies thresholds by module category. These policies
  protect high-risk modules from being hidden by well-covered utility code and
  may require a threshold higher than the aggregate New Code floor.

A PR must satisfy both gates. Passing the aggregate SonarQube condition does not
override a failing module policy.

## Quality Gates

Required pull-request gates:

- formatting and architecture checks pass
- GUI visual literals and forbidden dependency checks pass
- changed theme resources pass schema and inheritance validation
- changed observability code passes disabled-call, context propagation,
  redaction, metrics, profiler, queue, and retention tests
- all affected targets compile
- unit, integration, CLI, and MCP contract tests pass
- required GUI workflows pass
- no unexpected visual baseline changes
- changed production logic has appropriate regression coverage
- coverage does not fall below the project threshold or module policy

Overall project coverage is not accepted or rejected through one global number.
Module-category thresholds remain authoritative alongside the separate
aggregate New Code regression floor described above.

## Scheduled And Asynchronous Validation

Heavy validation is shifted to asynchronous periodic runs (nightly or triggered
development-branch pipelines) to avoid blocking developers.

Scheduled validation runs:

- the complete platform and configuration matrix from empty build and dependency
  state
- sanitizers and deep dynamic analysis
- full GUI, visual, and integration test suites
- full CLI/MCP cross-host contract suites
- longer stress, lifecycle, and concurrency tests
- clean builds without warm caches

Protected and scheduled validation cover the same supported configuration
tuples for different purposes. Protected validation is a warm-cache, bounded
acceptance pass with a 45-minute SLA. Scheduled validation has no 45-minute SLA
and increases test depth, performs cold builds, and runs long diagnostics.

Release validation additionally runs:

- reproducible package generation
- artifact integrity and manifest verification
- install and launch smoke tests
- GUI, CLI, and MCP acceptance tests against packaged artifacts
- generated game-template test harness and packaged-player smoke validation
- security and dependency reports

## CI Artifacts And Reporting

CI publishes raw artifacts for debugging:

- test reports
- merged coverage reports
- GUI scenario reports
- game project test reports and packaged-player smoke logs where applicable
- visual diffs for failed screenshots
- logs and crash diagnostics
- build and package artifacts where applicable
- timing data used for test sharding and pipeline optimization
- optional Linux devcontainer cache seeds produced only by protected
  default-branch workflows under the validation contract in
  [Build Cache](./build-cache.md#devcontainer-cache-seeds)

### Dashboards (GitHub Pages)

To provide visibility without requiring developers to dig through CI logs, the
project maintains an automated **GitHub Pages** site. It is a snapshot generated
by the latest successful Scheduled Validation run, not a live view of every
protected-branch build:

- **SonarQube Dashboard Link**: Current code quality, technical debt, and security hotspot status.
- **Test Matrix Dashboard**: A visual grid of test pass/fail states across all renderers and platforms.
- **Coverage Map**: Interactive HTML coverage reports.
- **Performance Trends**: Graphs tracking build times, frame times, and memory usage over time based on the Scheduled Validation runs.

Every page displays the source commit, workflow run, generation timestamp, and
age. If no successful refresh occurs within the expected nightly interval plus
the configured scheduling grace period, the site shows a prominent **stale**
status and retains the last successful snapshot for diagnosis. Links to
SonarQube and individual workflow runs may expose newer live data than the
nightly snapshot.

Logs and artifacts must not contain credentials, tokens, or sensitive project
data. Failed jobs retain structured logs with test/shard/build context.
Successful-job retention remains bounded by CI policy.

See [Observability Architecture](../observability/observability.md) for the canonical schema,
redaction pipeline, support bundles, and storage policy.
