# Headless network and gameplay composition

Owns the product wiring for standalone and networked headless worlds. It selects providers, builds services, and controls lifetime around Scene publication and fixed ticks. Subsystem behavior lives in [runtime modules](../../../../src/runtime/README.md).

| If you are changing | Start here |
| --- | --- |
| Network startup, product session or transport selection | [Product host](NetworkProductHost.cpp), [session](NetworkProductSession.cpp), [transport](NetworkProductTransport.cpp) |
| Committed Scene admission or owner startup and retirement | [Scene load boundary](HeadlessSceneLoadBoundary.cpp), [world services](HeadlessNetworkWorldService.h), [world owners](HeadlessNetworkWorldOwners.h) |
| Gameplay module activation and capability grants | [Gameplay world composition](GameplayWorldComposition.cpp) |
| Committed AI controllers and their environment queries | [Controller admission](HeadlessCookedDecisionAdmission.cpp), [query phase driver](HeadlessDecisionQueries.cpp), [gameplay integration tests](../../../../tests/integration/gameplay/) |

Pass selected capabilities and committed asset generations to their owners explicitly. See the [networking architecture](../../../../docs/architecture/runtime/networking-architecture.md) and [gameplay runtime integration](../../../../docs/architecture/extensions/gameplay-runtime-integration.md) for lifecycle contracts.
