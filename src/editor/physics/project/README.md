# Project collision defaults

[ProjectCollisionTemplate.cpp](ProjectCollisionTemplate.cpp) builds the initial
layers, profiles, and channels during project creation.
[CollisionIdentityGeneration.cpp](CollisionIdentityGeneration.cpp) issues stable
identities for the template and subsequent edits. Names and display order can
change while authored references keep the same identity.

The [collision authoring session](../CollisionProjectSettingsSession.cpp)
uses these helpers when adding entries to an existing working copy.
