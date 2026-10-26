/** @file SceneDocumentHierarchyFixture.h
 * @brief Shared authored parent/mesh fixture for desktop extraction and XR geometry contracts.
 */
#pragma once

#include "SceneDocumentTestSupport.h"

namespace Horo::Editor::SceneDocumentTestSupport {
    struct HierarchySceneObjects {
        Horo::Editor::SceneObjectId parent;
        Horo::Editor::SceneObjectId child;
    };

    [[nodiscard]] inline HierarchySceneObjects MakeHierarchyScene(Horo::Editor::SceneDocument &document) {
        Horo::Editor::EditorHistory history;
        Horo::Editor::SceneDocumentCommandExecutor commands{document, history};
        const auto parent = commands.Execute(Horo::Editor::CreateSceneObjectCommand{
            .name = "Parent",
            .localTransform = Horo::Math::Transform{.translation = {2.0F, 0.0F, 0.0F}},
        });
        REQUIRE((parent.HasValue()));
        const auto child = commands.Execute(Horo::Editor::CreateSceneObjectCommand{
            .name = "Box",
            .parent = parent.Value().object,
            .localTransform = Horo::Math::Transform{.translation = {0.0F, 3.0F, 0.0F}},
            .primitiveMesh = Horo::Runtime::PrimitiveMeshDescriptor{},
        });
        REQUIRE((child.HasValue()));
        return {parent.Value().object, child.Value().object};
    }

}  // namespace Horo::Editor::SceneDocumentTestSupport
