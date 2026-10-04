# Navigation source authority and approved next-minor integration

Status: implementation plan authorized by the task owner, not a published or
signed production release. HORO-1253 owns the shared release, descriptor and
migration integration. HORO-1128 supplies its network payload/transform; there
must not be concurrent edits to shared release authority.

## Existing authority and the precise gap

The normative Navigation architecture, lines 519–550, assigns stable sidecar
AssetId, grounded profiles, build/tile policy, areas and bake-scope policy to one
NavigationDefinition. Lines 581–617 require complete typed capture, publication
provenance, and HoroProjectVersion migration for source definitions/settings.
ADR-105 is PROPOSED and is not independent approval for a persisted schema.

The actual public envelope is NavigationSourceRecords, not a symbol named
NavigationDocumentEnvelope. NavigationDataSerialization.h defines stable
nonzero record IDs/type IDs, per-record versions, explicit consumer support
tables and required/optional records. HNAV envelope 1.1 is extensible: a new
required record with payload version 1.0 fits without changing existing record
bytes or the envelope version. Unknown required records reject; unknown optional
records can only be preserved inert. These rules do not define payload semantics.
Current production code has no registered semantic definition record/codec;
serializer tests use synthetic types 100/200 and short opaque byte payloads.

The normative project-versioning contract makes the distinction mandatory:

- Core Decisions, lines 34–69: all durable Horo authoring data shares one project
  version; persistent-model changes require a migration decision.
- Version semantics, lines 201–224: authored-semantic/descriptor changes cannot
  be introduced into a patch or another build advertising the same version.
- Governed project-owned data, lines 284–325: Navigation definitions and stable
  profile/area/link identities migrate; generated navigation is rebuilt.

The frozen releases/0.1.0/project-contract.json describes only .horo/project.json;
it contains no Navigation record-type registry or semantic payload descriptor.
Its release.json declares canonical persistent contract
sha256:997e790fc23515b362847c755006156aa35353ce7f2624518acf7ed1214ddb03,
migration definitions
sha256:3382b3c37fe16c263b13da2ef81a76c9d1c8ea2b5026a35ef17ed2c102487c24,
and recovery contract
sha256:ccca65a3a1fab86ad116098c3d5c1b8499118137470761e50eb174b7de121acb.
generate_project_compatibility.py hashes that descriptor canonically (omitting
the release marker) and verifies frozen manifest identities. Therefore an
unlisted new Navigation record would evade the old descriptor inventory, not
prove existing compatibility. That inventory gap is not permission to reuse
0.1.0 unchanged for new required semantics.

Raw SHA-256 preservation checkpoints, distinct from canonical contract hashes:

- releases/0.1.0/project-contract.json:
  0f1013738ead6565f73fefa8f3bb2f327ccafa71d7a1b3218955d2c73297bffe
- releases/0.1.0/release.json:
  352d5df2fff6e98f85758e12a7616a76690825c6168997296c36905d4c26eae0
- releases/0.1.0/migration-recovery-contract.json:
  1c1088b0003dcdad8602a9284046d2ad69fbce7717d5a1a1a824c2a8bf08dd9c
- definitions/0.1.0/ProjectMigration.cpp:
  da29da2aaf488eb2d957cd2a072421f5bbf7f1833dbb9415fcc4bb0fb0a156fb

## Approved smallest implementation

The actual version authority is root CMakeLists.txt's HORO_ENGINE_VERSION 0.1.0;
only release directories 0.0.1 and 0.1.0 exist. Use a new 0.2.0 development
contract, preserving every frozen 0.1.0 file.

1. Register one core required NavigationDefinition record type, stable
   ID 0x4e41564445463031 (NAVDEF01), payload version 1.0 in unchanged HNAV 1.1.
   Document the allocation in the new descriptor, header and consumer support
   table. Never reinterpret synthetic/unknown legacy payloads as this type.
2. Put the bounded typed immutable model and canonical codec under the lower
   Navigation owning target. Reuse existing grounded profile, area/filter,
   build and coordinate types. Keep Scene intent under the shared Scene owner.
   Editor/application/CLI/MCP consume one decoder and capture owner, not separate
   formats, default profiles or caches. Sidecar AssetId stays the asset authority.
