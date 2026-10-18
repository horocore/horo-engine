#pragma once

/** @file PhysicsInspectorSession.h
 * @brief Bounded Inspector-owned draft and text buffers; document/history remain host-owned.
 */
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <array>

namespace Horo::Editor {
    /** @brief Lossless decimal/UUID text retained across invalid edits; parsed only on changes. */
    struct PhysicsInspectorText final {
        std::array<char, 37> bytes{};
        bool valid{true};
    };

    /** @brief One explicit material mapping with no invented project identity. */
    struct PhysicsInspectorMaterialDraft final {
        Runtime::PhysicsColliderMaterialBinding value;
        PhysicsInspectorText slot;
        PhysicsInspectorText asset;
    };

    /** @brief Cached typed collider fields and their lossless identity text. */
    struct PhysicsInspectorColliderDraft final {
        Runtime::ColliderComponent value;
        PhysicsInspectorText bodyObject;
        PhysicsInspectorText bodySlot;
        PhysicsInspectorText profile;
        PhysicsInspectorText shapeAsset;
        PhysicsInspectorText shapePart;
        std::vector<PhysicsInspectorMaterialDraft> materials;
    };

    /** @brief Cached typed endpoint identity text, independent of object names/hierarchy. */
    struct PhysicsInspectorConstraintDraft final {
        Runtime::PhysicsConstraintComponent value;
        PhysicsInspectorText firstObject;
        PhysicsInspectorText firstBody;
        PhysicsInspectorText secondObject;
        PhysicsInspectorText secondBody;
    };

    /** @brief Detached editable values and text; exact owner identity remains private to the session. */
    struct PhysicsInspectorDraft final {
        std::optional<Runtime::RigidBodyComponent> body;
        std::vector<PhysicsInspectorColliderDraft> colliders;
        std::vector<PhysicsInspectorConstraintDraft> constraints;
        bool dirty{};
    };

    /** @brief One transient exact-owner draft, invalidated by actual selection/document replacement or detach. */
    class PhysicsInspectorSession final {
    public:
        /** @brief Copy only on a new exact source revision or selection. @return Whether the draft was replaced. */
        bool Begin(const SceneObject &object, const SceneTransactionContext &context, std::uint64_t selectionRevision);
        /** @brief Discard staged values and release all owned text on detach/selection loss. */
        void Clear() noexcept;
        /** @brief Restore the captured authored baseline, retaining no previous invalid text. */
        void Cancel();
        /** @brief Assemble a detached request after text validation; publishes no state. */
        [[nodiscard]] Result<PhysicsInspectorRequest> Request() const;
        /** @brief Stage a complete body/box producer set with explicit unresolved project references. */
        void AddBody();
        /** @brief Stage a collider with explicit body reference; project bindings must be supplied before Apply. */
        void AddCollider();
        /** @brief Stage a fixed constraint whose first endpoint is an explicit selected body when present. */
        void AddConstraint();
        /** @brief Capture all text for a typed collider once, outside its steady draw path. */
        static PhysicsInspectorColliderDraft CaptureCollider(const Runtime::ColliderComponent &value);
        /** @brief Capture all text for a typed constraint once. */
        static PhysicsInspectorConstraintDraft CaptureConstraint(const Runtime::PhysicsConstraintComponent &value);
        /** @brief Decimal text representation without floating-point conversion. */
        static PhysicsInspectorText Decimal(std::uint64_t value);
        /** @brief Bounded canonical UUID text representation; empty remains an invalid explicit input. */
        static PhysicsInspectorText Identity(std::string_view value);

        /** @brief Access editable draft data; no document/history mutation occurs. @return The session-owned mutable draft. */
        [[nodiscard]] PhysicsInspectorDraft &Draft() noexcept {
            return draft_;
        }

        /** @brief Inspect the current uncommitted draft. @return The session-owned immutable draft. */
        [[nodiscard]] const PhysicsInspectorDraft &Draft() const noexcept {
            return draft_;
        }

    private:
        /** @brief Allocate a never-colliding object-local component identity within the complete staged set. */
        [[nodiscard]] std::uint64_t NextComponentId() const noexcept;
        PhysicsInspectorDraft draft_;
        SceneSource::SceneObjectId object_;
        SceneTransactionContext context_;
        std::uint64_t selectionRevision_{};
        PhysicsInspectorValues baseline_;
    };
}  // namespace Horo::Editor
