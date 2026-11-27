# Physics state

Owns the formats and validation used to transfer physics state across capture and restore.
[snapshot/](snapshot/) contains the save contract and snapshot leases.
[envelope/](envelope/) contains the wire format and decoding.
[restart/](restart/) prepares restore candidates, and [canonical/](canonical/) handles the native solver
state that a continuation requires. Scene-owner rebinding is described in [scene state](../scene_state/).

- [`canonical/`](canonical/) · Native state continuation
- [`construction/`](construction/) · Physics construction state
- [`envelope/`](envelope/) · State envelopes
- [`identity/`](identity/) · Physics state identity
- [`restart/`](restart/) · Physics restart contracts
- [`runtime/`](runtime/) · Runtime state inventory
- [`snapshot/`](snapshot/) · Physics snapshots
- [`solver/`](solver/) · Solver state codecs
- [`validation/`](validation/) · Physics state validation
