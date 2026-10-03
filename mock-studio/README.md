# Horo Mock Studio

An independent React/Vite workspace for Horo editor design work. It is not a
CMake target, engine dependency, editor runtime, or production UI package.

```bash
cd mock-studio
npm ci
npm run dev
```

`npm run build` checks documentation links, type-checks, server-renders every
registered screen, and produces a static build in `dist/`. The studio has a
searchable catalog, side-by-side comparison, and a shared modal host. The
[design index](designs.md) lists all 43 screens. Existing deep links such as
`#design=architecture/editor/editor-workspace.html` remain stable design IDs;
the suffix does not load an HTML file.

## Structure

- `src/catalog.json` owns design identity, titles, and groups. Every entry must
  have a native React screen registered in `src/screens/registry.tsx`.
- `src/screens/` owns screen components. Related tool panels share typed layouts
  such as `ThreeColumnPanel`, `TabbedFormDesign`, `SimpleWorkbench`, and
  `SemanticDesign`. The graph editors share `WorkspaceShell` and `GraphCanvas`.
- `src/fixtures/` owns editable design data for those shared layouts. Build and
  runtime paths read fixtures directly; no HTML parsing occurs.
- `src/design-system/` owns reusable workspace and graph controls.
- `public/assets/` contains fonts and static assets used by the React app.

New design work belongs in typed components under `src/`, backed by shared
tokens and fixture data. Architecture documents in `docs/` describe contracts;
they do not own mock code.
