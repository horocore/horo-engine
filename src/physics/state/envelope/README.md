# State envelopes

Envelope decoding, wire helpers, hashes, and schema accounting for physics state.

[PhysicsStateEnvelope.cpp](PhysicsStateEnvelope.cpp) writes snapshot sections.
[PhysicsStateEnvelopeDecode.cpp](PhysicsStateEnvelopeDecode.cpp) inspects and validates the wire
layout before decoding into prepared storage. [PhysicsStateSchemaAccounting.cpp](PhysicsStateSchemaAccounting.cpp)
checks fixed and variable storage costs.
