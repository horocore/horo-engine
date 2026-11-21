# Editor workspace

Owns open-document and dock state, dispatches editor commands, and builds the data presented by workspace panels. Editable Scene state lives under [`editor/document/`](../../document/).

| If you are changing | Start here |
| --- | --- |
| Frame coordination or document and panel routing | [Workspace screen](EditorWorkspaceScreen.cpp), [workspace controller](EditorWorkspaceController.cpp) |
| Commands emitted by views or the data panels receive | [Workspace view model](EditorWorkspaceViewModel.h), [command routing](EditorWorkspaceControllerCommandRouting.cpp) |
| Inspector, viewport interaction or workspace dock content | [Panels](panels/), [shared design system](../../design_system/) |
| Decision graphs and environment query authoring | [Decision Graph view](EditorWorkspaceViewDecisionGraph.cpp), [query document](EditorWorkspaceEnvironmentQueryDocument.cpp), [query preview](EditorWorkspaceEnvironmentQuery.cpp) |
| Prefab and terrain document tools | [Prefab tools](prefab/), [terrain tools](terrain/) |

Keep commands typed and let the owning controller validate document revisions before applying changes. Panels render projected state and emit commands. See the [editor document model](../../../../docs/architecture/editor/editor-document-model.md) and [editor tests](../../../../tests/unit/editor/).
