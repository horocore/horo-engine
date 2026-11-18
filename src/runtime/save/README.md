# Save runtime

Owns save-session preparation, archive encoding and validation, and the restore transaction boundary.

- [`RuntimeSaveSession.cpp`](RuntimeSaveSession.cpp) — Save-session owner.
- [`RuntimeSaveSessionPreparation.cpp`](RuntimeSaveSessionPreparation.cpp) — Detached save and restore preparation.
- [`SaveArchiveReader.cpp`](SaveArchiveReader.cpp) — Archive reading and validation.
- [`SaveArchiveContainerWriter.cpp`](SaveArchiveContainerWriter.cpp) — Archive encoding.

[Public headers](../../../include/Horo/Runtime/Save/) · [Tests](../../../tests/unit/runtime/save/) · [Architecture](../../../docs/architecture/runtime/save-game-and-persistence.md)
