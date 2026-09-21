#pragma once

/**
 * @file Component.h
 * @brief Stable identities and serialization metadata for project-owned gameplay components.
 */

#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Gameplay {
    inline constexpr std::size_t MaximumComponentTypeIdBytes = 160;
    inline constexpr std::size_t MaximumComponentPropertyIdBytes = 96;
    inline constexpr std::size_t MaximumComponentProperties = 256;
    inline constexpr std::size_t MaximumSerializedComponentBytes = 1024 * 1024;
    inline constexpr std::size_t MaximumSerializedComponentsPerObject = 128;

    /** @brief Stable persistent identity of one project-owned component type. */
    class ComponentTypeId final {
    public:
        ComponentTypeId() = default;

        /**
         * @brief Parses a project-namespaced component identity.
         * @param value Identifier using the `game.<module>.<component>` namespace.
         * @return Valid identity or a typed validation error.
         */
        [[nodiscard]] static Result<ComponentTypeId> Parse(std::string_view value);

        /** @brief Returns the persistent identifier text. */
        [[nodiscard]] const std::string &Value() const noexcept;
        /** @brief Reports whether this value contains a parsed identity. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const ComponentTypeId &) const noexcept = default;

    private:
        explicit ComponentTypeId(std::string value) : value_(std::move(value)) {}

        std::string value_;
    };

    /** @brief Stable serialized identity of one component property. */
    class ComponentPropertyId final {
    public:
        ComponentPropertyId() = default;

        /**
         * @brief Parses a property identity that remains stable across display-name changes.
         * @param value Lowercase identifier containing letters, digits, and underscores.
         * @return Valid identity or a typed validation error.
         */
        [[nodiscard]] static Result<ComponentPropertyId> Parse(std::string_view value);

        /** @brief Returns the persistent property identifier text. */
        [[nodiscard]] const std::string &Value() const noexcept;
        /** @brief Reports whether this value contains a parsed identity. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const ComponentPropertyId &) const noexcept = default;

    private:
        explicit ComponentPropertyId(std::string value) : value_(std::move(value)) {}

        std::string value_;
    };

    /** @brief Language-neutral field kind used by generic authoring and inspection surfaces. */
    enum class ComponentPropertyKind : std::uint8_t {
        Boolean,
        SignedInteger,
        Number,
        String,
        Vec2,
        Vec3,
        Quaternion,
    };

    /** @brief Declarative serialization and authoring metadata for one stable property. */
    struct ComponentPropertyDescriptor {
        ComponentPropertyId id;
        std::string displayName;
        ComponentPropertyKind kind{ComponentPropertyKind::String};
        bool required{false};
    };

    /** @brief One deterministic forward schema edge supported by a component descriptor. */
    struct ComponentMigrationDescriptor {
        std::uint32_t fromSchemaVersion{};
        std::uint32_t toSchemaVersion{};
        [[nodiscard]] bool operator==(const ComponentMigrationDescriptor &) const noexcept = default;
    };

    /** @brief Complete host-owned metadata for one serializable project component type. */
    struct ComponentDescriptor {
        ComponentTypeId typeId;
        std::uint32_t schemaVersion{1};
        std::string displayName;
        std::string category;
        std::vector<ComponentPropertyDescriptor> properties;
        std::vector<ComponentMigrationDescriptor> migrations;
    };

    /** @brief Encoding of the opaque persistent payload retained when project code is unavailable. */
    enum class ComponentPayloadEncoding : std::uint8_t {
        CanonicalJson,
    };

    /** @brief Persistent component envelope independent from native C++ layout and module lifetime. */
    struct SerializedComponent {
        ComponentTypeId typeId;
        std::uint32_t schemaVersion{1};
        ComponentPayloadEncoding encoding{ComponentPayloadEncoding::CanonicalJson};
        std::vector<std::byte> payload;
        [[nodiscard]] bool operator==(const SerializedComponent &) const noexcept = default;
    };

    /**
     * @brief Validates a persistent component envelope without requiring project code.
     * @param component Candidate opaque payload.
     * @return Success or a stable bounded-data error.
     */
    [[nodiscard]] Result<void> ValidateSerializedComponent(const SerializedComponent &component);
}  // namespace Horo::Gameplay
