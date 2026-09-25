# Gameplay Module Architecture

## Purpose

This document is the overview for project-owned gameplay module architecture. It defines the high-level boundaries and points to focused companion documents for module loading, behavior authoring, runtime integration, and verification.

Use **gameplay module** for project-owned code and descriptors built with a game
project. Use **extension package** for installable packages handled by the
[Extension System](./plugin-system.md). Gameplay modules may expose runtime
descriptors, but they are not marketplace extension packages and do not use the
long-term extension C ABI.

## Core Decisions

- Native C++ game code is a project-owned module built against the public Horo SDK targets.
- The editor experience supports object-attached behavior authoring: write behavior code, build or reload it, then attach it to a scene object through the editor without manual engine integration.
- Script-authored and visual-authored behaviors are first-class authoring goals and use the same behavior descriptor, lifecycle, capability, and scene mutation boundary as native C++ behaviors.
- Gameplay systems use scene/runtime APIs and cannot depend on editor, GUI, MCP, or concrete renderer backends.
- Registration is declarative and complete before a runtime scene becomes active.
- Gameplay state lives in runtime scenes or game-owned services, not process globals.
- Native code hot reload is optional development behavior and never a shipping requirement.
- A project has one primary native gameplay module in the initial contract; additional gameplay modules, mods, or script packages are future extension points.

## Document Map

- [Current Gameplay Module Contract Audit](./gameplay-module-contract-audit.md):
  implementation snapshot covering the exact SDK ABI, generated bundle, build
  and publication artifacts, ownership, editor reload, persistence, platform
  behavior, evidence gaps, and focused follow-up ownership.
- [Gameplay Module Boundary](./gameplay-module-boundary.md): native module ABI, ownership, registration, capability context, services, hot reload, and diagnostics.
- [Gameplay Behavior Authoring](./gameplay-behavior-authoring.md): editor/IDE workflow, build integration, object-attached behaviors, scripted behaviors, visual scripting, and iteration-speed goals.
- [Gameplay Runtime Integration](./gameplay-runtime-integration.md): game-owned asset types, input actions, runtime systems, scene/play lifecycle, component persistence, and deferred runtime extension points.
- [Gameplay Module Verification](./gameplay-module-verification.md): required contract and regression coverage.
- [Game Project Testing](../delivery/game-project-testing.md): downstream game-owned unit, play-mode, content, visual, and packaged-player tests.

## Boundary Summary

Gameplay modules expose descriptors and runtime behavior through public SDK contracts. They do not get unrestricted access to application internals, editor state, renderer backend objects, global service lookup, or platform APIs. Editor tooling and build tooling may generate descriptors from project assets or annotated native code, but runtime activation consumes validated descriptors through the same registries.

Object-attached behaviors follow generated descriptor discovery rather than one
manual C++ registration entry per behavior:

```text
src/gameplay/DoorControllerBehavior.cpp
  -> NativeBehaviorScanner
  -> generated BehaviorDescriptor
  -> BehaviorRegistry

assets/scripts/DoorController.horo_script
assets/scripts/DoorController.horo_script.meta
  -> ScriptBehaviorScanner
  -> generated BehaviorDescriptor
  -> BehaviorRegistry
```

Native/static non-behavior descriptors that cannot be discovered from project
assets use the native module registration path described in
[Gameplay Module Boundary](./gameplay-module-boundary.md). Object-attached
behaviors do not use manual project registration.

## Related Documents

- [Gameplay Integration Config UI Reference](../../../mock-studio/designs.md#architecture-extensions-module-config): game libraries, package contributions, services, script runtime, and verification panel.

- [System Design](../foundation/system-design.md)
- [Scene Runtime](../runtime/scene-runtime.md)
- [Runtime Lifecycle](../runtime/runtime-lifecycle.md)
- [Asset Pipeline](../runtime/asset-pipeline.md)
- [Input Architecture](../runtime/input-architecture.md)
- [Runtime Debug Console And Development Overlays](../runtime/debug-console-and-overlays.md)
- [Build System](../delivery/build-system.md)
- [Developer Environment](../delivery/developer-environment.md)
- [Game Project Testing](../delivery/game-project-testing.md)
- [Platform Abstraction](../foundation/platform-abstraction.md)
- [Ownership And Resource Lifetime](../foundation/ownership-and-resource-lifetime.md)
- [Observability Performance](../observability/observability-performance.md)
- [Editor Document Model](../editor/editor-document-model.md)
- [Extension System](./plugin-system.md)
