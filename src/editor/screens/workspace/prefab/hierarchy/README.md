# Prefab hierarchy adapters

[PrefabHierarchyProjection.cpp](PrefabHierarchyProjection.cpp) projects prefab-local objects
for hierarchy display. [PrefabHierarchySources.cpp](PrefabHierarchySources.cpp) supplies the source
data, and [PrefabHierarchyCommand.cpp](PrefabHierarchyCommand.cpp) adapts panel edits to source-owned
object identities. The [document session](../document/README.md) owns the authoritative source.
