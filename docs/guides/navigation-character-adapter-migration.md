# Navigation Character desired-motion composition

NAV-004.9 adds `HoroEngine::NavigationCharacterIntegration` and
`Horo/Navigation/NavigationCharacterAdapter.h`. Existing NavigationRuntime and
CharacterInput APIs retain their callers. Hosts that previously forwarded generic
ExternalIntent frames manually can opt into this bridge; do not retain both paths
as competing writers for the same Character tick.

1. Prepare the normal Navigation movement/velocity owners and Character world.
   Issue a **dedicated** Character capability and permitted ExternalIntent principal.
   Compose `NavigationCharacterAdapter::Create` with the exact controller, Navigation
   Scene/world binding and the same nonzero Character revision as the velocity
   pipeline. The velocity pipeline and its movement owner outlive the bridge.
2. Capture crowd motion from the committed Character root. Run ordinary Navigation
   Prepare/solve/Commit. Call bridge `Prepare(publication, observation, correlation)`
   once, then `Submit(work, freshObservation)` immediately before Character admission
   closes. The host supplies a one-based simulation tick and strictly increasing
   correlation; the original sequence reaches Character unchanged. A present zero
   remains an explicit stop, never absent intent.
3. For full/busy, retain and retry **the same immutable work** only while its tick,
   publication and root remain current. Do not recapture the tick or add a hidden
   adapter queue. Typed errors preserve their original domain. Rejection authorizes
   no movement and the host applies its explicit stop policy when appropriate.
4. Advance Character with the exact same fixed tick and quantum declared by the
   Navigation observation. Character owns root motion, ground/step resolution and
   Physics publication. Call `ObserveFeedback()` before the next Navigation Prepare
   or another Character tick. This returns a complete copied locomotion snapshot,
   and feeds actual achieved movement into existing bounded blockage/repath policy.
   It does not feed desired velocity back as if it had been achieved.
5. For cancellation, collect the already admitted result if still current, or
   explicitly `DiscardFeedback()` before replacing the goal. This does not recall
   the Character command. On teleport/controller replacement/reload, first close
   the bridge, then reset/rebind Navigation with a newer Character revision and
   compose a fresh bridge and grant. Old work and historical post-teleport feedback
   fail closed. Finally retire Navigation and drain existing jobs under their own
   lifecycle contracts.

The new target is downstream of NavigationRuntime and CharacterInput. No Nav type
or dependency was added to Character or Physics. Public consumer compilation covers
that boundary. Adapter regression tests cover real Character queue admission and
publication, full retry, stale/cancelled/revoked evidence, exact command correlation,
explicit stops, and feedback-driven bounded repath; scripted solver outcomes are
identified in the tests and do not certify a native Physics backend or GPU run.
