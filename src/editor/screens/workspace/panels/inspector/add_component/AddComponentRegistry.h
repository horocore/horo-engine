#pragma once
/** @file AddComponentRegistry.h
 * @brief Inert menu contributions over canonical scene components and project behavior identities.
 */
#include "Horo/Foundation/Result.h"
#include "Horo/Gameplay/BehaviorTypes.h"
#include "editor/screens/workspace/panels/inspector/PhysicsInspectorSession.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Horo::Editor {
    /** @brief Stable grouping independent of translated labels. */
    enum class AddComponentCategory {
        Rendering,
        Audio,
        Gameplay,
        Scripts,
        Physics,
        Count
    };
    /** @brief Existing detached Physics creation operations. */
    enum class AddPhysicsComponent {
        Body,
        BoxCollider,
        TriggerCollider,
        Joint
    };
    /** @brief Source creation workflows; no document is attached implicitly. */
    enum class AddComponentWorkflow {
        CreateScript
    };
    using BuiltinAddComponentAction = std::variant<ComponentType, AddPhysicsComponent, AddComponentWorkflow>;
    using AddComponentAction = std::variant<ComponentType, AddPhysicsComponent, AddComponentWorkflow, Gameplay::BehaviorTypeId>;

    /** @brief Read-only owner state used to evaluate menu availability without mutations or native activation. */
    struct AddComponentContext {
        const SceneObject &object;
        const PhysicsInspectorDraft &physics;
        bool physicsEditable{};
        bool hasProject{};
        bool canIssueCommand{true};
    };

    using AddComponentAvailability = bool (*)(const AddComponentContext &) noexcept;

    /** @brief Static contribution metadata; copying/registering it never invokes its predicate or action. */
    struct BuiltinAddComponentDefinition {
        std::string_view id;
        std::string_view labelKey;
        AddComponentCategory category;
        BuiltinAddComponentAction action;
        AddComponentAvailability available{}; /**< Borrowed static code; must outlive the registry. */
        bool separatorBefore{};
    };

    /** @brief Stable category identity and external localization key. */
    struct AddComponentCategoryDefinition {
        AddComponentCategory id;
        std::string_view labelKey;
    };

    /** @brief Owned cached menu metadata; behavior schemas remain owned by ProjectGameplayRegistry. */
    struct AddComponentEntry {
        std::string id;
        std::string label;
        AddComponentAction action;
        AddComponentAvailability available{};
        bool localized{true};
        bool allowMultiple{};
        bool separatorBefore{};
    };

    /** @brief Explicit panel-owned presentation catalog; no globals, service discovery, filesystem access or activation. */
    class AddComponentRegistry final {
    public:
        /** @brief Copies the validated static built-in definitions into this catalog. */
        AddComponentRegistry();
        /** @brief Adds inert static metadata before drawing. @param definition Owned strings are copied; callback code is borrowed.
         * @return Success or invalid/duplicate metadata, with the existing catalog preserved on failure. */
        [[nodiscard]] Result<void> RegisterStatic(const BuiltinAddComponentDefinition &definition);
        /** @brief Replaces project script contributions only when scope or owner revision changes.
         * @param projectScope Exact workspace project identity. @param revision Behavior publication revision.
         * @param behaviors Authoritative registered descriptors; names/multiplicity are copied, not rediscovered.
         * @return Success or malformed/duplicate descriptors. Callers must not draw a failed projection. */
        [[nodiscard]] Result<void> SynchronizeBehaviors(std::string_view projectScope, std::uint64_t revision,
                                                        std::span<const Gameplay::BehaviorDescriptor> behaviors);
        /** @brief Releases project contributions on detach while preserving static definitions. */
        void ClearProjectBehaviors();
        /** @brief Borrows ordered category metadata. @return Built-in grouping order. */
        [[nodiscard]] static std::span<const AddComponentCategoryDefinition> Categories() noexcept;
        /** @brief Borrows one category until registry mutation. @param category Valid category. @return Current entries. */
        [[nodiscard]] std::span<const AddComponentEntry> Entries(AddComponentCategory category) const noexcept;
        /** @brief Finds exact contribution identity. @param id Stable contribution ID. @return Borrowed entry or null. */
        [[nodiscard]] const AddComponentEntry *Find(std::string_view id) const noexcept;
        /** @brief Evaluates the current selection/draft policy without side effects.
         * @param entry Registered entry. @param context Current owner state. @return Whether its action can be selected. */
        [[nodiscard]] bool IsAvailable(const AddComponentEntry &entry, const AddComponentContext &context) const noexcept;

    private:
        /** @brief Copies already validated inert metadata without evaluating callbacks. */
        void AppendStatic(const BuiltinAddComponentDefinition &definition);
        std::array<std::vector<AddComponentEntry>, static_cast<std::size_t>(AddComponentCategory::Count)> entries_;
        std::string projectScope_;
        std::optional<std::uint64_t> behaviorRevision_;
    };

    /** @brief Borrows compile-time built-in definitions; accessing them performs no registration or activation. */
    [[nodiscard]] std::span<const BuiltinAddComponentDefinition> BuiltinAddComponentDefinitions() noexcept;
}  // namespace Horo::Editor
