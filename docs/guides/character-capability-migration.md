# Character client capability (CHR-004.6)

`Horo/Physics/CharacterCapability.h` belongs to `HoroEngine::Physics`. Hosts explicitly
issue a client from a prepared or active `CharacterWorld`, outside placement/tick
callbacks. Issuance does not activate a module, discover a world, select a backend or
grant module permission. GameplayRuntime integration (CHR-004.7) and VM imports remain
separate responsibilities. No existing caller is implicitly switched to a client.

Clients expose only prepared-owner controller creation, tick-addressed movement
admission and copied descriptor/transform/locomotion queries. Active creation remains
`character.state.invalid`; it is not implemented by a fake deferred success. Hosts
retain spawn/teleport, tick scheduling, activation and world ownership. Consumers
receive no mutable registry, Physics scene, native handle or query adapter.

The host may pass its module-generation cancellation token at issuance and explicitly
revoke a client before reload. Copies share one grant. Revocation and parent cancellation
are permanent; replacement consumers receive a fresh grant generation. Access runs on
the Character preparation thread. Only copied identity inspection and revocation are
cross-thread operations; background producers needing direct thread-safe admission
retain the existing explicitly host-owned CharacterWorld queue boundary.

Creation, handle, capacity, command-order and request failures retain their original
typed causes. Inert/moved-from clients return `character.capability.unavailable`;
revoked scopes return `character.capability.revoked`; owner retirement returns
`character.capability.stale`; foreign-thread calls return
`character.thread.affinity_violation` before world access. Clients do not extend world
lifetime. World destruction is owner-thread-only and cannot race client operations;
retirement clears client borrows before controller storage release. Returned snapshots
are bounded owned values and preserve their historical tick, sequence and revisions.

At most 256 live grants are retained per world. Copying a grant does not consume another
slot; expired/revoked grants permit reuse with a strictly increasing, non-wrapping
generation. Grant allocation occurs only at explicit issuance and failures leave the
slot/next identity unchanged. Successful client commands and copied-query/tick paths
allocate no new storage. Queue limits are still the immutable world settings.

Commands carry only copied intent and cancellation state, not a consumer/world pointer.
Command closure removes revoked pending intents before ordering/replacement, including
future-tick intents. It reclaims their queue capacity at that boundary. Once frozen,
commands belong to that attempted tick; callback revocation does not rewrite their
simulation outcome. No cancelled intent is replayed on the next tick. Host-direct
commands have no client cancellation fence and retain their existing semantics.

Regression coverage uses real CharacterWorld creation/admission/publication plus a
native-enabled canonical Physics adapter path, testing errors, bounded grants,
allocation rollback, command replacement/cutoff, revoke/reload, stale slot/world,
cross-thread denial, immutable copied state and shutdown. Execution evidence must be
reported separately; adding these tests does not claim they have run.
