#pragma once
/** @file BuiltinSceneComponents.h
 * @brief Inert stable schema identities used to validate complete typed Scene group projections.
 */
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

#include <optional>
#include <span>
#include <string_view>

namespace Horo::Runtime {
    /** @brief Closed built-in component vocabulary; values never select or instantiate a subsystem backend. */
    enum class BuiltinSceneComponentKind : std::uint8_t {
        PrimitiveMesh,
        Camera,
        Light,
        AudioSource,
        AudioListener,
        UiCanvas,
        NavigationSurface,
        NavigationRegion,
        NavigationModifier,
        NavigationLink,
        NavigationAgent,
        AiAgent,
        AiController,
        CharacterController,
        RigidBody,
        Collider,
        PhysicsConstraint,
        Count
    };

    /** @brief Stable component identity shared by canonical codecs and Scene occurrence validation. */
    struct BuiltinSceneComponentSchema final {
        BuiltinSceneComponentKind kind;
        std::string_view typeId;
    };

    /** @brief Borrows the finite immutable built-in schema table. @return Process-lifetime inert metadata. */
    [[nodiscard]] std::span<const BuiltinSceneComponentSchema> BuiltinSceneComponentSchemas() noexcept;
    /** @brief Looks up one exact stable type identity. @param typeId Canonical identity. @return Kind or absent for a custom type. */
    [[nodiscard]] std::optional<BuiltinSceneComponentKind> FindBuiltinSceneComponent(std::string_view typeId) noexcept;
    /** @brief Counts typed occurrences without filtering disabled durable data.
     * @param components Complete typed values. @param kind Exact built-in kind. @param primitiveMesh Whether the entity owns a typed
     * primitive. @return Bounded occurrence count.
     */
    [[nodiscard]] std::size_t BuiltinSceneComponentCount(const RuntimeComponentSet &components, BuiltinSceneComponentKind kind,
                                                         bool primitiveMesh = false) noexcept;
    /** @brief Counts every typed built-in occurrence. @param components Complete typed values. @param primitiveMesh Whether the entity owns
     * a typed primitive. @return Total occurrence count. */
    [[nodiscard]] std::size_t BuiltinSceneComponentCount(const RuntimeComponentSet &components, bool primitiveMesh = false) noexcept;
}  // namespace Horo::Runtime
