# Scene activation planning

Activation plans and capability checks for bodies and characters. The activation entry point remains in the Physics root.

[PhysicsSceneActivationPlan.cpp](PhysicsSceneActivationPlan.cpp) resolves the authored body and
material inputs. [CharacterSceneActivationPlan.cpp](CharacterSceneActivationPlan.cpp) validates controller
descriptors and their query routes. [PhysicsSceneActivationLifecycle.cpp](PhysicsSceneActivationLifecycle.cpp)
owns preparation, publication, and retirement of a candidate.
