# Runtime Scene Cell Data Layers

WST-004.8 adds `RuntimeSceneCellLayers` to `HoroEngine::SceneCellPayload`.
The existing unfiltered cell payload API and ADR-023 container remain unchanged.
The integration target owns the new public header through the existing ownership
registry; neither RuntimeScene nor WorldStreaming gains a reverse dependency.

Cook callers first produce the complete offline-expanded `RuntimeSceneCellPayload`,
then call `EncodeRuntimeSceneCellLayers` with its exact source identity, canonical
layer references and canonical `(SceneObjectId, StreamingLayerId)` membership edges.
A successful encoding transfers the baseline once. Failure, including cancellation,
leaves that baseline unconsumed. Metadata owns no component copies, runtime epochs,
state generations, service pointers or source paths. Advance the baseline source
revision whenever membership or layer classification changes; never reuse an old
revision for changed inputs. Ownership classification revisions remain exact inputs.

An entity with no edges is unconditional. Multiple edges use union semantics: it
survives once if any membership layer is target-included and Activated. This keeps
one object payload even for overlapping layers. The host supplies the complete
same-order ownership and state snapshot for every encoded layer and an exact target
policy/context to `FilterRuntimeSceneCellLayers`. Target rules are those of
`FilterWorldLayers`; state records retain their independent layer lifecycle.
Loaded, transitional, rollback and Failed layers cannot activate content. Excluded
parents or required constraint references return Scene validation errors rather
than silently changing transforms or references. Authored entity order is preserved.

Filtering is load-time work. Admitted entity, edge, layer and retained-byte ceilings
bound ownership and work; membership lookup uses canonical binary searches and
bounded per-object edge traversal. There is no allocation per entity during lookup.
Materialization creates exactly one detached Scene definition for the selected
publication. Complete baseline asset requirements are conservatively retained,
because opaque gameplay dependencies cannot safely be inferred from components.
This may load assets also used only by excluded entities; dependency pruning needs
an explicit per-object dependency contract and is not guessed here.

Queue the complete selection with `QueueRuntimeSceneCellLayers`, an exact residency
fence and a shared host `SceneCellLayerAuthority`. Its bounded owner-thread predicate
checks current content identity, world owner/epoch, full cell attempt, exact policy,
every state/ownership fence and provider/reservation readiness at admission and at
`CommitDeferredLifecycleChanges`. Excluded layers are fenced too. Policy/state/world
replacement before commit rejects stale work and preserves active Scene. The caller
can retire source, encoding, selection and its authority reference immediately after
successful queueing; the pending operation owns copies and the authority lease.

`RuntimeSceneService` retains its existing asynchronous asset preparation,
transactional replacement, failure reporting, cancellation, unload and shutdown
ownership. New layer publications prepare a fresh complete selection; they do not
mutate active storage through membership metadata. Moved encodings and selections
are unusable. No competing residency or layer state machine is introduced.
