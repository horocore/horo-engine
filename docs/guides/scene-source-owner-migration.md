# Shared Scene source ownership

HORO-1253 moves the existing Scene schema-1 value model and canonical codec out of
Editor ownership. This is an ownership migration, not a persisted-format change.

- `HoroEngine::SceneSourceModel` owns `Horo/Scene/SceneSourceModel.h`.
- `HoroEngine::SceneSource` owns `Horo/Scene/SceneSource.h` and the one codec.
- Editor's existing object, component and prefab placement names are aliases to
  those values. Document state/revision, commands, history, mutation leases,
  conflict checks and recovery orchestration remain Editor-owned.
- Headless capture links SceneSource and calls DecodeSceneSource after the host's
  project-compatibility, trust and containment admission. It does not link Editor.
- The non-installed SceneSourceCodecInternal target bridges existing Editor JSON
  projections and navigation-agent conversion. It exposes only the codec's
  dedicated directory, never a repository-wide include root or installed JSON API.

The existing schemaVersion value, fields, primitive/component encodings, optional
legacy defaults and two-space JSON projection are unchanged. The existing
horo.editor.scene_persistence error domain and codes are deliberately retained for
caller compatibility. No frozen release descriptor, project marker or Navigation
record type/payload is changed.

Generated SceneSource/SceneSourceModel public-header consumers cover staged
ownership. SceneSourceTests exercise canonical bytes, typed round trip, legacy
defaults and malformed/version/size rejection without an Editor dependency.
Existing SceneDocumentPersistenceTests continue exercising editor load/save,
round-trip, recovery and path/error behavior through the same codec.

The source-extraction headless configuration (GUI/examples/native Physics/Recast
disabled) built SceneSourceTests, SceneDocumentPersistenceTests and both staged
public-header consumers. Serial focused CTest passed all 38 cases: five shared
source regressions and 33 existing Editor persistence regressions. This is narrow
local extraction evidence, not validation of the pending Navigation automation
or 0.2.0 integration. The manager build slot was released after completion.
