# Scene asset integration

[AssetSceneDrop.cpp](AssetSceneDrop.cpp) prepares geometry and previews for prefab drops.
[EditorWorkspaceControllerAssetInstantiation.cpp](EditorWorkspaceControllerAssetInstantiation.cpp)
handles placing assets in the scene, while
[EditorWorkspaceControllerSceneAssets.cpp](EditorWorkspaceControllerSceneAssets.cpp) manages
scene asset state. Asset discovery and editing commands start in the [content browser](browser/README.md).
