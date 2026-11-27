# Physics inspector

[InspectorPanelPhysics.cpp](InspectorPanelPhysics.cpp) coordinates inspector rendering for
physics components. Separate implementations render body, collider, constraint, and character controls.
The [editing helpers](editing/README.md) hold drafts and shared widgets before the inspector submits
a complete gesture to the host.
