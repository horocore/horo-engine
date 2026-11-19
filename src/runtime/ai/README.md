# AI runtime

Owns agent activation, blackboards, decision graphs, perception and environment queries. Scene composition grants capabilities and publishes agent generations.

- [`api/`](api/) — Agent, blackboard, decision, perception and query contracts.
- [`compile/`](compile/) — Decision graph compilation and runtime graph instances.
- [`integration/`](integration/) — Scene activation, prefab population changes, scheduling, perception and environment query execution.
- [`registration/`](registration/) — AI registration and descriptor contributions.
- [`save/`](save/) — Canonical base-agent capture through the Runtime Save participant boundary.

Use [Navigation](../navigation/README.md) for pathfinding and movement authority, and [Gameplay integration tests](../../../tests/integration/gameplay/) for prefab ownership across subsystems.

## Where to start

| If you are changing | Start here |
| --- | --- |
| Decision task properties or provider admission | [Decision source values](compile/DecisionSourceValues.cpp), [provider admission](compile/DecisionProviderAdmission.cpp), [editor output picker](../../editor/screens/workspace/EditorWorkspaceViewDecisionGraph.cpp) |
| Query authoring or preview in the editor | [Workspace query document](../../editor/screens/workspace/EditorWorkspaceEnvironmentQueryDocument.cpp), [preview and viewport navigation](../../editor/screens/workspace/EditorWorkspaceEnvironmentQuery.cpp) |
| Available query operations and named contexts | [Query registry](api/EnvironmentQueryRegistry.cpp), [context resolution](integration/EnvironmentQueryContexts.cpp) |
| Query execution and result publication to a Decision task | [Query execution](integration/EnvironmentQueryExecution.cpp), [Decision query task](integration/DecisionQueryTask.cpp), [execution tests](../../../tests/unit/runtime/ai/EnvironmentQueryExecutionTests.cpp) |
| Running cooked queries in headless gameplay | [Headless composition](../../../apps/horo-engine/composition/network/README.md), [query phase driver](../../../apps/horo-engine/composition/network/HeadlessDecisionQueries.cpp), [Scene admission tests](../../../tests/integration/gameplay/HeadlessSceneLoadBoundaryTests.cpp) |
| Capturing and restoring the canonical AI population | [Save capture](save/AISaveCapture.cpp), [canonical record codec](compile/AICanonicalStateRecord.cpp), [Scene restore owner](integration/AISceneRestore.cpp) |

Keep authoring UI in the editor and query execution in the runtime. Supply query providers through host composition and resolve them through the registry using their declared identities and capabilities.

[Public headers](../../../include/Horo/AI/) · [Tests](../../../tests/unit/runtime/ai/) · [Architecture](../../../docs/architecture/runtime/navigation-and-ai-architecture.md)
