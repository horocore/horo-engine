# Networking runtime

Owns network session lifecycle, handshake and admission, inbound dispatch and replication identities.

- [`api/`](api/) — Network contract implementations.
- [`gns/`](gns/) — GameNetworkingSockets transport integration.
- [`null/`](null/) — Null transport.
- [`NetworkAdmissionController.cpp`](NetworkAdmissionController.cpp) — Runtime admission coordination.

[Public headers](../../../include/Horo/Network/) · [Tests](../../../tests/unit/runtime/networking/) · [Architecture](../../../docs/architecture/runtime/networking-architecture.md)