3. Add releases/0.2.0 descriptor/manifest/recovery references and a convention-
   discovered definitions/0.2.0 migration from the exact 0.1.0 contract. Compute
   contract/definition-set/decision hashes with existing generators, not hand
   edit frozen hashes. Register the exact target validator and retain existing
   journal-v1 recovery authority.
4. Migration preserves already opaque records and authored identity; it must not
   fabricate required profiles/settings from uninterpretable legacy bytes.
   Unsupported required semantics fail the unpublished candidate with explicit
   diagnostics/fidelity evidence. Derived bake artifacts/cache are invalidated,
   never converted into authoring truth. New definitions use the registered type.
5. Integrate HORO-1128's explicit legacy UnknownCompleteness state without
   upgrading it to authoritative complete-empty. Network fields and transforms
   are supplied by that owner, while the global release files remain single-owned.
6. Update development engine/version coupling, SDK metadata and consumers
   deliberately. 0.2.0 is not publication/signing authority; existing native SDK
   fingerprints and required rebuilds must remain truthful.

Affected callers are the shared definition/capture owner, Editor definition and
Scene adapters, CLI/MCP automation, migration target validation/project-open, and
SDK/release-version fixtures. Generic HNAV serialization retains its existing
bytes and explicit support-policy contract.

Without the typed source owner and registered persistent contract, full-project
and selected-surface automation cannot derive canonical profiles/settings and
validated source provenance from current opaque payloads. Inventing those values
inside CLI/MCP would create a competing source authority and cannot meet HORO-1253.
The approved next-minor integration removes that blocker; no further routine
payload decision is being awaited.

## Candidate identity classification and incremental validation

The published Network codec stack was integrated by normal fast-forward to
cc75273602e3365d545016234e4fa708062fe8cb; the ticket's dirty edits were restored
without conflicts and a labeled recovery stash is retained.

Navigation definition classification reads committed `.horoasset.horo` identity
sidecars from the constrained candidate, not the derived asset index or source
magic. `Assets::DecodeAssetIdentitySidecar` is a pure bounded promotion of the
existing registry decoder in `HoroAssets` (the existing AssetRegistry.h owner).
Registry rebuilding and migration share the same schema, identity, source/type
and containment checks. No new filesystem or activation behavior is exposed.
The generated `HoroAssetsPublicHeaderConsumer` covers this public declaration;
Asset registry tests exercise it directly. Source plus `.horo` is bounded to
4096 bytes before copies; metadata is bounded to 1 MiB.

Declared `core.navmesh.definition` sources require exactly one NAVDEF01 record,
including when their bytes are truncated or their magic is corrupt. Other asset
types do not acquire Navigation semantics. Generic optional HNAV authoring is
still preserved inert; migration never invents a profile or definition.

The existing transaction owner verifies prior history bytes against the
authoritative head before appending a receipt and computes the replacement head
from the exact new history bytes before finalization. The journal-v1 contract is
unchanged. New production regressions cover restart/resume, lost-forward-evidence
rollback, missing/tampered history, corrupted declared definitions, and generic
asset preservation. All five requested migration/Assets targets and the generated
Assets public-header consumer compiled. The final serial focused CTest pass was
14/14, timeout 60 seconds per case. An initial test compile error (shared-pointer
access) and an overly generic diagnostic assertion were corrected; production
codec diagnostics were preserved. Earlier Scene/definition passes are separate
evidence, not a substitute for this integration slice.

Codacy-only feedback on the 51-file parent delta returned exit 2: 29 local
complexity/length advisories, five unsupported CMake paths, and oversized
tests/CMakeLists.txt (190834 bytes) remain incomplete/UNKNOWN. Local Sonar was
explicitly waived. Separate CLI secrets scanning passed all 51 selected files.
The exact parent baseline is cc75273602e3365d545016234e4fa708062fe8cb. The moved
Scene findings retain their baseline CCN and original source ranges; neighboring
findings retain their baseline metrics. The promoted pure Assets decoder has CCN
13 versus 14 in its previous I/O-owning ParseAndValidateSidecar function. No
suppression or metric-only refactor was applied. The local CPD estimate retains
39 clone groups with exact ranges, including inherited Scene/test blocks and new
short migration-interface/strict-JSON boilerplate; it is not hosted duplication
percentage proof. Full findings and provenance are retained in the local
validation reports, not checked into release authority. This is truthful partial
feedback, not a clean global gate or full HORO-1253 acceptance claim.
