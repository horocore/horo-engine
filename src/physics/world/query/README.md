# Physics world queries

World query admission, query capabilities, and query contract validation.

[PhysicsQuery.cpp](PhysicsQuery.cpp) validates query geometry, handles, and request limits.
[PhysicsWorldQuery.cpp](PhysicsWorldQuery.cpp) is the synchronous world entry point.
[PhysicsQueryEventCapability.cpp](PhysicsQueryEventCapability.cpp) handles client admission
and deferred query batches.
