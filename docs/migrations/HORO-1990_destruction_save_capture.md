# HORO-1990 destruction Save capture

## One semantic model

Save capture and Network replication must not define different destruction state or
make a Save adapter depend on Network. `CanonicalDestructionSnapshot` in
`HoroEngine::DestructionApi` is the single semantic model. The previous replication
state/mask/seed/limits/artifact spellings are aliases of those types, preserving one
source of truth. Their authority field now uses `DestructionAuthorityEpoch`; callers
construct that product identity explicitly and convert its value at Network role/fence
boundaries. All clients must rebuild after this public layout change. The Network
version1.0 field IDs and bytes remain unchanged; existing golden tests protect them.

## Host composition

`HoroEngine::DestructionSaveIntegration` composes DestructionRuntime and Runtime Save,
without a Network or Physics backend dependency. The host owns one
`IDestructionSaveSource` lease and binds its exact Scene opening, target/content,
capability and authority generations to a `DestructionSaveAdapter`. Creating the
binding or descriptor registers nothing and invokes no source callback. Register the
adapter explicitly in `CanonicalStateParticipantRegistry` before capturing.

The source runs synchronously at the common Save owner safe point and calls
`CapturedDestructionState::Capture` with the actual `DestructionDamageRuntime`, current
owner evidence, exact cooked stable-ID table and the DFR owner's complete chunk,
seed and support state. Health is unsigned Q16.16 with nearest-unit rounding; capture
rejects unrepresentable health. Identity, configuration, revision, phase and features
must match the live canonical owner. Capture never commits damage, changes streaming
residency, marks a document saved or emits presentation events.

Every active chunk requires an explicit same-epoch Physics canonical participant
checkpoint receipt with exactly matching stable chunk membership. The Save binding
names that required participant dependency. The receipt contains identity/provenance
only; this adapter neither serializes nor reconstructs Physics body motion. An absent,
stale or mismatched receipt rejects the complete capture. An inactive state cannot
carry a contradictory motion receipt. Host integrations must obtain the receipt from
the real Physics participant; a body observation is insufficient authority.

The adapter checks budget before invoking the source and writes only bounded canonical
bytes through the host-owned sink. RuntimeSaveCaptureBuilder owns aggregate rollback,
ordering and immutable publication. Retained records survive later source replacement
or shutdown. New capture after cancellation, replacement or adapter shutdown fails
without mutating owner state. Source leases must safely own or borrow live DFR owners;
no runtime reference crosses into encoded worker state.

## Save schema1

The deterministic Save codec writes approved canonical primitives in this fixed
schema order: version; canonical handle/content octets; configuration, product authority,
semantic revision and seed/cursor; capabilities, phase, health, seed schema, exact chunk
count and support cursor; four bounded masks; sorted stable anchors; common Scene
incarnation/revision/tick and capability revision; then optional paired Physics
participant/checkpoint IDs. Participant identity is the lossless dotted semantic text
encoded as a length-delimited byte string (maximum96 bytes); the checkpoint retains its
16-byte persistent identity. Byte admission accounts the exact variable identity length. Masks use bit zero for the first stable chunk in the exact
artifact table. The decoder validates version, content, bounds, sets, anchors, pairing
and complete input consumption before returning detached values. It performs no live
restore or publication. DFR-006.3 owns restore/content reconciliation and aggregate
application of this record with its paired Physics state.

## Qualification

The new tests exercise the real damage owner, Save participant registry and capture
builder, deterministic round trip, detached lifetime, mismatched state, active checkpoint
pairing, cancellation/budget/sink failure, replacement, shutdown, wrong thread, malformed
and trailing input. The public consumer uses only staged public contracts. Local/CI
execution evidence is recorded separately from authored coverage.
