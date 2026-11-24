# Editor collision settings

Owns the workflow for editing project collision layers, profiles, and query
channels. [CollisionProjectSettingsSession.cpp](CollisionProjectSettingsSession.cpp)
opens the working copy and tracks its revision. Commands edit that copy, and
[ProjectCollisionSettingsStore.cpp](ProjectCollisionSettingsStore.cpp) commits
the reviewed revision through the project settings owner.

- [`project/`](project/) creates the initial collision settings and stable identities.
- [`review/`](review/) inspects durable references and checks that a review is still current at save time.

The [project collision settings guide](../../../docs/guides/collision-project-settings.md)
describes how changes are reviewed and when an active runtime can adopt them.
