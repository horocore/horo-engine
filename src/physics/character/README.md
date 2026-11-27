# Character controller

Owns controller state, movement commands, and character queries. Start with
[CharacterWorld.cpp](runtime/CharacterWorld.cpp) for world ownership and
[CharacterWorldCommandPipeline.cpp](runtime/CharacterWorldCommandPipeline.cpp) for command execution.
Clearance queries use the physics query interface, and platform attachment tracks movement with a supporting body.

- [`controller/`](controller/) · Controller contracts
- [`diagnostics/`](diagnostics/) · Character diagnostics
- [`events/`](events/) · Character events
- [`movement/`](movement/) · Character movement
- [`platform/`](platform/) · Moving platform attachment
- [`query/`](query/) · Character queries
- [`runtime/`](runtime/) · Character world runtime
