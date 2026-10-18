#include "Horo/Runtime/Scene/BuiltinSceneComponents.h"

#include <array>

namespace Horo::Runtime {
    namespace {
        constexpr std::array
            schemas{BuiltinSceneComponentSchema{BuiltinSceneComponentKind::PrimitiveMesh, "game.horo.primitive_mesh"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::Camera, "game.horo.camera"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::Light, "game.horo.light"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::AudioSource, "game.horo.audio_source"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::AudioListener, "game.horo.audio_listener"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::UiCanvas, "game.horo.ui_canvas"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::NavigationSurface, "game.horo.navigation_surface"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::NavigationRegion, "game.horo.navigation_region"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::NavigationModifier, "game.horo.navigation_modifier"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::NavigationLink, "game.horo.navigation_link"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::NavigationAgent, "game.horo.navigation_agent"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::AiAgent, "game.horo.ai_agent"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::AiController, "game.horo.ai_controller"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::CharacterController, "game.horo.character_controller"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::RigidBody, "game.horo.rigid_body"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::Collider, "game.horo.collider"},
                    BuiltinSceneComponentSchema{BuiltinSceneComponentKind::PhysicsConstraint, "game.horo.physics_constraint"}};
    }

    /** @copydoc BuiltinSceneComponentSchemas */
    std::span<const BuiltinSceneComponentSchema> BuiltinSceneComponentSchemas() noexcept {
        return schemas;
    }

    /** @copydoc FindBuiltinSceneComponent */
    std::optional<BuiltinSceneComponentKind> FindBuiltinSceneComponent(const std::string_view typeId) noexcept {
        for (const auto &schema : schemas)
            if (schema.typeId == typeId)
                return schema.kind;
        return std::nullopt;
    }

    /** @copydoc BuiltinSceneComponentCount */
    std::size_t BuiltinSceneComponentCount(const RuntimeComponentSet &components, const BuiltinSceneComponentKind kind,
                                           const bool primitiveMesh) noexcept {
        using enum BuiltinSceneComponentKind;
        switch (kind) {
            case PrimitiveMesh:
                return primitiveMesh ? 1U : 0U;
            case Camera:
                return components.camera.has_value() ? 1U : 0U;
            case Light:
                return components.light.has_value() ? 1U : 0U;
            case AudioSource:
                return components.audioSource.has_value() ? 1U : 0U;
            case AudioListener:
                return components.audioListener.has_value() ? 1U : 0U;
            case UiCanvas:
                return components.uiCanvas.has_value() ? 1U : 0U;
            case NavigationSurface:
                return components.navigationSurface.has_value() ? 1U : 0U;
            case NavigationRegion:
                return components.navigationRegion.has_value() ? 1U : 0U;
            case NavigationModifier:
                return components.navigationModifier.has_value() ? 1U : 0U;
            case NavigationLink:
                return components.navigationLink.has_value() ? 1U : 0U;
            case NavigationAgent:
                return components.navigationAgent.has_value() ? 1U : 0U;
            case AiAgent:
                return components.aiAgent.has_value() ? 1U : 0U;
            case AiController:
                return components.aiController.has_value() ? 1U : 0U;
            case CharacterController:
                return components.characterController.has_value() ? 1U : 0U;
            case RigidBody:
                return components.rigidBody.has_value() ? 1U : 0U;
            case Collider:
                return components.colliders.size();
            case PhysicsConstraint:
                return components.physicsConstraints.size();
            case Count:
                return 0;
        }
        return 0;
    }

    /** @copydoc BuiltinSceneComponentCount */
    std::size_t BuiltinSceneComponentCount(const RuntimeComponentSet &components, const bool primitiveMesh) noexcept {
        std::size_t total{};
        for (const auto &schema : schemas)
            total += BuiltinSceneComponentCount(components, schema.kind, primitiveMesh);
        return total;
    }
}  // namespace Horo::Runtime
