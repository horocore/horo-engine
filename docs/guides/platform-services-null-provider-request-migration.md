# Platform service request parameter migration

Leaderboard page/window provider entry points now borrow `const LeaderboardRankedQuery&`,
`const LeaderboardAroundSubjectQuery&`, and `const LeaderboardFriendsQuery&` for the call.
Providers that retain a query after returning must copy it into operation-owned storage.
This avoids an unused copy when an explicit Null provider rejects the operation.

Cloud write/delete provider entry points now accept `CloudBlobWriteRequest&&` and
`CloudBlobDeleteRequest&&`. They remain ownership-transfer boundaries: providers must move
an admitted intent into operation-owned storage before returning, preserving its bytes and
idempotency metadata through retirement. They must never retain the argument reference.
Passing an existing named request directly requires `std::move(request)`; passing a temporary
continues to work. The frontend still accepts owned values and forwards them with `std::move`.

Provider implementations must update these five override signatures and rebuild against
the matching headers/library. The provider extension C ABI, request handles, lifecycle,
capability checks, Null rejection codes, and durable mutation semantics are unchanged.
