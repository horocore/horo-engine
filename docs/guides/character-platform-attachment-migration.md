# Character movement-base attachment (CHR-003.3)

`CharacterWorld` owns one bounded optional `CharacterPlatformAttachment` in each
committed movement result. It establishes attachment only from the final blocking,
walkable, body-backed support and an exact live body/shape binding. Static and
kinematic bases are eligible; dynamic attachment requires the descriptor's explicit
`allowDynamicPlatformAttachment`. Trigger, overlap, steep, absent-body and jump
evidence cannot establish a base. Selected support is independent of contact-prefix
truncation.

The payload copies body/shape world and slot generations, optional authored child,
the body-local contact point/normal and collision-root pose, sampled body pose,
source tick and Physics publication revision. No native pointer, query reference,
resource lease or durable checkpoint identity is retained. Replacing the body,
shape or authored child emits one `BaseChanged` outcome; unresolved prior binding
emits `Stale` and detaches. Support loss, jump and teleport clear attachment.
New unsupported bindings are `Unavailable`, not an invented identity pose.

Attachment reads consume the existing fixed-tick query budget. Malformed pose,
foreign evidence, stale query context, capacity exhaustion and shutdown abort the
candidate before any controller publication. A failed tick retains the prior owned
snapshot. Replacing/reloading the Character world begins with no attachment; copies
from a retired world remain historical values and must be generation-fenced by
consumers. A tick without a command retains its last controller snapshot and performs
no new query, following the existing command contract.

## Physics evidence is revision-fenced, not a historical snapshot

The canonical adapter uses owner-thread `ReadSceneBodyReconciliation` between joined
Physics steps. This copies **current** native body state, not an independently
retained historical Physics snapshot. `PlatformBody` first requires a completed
Physics publication and the exact captured nonzero revision. The same owner thread
cannot interleave a mutation between that check and the synchronous read; this path
invokes no callback. A successful fixed step or immediate body/binding publication
advances the revision. Stepping, failure, retirement and shutdown close the read.

Before the first completed tick, Physics structural publication intentionally leaves
revision zero unchanged, so the adapter explicitly rejects that pre-publication
case. Queue admission and creation of an unused shape do not change the resident
body pose/binding; they can retain the same revision. Prepared bodies are invisible
until their publication fence. Regression coverage exercises real native body
admission, shape mutation plus solver motion, retirement/replacement and stale
body/shape generations, not just an asserted provider stub.

The existing canonical capsule-query inventory remains fixture-only; this ticket
does not claim authored Scene query projection. Fixture-only or retired/nonresident
support has no scene-body pose mapping and cannot fabricate a platform attachment.
Hosts supplying a Character query adapter must provide copied platform evidence from
the same synchronous world/tick view; absent `platformBody` explicitly reports
attachment unavailability while preserving the existing movement behavior.

## Scope and caller migration

Full platform rotation maps the local contact/root frame without implicitly rotating
the capsule up basis. CHR-003.4 owns swept carry, admitted target/point-velocity
prediction, optional heading inheritance and leave-velocity policy. This ticket does
not apply unswept motion or move Character after Physics.

The existing Physics target owns the appended public fields. In-process consumers
must rebuild. Positional query-context initializers may omit the trailing callback;
movement-result providers must not set `platformAttached` without coherent owned
attachment data. The world derives its own attachment after resolving movement.
Native and public-header consumers cover the expanded contract. No serialized Scene
schema, semantic surface catalog, backend ABI or additional dependency is introduced.
