# Runtime modules

Use this index to find the component that owns a runtime responsibility. Module READMEs point to implementation, public headers, tests and the relevant architecture. The [architecture index](../../docs/architecture/README.md) defines dependency direction and ownership rules.

| Module | Responsibility |
| --- | --- |
| [AI runtime](ai/README.md) | Owns agent activation, blackboards, decision graphs, perception and environment queries. |
| [Animation](animation/README.md) | Owns animation sources and cooked data, pose evaluation, animation graphs and scene binding. |
| [Runtime assets](assets/README.md) | Owns asset catalog publication, import and cook services, runtime asset loading and the resource lease boundary. |
| [Cinematic runtime](cinematic/README.md) | Owns sequence data, timeline evaluation and camera handoff during cinematic playback. |
| [Runtime console](console/README.md) | Owns command registration and execution, command admission, and runtime console projections. |
| [Destruction runtime](destruction/README.md) | Owns fracture data and the runtime state transitions for breakable objects, with separate cook, save and network adapters. |
| [Frame scheduling](frame/README.md) | Owns runtime phase dispatch, fixed-step accumulation and simulation control. |
| [Runtime input](input/README.md) | Owns input collection, action and accessibility projections, and device state at the runtime boundary. |
| [Navigation](navigation/README.md) | Owns navigation-world lifetime, agent registration, path queries and movement intent. |
| [Networking runtime](networking/README.md) | Owns network session lifecycle, handshake and admission, inbound dispatch and replication identities. |
| [Procedural content generation](pcg/README.md) | Owns procedural point and graph operations and their Scene and Terrain integrations. |
| [Prefab runtime](prefab/README.md) | Owns prefab documents, dependency resolution, cooked templates and transactional instance preparation. |
| [Renderer](renderer/README.md) | Owns rendering contracts, resource and frame lifetime, render-graph execution and the concrete graphics backends. |
| [Save runtime](save/README.md) | Owns save-session preparation, archive encoding and validation, and the restore transaction boundary. |
| [Scene runtime](scene/README.md) | Owns live Scene entities, component state, structural transactions and publication. |
| [Terrain runtime](terrain/README.md) | Owns terrain data, cell streaming and the adapters that connect terrain to scene and rendering. |
| [Runtime UI](ui/README.md) | Owns game UI execution, presentation state, event routing and accessibility at runtime. |
| [VFX runtime](vfx/README.md) | Owns effects and particle simulation contracts, effect instance lifetime and CPU simulation. |
| [World streaming](world_streaming/README.md) | Owns World cell identity, attachment, origin frames and rebase transactions. |
| [XR runtime](xr/README.md) | Owns XR capabilities, session and frame lifecycle, tracking and interaction contracts. |

The main implementations for [Audio](../audio/README.md), [Physics](../physics/README.md) and [Platform services](../platform_services/README.md) sit directly under `src/`. [Accessibility](accessibility/), [Camera](camera/), [Character input](character/input/) and [Runtime host lifecycle](lifecycle/) have smaller implementation folders.

See also [Application services](../application/README.md), [Editor](../editor/README.md), [Extensions](../extensions/README.md), [Foundation](../foundation/README.md), [Gameplay](../gameplay/README.md), [Packages](../packages/README.md), [Platform](../platform/README.md), [Scene source and cook](../scene/README.md) and [UI templates](../ui_templates/README.md).
