# Collision reference review

[CollisionReferenceReview.cpp](CollisionReferenceReview.cpp) checks whether
the proposed settings would invalidate durable references.
[CollisionReviewFreshness.cpp](CollisionReviewFreshness.cpp) verifies that the
session and reference inventory still match the reviewed evidence before a save.

[`CollisionSourceInventory.cpp`](CollisionSourceInventory.cpp) validates the
retained asset inventory. [NativeAssetRegistrySource.cpp](NativeAssetRegistrySource.cpp)
connects the editor workflow to the project-owned registry source.
