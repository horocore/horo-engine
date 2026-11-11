# Platform Services authoring qualification

This records PLS-003 acceptance evidence and remaining integration work. A closed
GitHub child or a successful model test does not establish host, migration or
private-provider qualification.

## Baseline and scope

The delivery starts at `f51e0c0f44cd0f4c7931d288a5dde794e3316dfd` on
`codex/editor-improvements`. The main checkout and its open PR #3457 are separate
from this worktree. Native GitHub sub-issues contain exactly these eight children.
The corresponding Jira parent is HORO-1825; Jira workflow states are retained,
not inferred from GitHub closure.

On 11 November 2026 the delivery branch was rebased onto the local parent
`5d0aff9d63b03b3370f0f09fab49893ec0e4ae08`. That commit adds only the verified
C11 provider-admission fixture configuration. The parent was not yet published
to origin at the time of this rebase. No remote lookup was used to find it and no
uncommitted main-checkout changes were imported. Hash comparison preserved all
23 source edits; only `tests/CMakeLists.txt` gained the expected parent hunk,
with the PLS target additions retained. Final native validation remains pending
against this rebased worktree.

| Child | GitHub / Jira | Baseline source and regression evidence |
| --- | --- | --- |
| PLS-003.1 | #1888 / HORO-1844 | ADR-132 and the normative Platform Services identity section; #2467 and #2508 merged |
| PLS-003.2 | #1895 / HORO-1851 | `PlatformStableIdRegistry.cpp`, `PlatformStableIdRegistryTests.cpp`; #2692 merged |
| PLS-003.3 | #1896 / HORO-1852 | `AchievementDefinitionRegistry.cpp`, `AchievementDefinitionRegistryTests.cpp`; #2695 merged |
| PLS-003.4 | #1893 / HORO-1849 | `PlatformDefinitionRegistries.cpp`, `PlatformDefinitionRegistriesTests.cpp`; #2698 merged |
| PLS-003.5 | #1894 / HORO-1850 | `PlatformProjectConfiguration.cpp`, `PlatformProjectConfigurationTests.cpp`; #2701 merged |
| PLS-003.6 | #1900 / HORO-1856 | `PlatformProviderManifestCook.cpp`, `PlatformProviderManifestCookTests.cpp`; #3025 merged |
| PLS-003.7 | #1897 / HORO-1853 | Open; shared Project Settings integration and localized presentation remain required |
| PLS-003.8 | #1898 / HORO-1854 | Open; integrated host, migration and clean/incremental publication evidence remains required |

All seven PLS merge commits supplied by those PRs were verified as ancestors of
the starting HEAD using `git merge-base --is-ancestor`. The transitive
CFG-001.2 dependency #1822 / HORO-1778 is GitHub closed, with #2680 merge
`45623cb8da27f9bb65c909449b881e6ab6b80abd` also an ancestor; its Jira state
remains In Review. PLS-003.1 is Jira Done; the other five closed PLS children
remain Jira In Review.

PLS-003.7 depends on #1894, #1893, #1896 and the open shared modal dependency
#1825 / HORO-1781. PLS-003.8 depends on #1895, #1893, #1896, #1894, #1897 and
#1900. The shared CFG modal work has a separate owner; PLS must use its common
draft lifecycle rather than introduce another settings authority.

ADR-132 still has a Proposed status label. The normative architecture explicitly
references its identity contract, so that label is not permission to change the
algorithm, salt authority, permanent history or private mapping boundary.

## Additive authoring contracts

Ordinary ledger edits use `BuildPlatformStableIdRegistryReplacement` with the
current immutable registry and the edit's captured fingerprint. Initial detached
validation remains `BuildPlatformStableIdRegistry`. Replacement preserves every
primary, numeric ID and alias; active entries may become tombstones with removal
provenance. Tombstones cannot disappear, restore themselves or rewrite their
removal evidence. Project identity and the captured root salt cannot change,
including for empty ledgers. A host must serialize validation with publication;
this pure function does not provide filesystem transaction authority.

Existing callers of initial construction need no source migration. A host editing
an existing ledger must adopt the replacement operation instead of validating only
the candidate. There is no implicit compatibility path for namespace regeneration
or restoration. Those remain separately authorized migration operations.

Diagnostic classification now distinguishes invalid root salts (`InvalidSalt`),
duplicate primary/alias keys (`DuplicateKey`), reserved tombstone reuse
(`Tombstoned`) and numeric collisions between different primaries
(`HashCollision`). Previously an identical duplicate primary reached the numeric
collision check first. Callers matching the older generic classification must
use the refined code. The registry tests and Project codecs are the affected
callers in this change. Canonical document size is now part of initial validation,
so a candidate that would exceed 4 MiB cannot become a snapshot that its own loader
cannot read.

The Project-owned `HoroEngine::PlatformServicesAuthoring` target owns
`Horo/Application/PlatformServicesAuthoring.h`. It exposes bounded ledger
decode/encode operations without leaking its JSON dependency. The canonical
document has exactly `schemaVersion`, `projectId`, `algorithm` and `entries`;
entries have exactly `kind`, `canonicalKey`, `id`, `aliases`, `state` and
`removalProvenance`. Kind tags and ID text follow ADR-132. Output uses two-space
indentation and one final newline, with registry/alias ordering from the shared
validator. The salt is supplied from one captured root metadata authority and is
never serialized in the ledger. Unknown members, duplicate JSON keys, unsupported
algorithms/versions, oversized documents and invalid encodings fail closed.

