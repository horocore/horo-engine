# Physics snapshots

Snapshot slots, preparation, leases, and the private save contract.

[PhysicsStateSnapshotPreparation.cpp](PhysicsStateSnapshotPreparation.cpp) reserves bounded slot
storage. [PhysicsStateSnapshotLease.cpp](PhysicsStateSnapshotLease.cpp) pins a slot while a reader
uses it. [PhysicsStateSaveContract.cpp](PhysicsStateSaveContract.cpp) defines the participant,
record, and descriptor used at the Save boundary.
