# Convex hull and compound collision

This directory owns convex hull and compound cooking, plus the matching cooked artifact codecs. Compound cooking reuses the convex hull implementation for hull leaves.

Start with `PhysicsConvexHullCook.cpp` for a hull and `PhysicsCompoundCook.cpp` for a compound. The corresponding `*Artifact.cpp` files own their cooked payload formats.
