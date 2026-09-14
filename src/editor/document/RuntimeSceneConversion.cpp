#include "editor/document/RuntimeSceneConversion.h"

#include <algorithm>
#include <iterator>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId SceneConversionDomain{"horo.editor.scene_conversion"};
        const ErrorCodeDescriptor PrefabResolutionRequired{
            .domain = SceneConversionDomain,
            .code = ErrorCode{"scene_conversion.prefab_resolution_required"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene prefab instances require a pinned resolver snapshot before runtime conversion.",
            .remediationHint = "Resolve every authored prefab instance before preparing a runtime scene candidate.",
            .retryable = true,
            .userActionable = false,
        };

        template <typename Component> [[nodiscard]] std::optional<Component> ActiveComponent(const std::optional<Component> &component) {
            return component.has_value() && component->enabled ? component : std::nullopt;
        }

        template <typename Component> [[nodiscard]] std::vector<Component> ActiveComponents(const std::vector<Component> &components) {
            std::vector<Component> active;
            active.reserve(components.size());
            std::ranges::copy_if(components, std::back_inserter(active), &Component::enabled);
            return active;
        }
    }  // namespace

    /** @copydoc ConvertSceneDocumentToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertSceneDocumentToRuntime(const SceneDocumentSnapshot &document,
                                                                          const Runtime::SceneDefinitionId sceneId) {
        if (!document.prefabInstances.empty())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(PrefabResolutionRequired));
        Runtime::SceneDefinitionBuilder builder{sceneId, Runtime::SceneDefinitionRevision{document.state.value}};
        for (const SceneObjectSnapshot &object : document.objects) {
            const Runtime::RuntimeComponentSet components{
                .camera = ActiveComponent(object.components.camera),
                .light = ActiveComponent(object.components.light),
                .triggerVolume = ActiveComponent(object.components.triggerVolume),
                .audioSource = ActiveComponent(object.components.audioSource),
                .navigationSurface = ActiveComponent(object.components.navigationSurface),
                .navigationRegion = ActiveComponent(object.components.navigationRegion),
                .navigationModifier = ActiveComponent(object.components.navigationModifier),
                .navigationLink = ActiveComponent(object.components.navigationLink),
                .rigidBody = ActiveComponent(object.components.rigidBody),
                .colliders = ActiveComponents(object.components.colliders),
                .physicsConstraints = ActiveComponents(object.components.physicsConstraints),
                .behaviors = object.components.behaviors,
            };
            builder.Add(Runtime::RuntimeEntityDefinition{
                .object = Runtime::SceneObjectId{object.id.value},
                .parent = object.parent ? std::optional{Runtime::SceneObjectId{object.parent->value}} : std::nullopt,
                .localTransform = object.localTransform,
                .primitiveMesh = object.primitiveMesh,
                .components = components,
            });
        }
        return std::move(builder).Build();
    }
}  // namespace Horo::Editor