The canonical `ProjectSourceDocument` parser captures the optional root salt and
`settings.platformServices` JSON values alongside existing metadata. It does not
introduce a second root parser, defaults or provider activation. Its additional
source fields are trailing optional fields, so existing aggregate callers remain
source compatible. Legacy absence remains readable to compatibility inspection;
explicit Platform Services resolution then fails with a typed missing-salt or
missing-policy error until migration supplies the new authoring data.

Portable policy contains exactly `schemaVersion`, `provider` and `services`.
Provider selection is the closed `{mode, key}` object (`null` or `exact`), with
service requirements in exhaustive `PlatformServiceKind` order. The explicit host
profile is supplied by composition, never stored as an authoring override. Root
salt validity is checked before policy admission; trusted-module, exact-provider
and required-capability failures retain the shared validator's typed identities.
The policy encoder excludes host profile, draft fingerprints and private values.

`PlatformProjectSettings.h` contributes the same portable fields to Foundation:
eight keys owned by `horo.platform.services`, legal only at the Project source,
with `ProjectReopen` reload policy. The contribution is inert and has no
environment bindings. `PlatformProjectSettingsValues` and
`ResolvePlatformProjectSettings` bridge the immutable typed policy and the one
Foundation source resolver. There is no separate UI settings state or precedence
implementation. Salt, identity, host profile and mappings are absent from this
editable key set.

Primitive schema validation does not authorize a provider/capability transition.
The shared Settings integration must project and validate the complete candidate
through `ResolvePlatformProjectSettings` before applying its one transaction.
Registering the descriptors alone does not complete that integration, and schema
defaults must not conceal missing required project authoring data.

This codec does not register a new persistent release contract or mutate
existing projects. Host adoption must accompany project creation and an explicit
root-last migration through the existing project migration executor. Read-only
load and cook must never synthesize a salt or repair missing ledger data.

The earlier registry fingerprint hashed a separate binary projection rather than
the canonical ledger document. The corrected fingerprint hashes ASCII
`horo.platform-services.registry-fingerprint.v2`, one zero byte and the exact
canonical UTF-8 ledger bytes. `CanonicalDocument` is the single encoder used by
both the registry fingerprint and the Project codec. Schema-v1 salt derivation,
numeric IDs, primary keys, aliases and tombstones do not change.

This deliberately invalidates old fingerprint-bound definition snapshots, mapping
evidence, cooked outputs and retained drafts. There is no dual-fingerprint
compatibility validator. Consumers must load the unchanged root salt and ledger,
revalidate semantic definitions against the new captured fingerprint and regenerate
provider mappings and both cooked streams as one generation. They must not relabel
old artifacts as fresh. Previously captured immutable values remain alive for their
owners; a host may publish the newly validated generation only at its existing safe
boundary. Persisted authoring adoption remains part of the versioned migration,
and runtime/offline migration cannot silently reinterpret an old binding.

## Validation ledger

The first native Windows Debug pass configured the headless composition, built
`HoroPlatformRequestTests` and `HoroPlatformServicesPublicHeaderConsumer` with
`--parallel 1`, then ran `HoroPlatformRequestTests [stable-id]`: 10 cases and
183 assertions passed. This predates the subsequent codec, UTF-8 and collision
diagnostic edits and is not final-diff evidence.

Every heavy command uses `scripts/dev.py::heavy_build_lock` keyed by the resolved
common Git directory. Before starting, a 30-second CPU average and available
memory are checked; CPU above 60 percent or available memory below 25 percent
defers execution. A busy shared lock causes a no-queue exit. The native host
`LOCALAPPDATA` must be used: a sandbox-redirected cache would not coordinate with
other chats. After acquiring `heavy_build_lock` and before spawning any heavy
command, `psutil.Process().open_files()` must confirm the actual open handle's
Windows path equals the shared native `heavy-build.lock` path. A matching Git
identity or textual cache path alone is insufficient. A different AC/sandbox
handle path or access denial defers work without a private-lock fallback.
The main Windows ALL validation retains priority; a free or bounded slot must be
announced before another heavy job starts. Test concurrency is one.

Final codec tests, affected header consumers, complete platform-service
regressions and the required final-scope Sonar IDE / CLI secrets / Codacy report
are pending. No Linux/macOS, native private-provider, GUI interaction, durable
migration recovery or cloud quality-gate pass is claimed here.

## Outstanding integrated evidence

- One project-creation transaction must publish root identity, cryptographic salt
  and empty ledger together; a versioned migration must add legacy identity once.
- GUI/CLI/headless/cook must capture the same root and ledger revisions and use
  the shared validators. Pure codec reuse alone does not prove host wiring.
- Shared Settings apply/cancel, stale revisions, localization, theme layers,
  narrow widths and long text must be exercised against the actual modal.
- Clean and incremental cooks must converge after durable authoring reload,
  preserve the previous published pair on failure, and fence mixed revisions.
- Fork/regeneration must provide complete reviewed reference/mapping disposition,
  rollback and derived/offline invalidation evidence. Unknown references must
  block without changing recoverable source data.
