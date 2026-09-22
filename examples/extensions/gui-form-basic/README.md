# Basic Declarative Form Extension

This reference extension builds a non-trivial settings form from the public
`Horo/Extensions/EditorUiForm.h` primitive set. It contains no Dear ImGui, SDL,
renderer, editor-global state, extension-owned colors, or persistence logic.

The same owned form is projected into the semantic-token render snapshot used
by GUI and headless adapters. The owning configuration service remains
responsible for accepting or rejecting value changes and for persistence.

Build it with `HORO_BUILD_EXAMPLES=ON`. The executable validates the form and a
headless render projection at startup; the copied `extension.json` documents the
corresponding `editor.settings_page` contribution.
