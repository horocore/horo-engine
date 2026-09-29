# Horo Engine

[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=horocore_horo-engine&metric=alert_status)](https://sonarcloud.io/summary/new_code?id=horocore_horo-engine)
[![Coverage](https://sonarcloud.io/api/project_badges/measure?project=horocore_horo-engine&metric=coverage)](https://sonarcloud.io/summary/new_code?id=horocore_horo-engine)
[![Bugs](https://sonarcloud.io/api/project_badges/measure?project=horocore_horo-engine&metric=bugs)](https://sonarcloud.io/summary/new_code?id=horocore_horo-engine)
[![Code Smells](https://sonarcloud.io/api/project_badges/measure?project=horocore_horo-engine&metric=code_smells)](https://sonarcloud.io/summary/new_code?id=horocore_horo-engine)
[![Vulnerabilities](https://sonarcloud.io/api/project_badges/measure?project=horocore_horo-engine&metric=vulnerabilities)](https://sonarcloud.io/summary/new_code?id=horocore_horo-engine)

> [!WARNING]
> Horo Engine is under active development.
> The repository does not currently provide a stable public release, production
> SDK, or ready-to-use editor build. APIs, module boundaries, and project layout
> may change without backward compatibility guarantees until the first stable
> release line is established.

Horo Engine is an open-source C++20 game engine and AI-centric editor IDE.

The current focus is building the engine around a clean modular architecture,
explicit ownership boundaries, deterministic tooling, and editor-native AI
workflows that reason from engine data instead of treating the editor as a black
box.

## Repository Layout Target

The active repository is being organized around this high-level structure:

```text
horo-engine/
├── include/Horo/      # public engine API
├── src/               # engine implementation
├── apps/              # editor and CLI application entry points
├── sdk/               # SDK templates, schemas, and integration files
├── examples/          # sample projects, packages, and extensions
├── tests/             # unit, integration, UI, and smoke tests
├── scripts/           # repository automation scripts
├── tools/             # standalone developer tools
├── cmake/             # CMake modules
├── docs/              # architecture and design documentation
└── vendor/            # third-party dependency location
```

See [Desired Project Tree](./docs/architecture/desired-project-tree.md) for the
full target structure.

## Build Status

Configure and build the canonical local development matrix from the repository
root:

```bash
cmake -S . -B build/skeleton -DBUILD_TESTING=ON
cmake --build build/skeleton --parallel
```

`HORO_BUILD_PHYSICS_NATIVE` defaults to `ON` for the pinned private CPU solver.
`HORO_BUILD_NAVIGATION_RECAST_DETOUR` defaults to `ON` for the pinned private grounded
navigation query provider; set it to `OFF` for compositions that deliberately omit the
real provider dependency.
Set it to `OFF` for a Physics-omitted composition; the typed descriptor API still
builds, but the native compatibility check reports unavailable. Linking Physics
does not register native types, initialize a world or activate simulation.

## Tests

Install the repository-owned Python test dependency before configuring a test
build, preferably inside a virtual environment:

```bash
python3 -m pip install -r scripts/requirements.txt
```

Run the normal test suite without display/GPU-dependent tests:

```bash
ctest --test-dir build/skeleton -LE gpu --output-on-failure
```

Run every configured test:

```bash
ctest --test-dir build/skeleton --output-on-failure
```

GPU smoke and editor first-frame tests are opt-in and require a compatible
display and graphics device. Enable them while configuring:

```bash
cmake -S . -B build/skeleton \
  -DBUILD_TESTING=ON \
  -DHORO_ENABLE_GPU_SMOKE_TESTS=ON
cmake --build build/skeleton --parallel
ctest --test-dir build/skeleton -L gpu --output-on-failure
```

Filter CTest by target or test name:

```bash
ctest --test-dir build/skeleton -R HoroProjectMigrationTests --output-on-failure
ctest --test-dir build/skeleton -N
```

Catch2 executables also support direct case and tag selection:

```bash
./build/skeleton/tests/HoroProjectMigrationTests \
  "Pipeline Requires Terminal Validation"
./build/skeleton/tests/HoroProjectMigrationTests "[unit][application]"
```

Run Python tooling tests directly through pytest:

```bash
python3 -m pytest tests/python
```

See [Testing Architecture](./docs/architecture/delivery/testing-architecture.md)
for the headless matrix, test layers, tags, fixtures, GPU requirements, and test
authoring rules.

## AI-Centric Editor Direction

Horo's editor architecture is designed for agents that operate close to the
engine rather than guessing from screenshots. The intended AI model is
numeric-first:

- agents query typed engine/editor state through MCP
- transforms, bounds, raycasts, contacts, animation curves, and telemetry are
  preferred over raw visual data
- visual captures are secondary and should be annotated with entity IDs and
  measurements
- viewport inline chat and magic AI tools should execute through typed,
  undoable editor commands

See:

- [Editor AI Agent Architecture](./docs/architecture/editor/editor-ai-agent-architecture.md)
- [MCP Architecture](./docs/architecture/interfaces/mcp-architecture.md)
- [Desired Project Tree](./docs/architecture/desired-project-tree.md)

## Documentation

| Area                         | Document                                                                                                               |
|------------------------------|------------------------------------------------------------------------------------------------------------------------|
| Architecture index           | [docs/architecture/README.md](./docs/architecture/README.md)                                                           |
| Desired repository layout    | [docs/architecture/desired-project-tree.md](./docs/architecture/desired-project-tree.md)                               |
| Editor AI agent architecture | [docs/architecture/editor/editor-ai-agent-architecture.md](./docs/architecture/editor/editor-ai-agent-architecture.md) |
| Release architecture         | [docs/architecture/release/release.md](./docs/architecture/release/release.md)                                         |
| CLI architecture             | [docs/architecture/interfaces/cli-architecture.md](./docs/architecture/interfaces/cli-architecture.md)                 |
| MCP architecture             | [docs/architecture/interfaces/mcp-architecture.md](./docs/architecture/interfaces/mcp-architecture.md)                 |
| Testing architecture         | [docs/architecture/delivery/testing-architecture.md](./docs/architecture/delivery/testing-architecture.md)             |
| HoroEditor Grafana dashboard | [observability/grafana/README.md](./observability/grafana/README.md)                                                    |
| ADRs                         | [docs/adr/README.md](./docs/adr/README.md)                                                                             |

### Topics needs to be covered

| Topics                                                                                | Notes |
|---------------------------------------------------------------------------------------|-------|
| We need to create an example module with a panel                                      |       |
| We should prepare an agent mode with vierport that shows real agents as camera on gui |       |

## License

[MIT](LICENSE).
