# Read-only friends API migration

PLS-006.6 changes the C++ friends projection because the old
`FriendPresentation::subject` used the same capability as the local player.
That allowed social presentation to be passed as a local service subject and did
not bind social identity to the requesting access revision.

`HoroEngine::PlatformServices` owns the new `PlatformFriendsCoordinator.h` header
and the existing service contracts. `HoroPublicHeaderOwnership.cmake` records the
sole ownership. `HoroPlatformFriendsPublicConsumer` includes only the staged public
headers through that target and verifies the separate local/social types.

Provider adapters returning `FriendsPage` and presentation callers must migrate:

1. Use `PlatformSocialSubjectHandle` for each social row. The protected adapter's
   identity broker supplies a cryptographic random `PlatformSubjectNonce` to
   `BuildPlatformSocialSubjectHandle` with the captured requesting session.
   Do not copy a native identifier, credential or a hash of a name/account into it.
2. Set `FriendsQuery::startIndex` and echo it in `FriendsPage::startIndex`.
   Existing first-page aggregate queries retain offset zero. New offsets and
   sizes must fit in the finite provider limits without unsigned overflow.
3. Compose `PlatformFriendsCoordinator` with the complete policy-projected provider
   capabilities and the matching published session. Keep mutation on the owner
   lane and forward its committed session/access snapshots before query dispatch
   or presentation. Recreate the coordinator on provider-generation replacement;
   an old capability binding cannot authorize a new provider.
4. Call `BeginQuery`, retain the exact query token, and pass the normalized provider
   result to `Complete`. A failure remains a typed failure. A malformed or oversized
   page publishes no partial rows. A successful empty page means an approved query
   returned no rows; it never represents denied access.
5. On cancellation, invalidate the token with `Cancel` before requesting bounded
   native cancellation. Session/access updates and `Close` also invalidate pending
   tokens. The adapter still owns native callback/code/credential retirement;
   invalidating a token does not authorize freeing a live callback context.
6. Obtain presentation copies through `ReadPage` immediately before exposure.
   Do not retain an earlier copied page across a session/access change or expiry.
   Names are untrusted UTF-8: render them as text, never interpret markup or links.
   Do not serialize, log, export or use social handles/names as gameplay identity.

The coordinator retains at most one page and one query token. Default limits are
128 rows, 256 UTF-8 bytes per name, 32 KiB total name bytes and 30 seconds retention,
further restricted by provider limits. No disk cache, analytics export, avatar
fetching, friend management or invite API is introduced. Its memory is ordinary
process memory and is not a secure credential store.

There is no proprietary SDK or native friends wire schema in these contracts.
Private provider packages must adapt their C++ friends projections and separately
qualify native mapping, consent, parental/region policy and account-switch races.
