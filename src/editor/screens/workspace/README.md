# Editor workspace

The workspace owns the active document and dock state, routes editor commands, and projects
data for panels. Start with [EditorWorkspaceScreen.cpp](EditorWorkspaceScreen.cpp) for frame
coordination, [EditorWorkspaceController.cpp](EditorWorkspaceController.cpp) for workspace state,
or [EditorWorkspaceViewModel.h](EditorWorkspaceViewModel.h) for data and commands shared by views.
Editable Scene state lives in the [editor document model](../../document/).

| If you are changing | Start here |
| --- | --- |
| Host composition, input routing, or project lifecycle | [Screen](screen/README.md) |
| View command dispatch or admitted automation operations | [Commands](commands/README.md) |
| Docked panels, geometry, or layout persistence | [Docking](docking/README.md), [layout](layout/README.md), [panel guides](panels/README.md) |
| Scene objects, viewport interactions, or agent inspection | [Scene](scene/README.md), [viewport](viewport/README.md), [inspection](inspection/README.md) |
| Asset drops, discovery, or source file workflows | [Assets](assets/README.md), [content browser](assets/browser/README.md), [source](source/README.md) |
| AI graphs, blackboards, or environment queries | [AI authoring](ai/README.md) |
| Prefabs, terrain, or world composition | [Prefab](prefab/README.md), [terrain](terrain/README.md), [world](world/README.md) |
| Specialized authoring tools | [Navigation](navigation/README.md), [PCG](pcg/README.md), [sequence](sequence/README.md), [UI canvas](ui_canvas/README.md) |
| Play, project settings, or extension contributions | [Gameplay](gameplay/README.md), [project settings](project_settings/README.md), [extensions](extensions/README.md) |

Panels render projected state and emit typed commands. The owning controller validates document
revisions before applying changes. See the [editor document architecture](../../../../docs/architecture/editor/editor-document-model.md),
the [shared design system](../../design_system/), and the [editor tests](../../../../tests/unit/editor/).
