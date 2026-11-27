# UI templates

Owns reusable UI template artifacts, their catalog and binding data, and the limits used to admit template work.

Start at [`UiTemplateArtifact.cpp`](UiTemplateArtifact.cpp) for the artifact, [`UiTemplateCatalog.cpp`](UiTemplateCatalog.cpp) for lookup, and [`UiTemplateBindingCodec.cpp`](UiTemplateBindingCodec.cpp) for binding serialization. The [runtime UI](../runtime/ui/README.md) owns live UI state and input.

[Public headers](../../include/Horo/UiTemplates/) · [Tests](../../tests/unit/ui_templates/) · [Architecture](../../docs/architecture/runtime/game-ui-and-hud.md)
