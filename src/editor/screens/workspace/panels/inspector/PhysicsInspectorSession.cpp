#include "editor/screens/workspace/panels/inspector/PhysicsInspectorSession.h"

#include <algorithm>
#include <charconv>
#include <limits>

namespace Horo::Editor {
    /** @copydoc PhysicsInspectorSession::Decimal */
    PhysicsInspectorText PhysicsInspectorSession::Decimal(const std::uint64_t value) {
        PhysicsInspectorText text;
        const auto result = std::to_chars(text.bytes.data(), text.bytes.data() + text.bytes.size() - 1, value);
        text.valid = result.ec == std::errc{} && value != 0;
        return text;
    }

    /** @copydoc PhysicsInspectorSession::Identity */
    PhysicsInspectorText PhysicsInspectorSession::Identity(const std::string_view value) {
        PhysicsInspectorText text;
        text.valid = value.size() == 36;
        if (value.size() < text.bytes.size())
            std::ranges::copy(value, text.bytes.begin());
        return text;
    }

    /** @copydoc PhysicsInspectorSession::CaptureCollider */
    PhysicsInspectorColliderDraft PhysicsInspectorSession::CaptureCollider(const Runtime::ColliderComponent &value) {
        PhysicsInspectorColliderDraft draft;
        draft.value = value;
        draft.bodyObject = Decimal(value.body.object.value);
        draft.bodySlot = Decimal(value.body.body.value);
        draft.profile = Identity(value.collisionProfile.IsValid() ? value.collisionProfile.ToString() : "");
        if (const auto *asset = std::get_if<Runtime::PhysicsShapeAssetReference>(&value.source)) {
            draft.shapeAsset = Identity(asset->asset.IsValid() ? asset->asset.ToString() : "");
            draft.shapePart = Decimal(asset->subresource.value);
        }
        for (const auto &material : value.materials)
            draft.materials.emplace_back(material, Decimal(material.slot.Value()),
                                         Identity(material.material.IsValid() ? material.material.ToString() : ""));
        return draft;
    }

    /** @copydoc PhysicsInspectorSession::CaptureConstraint */
    PhysicsInspectorConstraintDraft PhysicsInspectorSession::CaptureConstraint(const Runtime::PhysicsConstraintComponent &value) {
        PhysicsInspectorConstraintDraft draft;
        draft.value = value;
        draft.firstObject = Decimal(value.first.body.object.value);
        draft.firstBody = Decimal(value.first.body.body.value);
        if (const auto *second = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&value.second)) {
            draft.secondObject = Decimal(second->body.object.value);
            draft.secondBody = Decimal(second->body.body.value);
        }
        return draft;
    }

    /** @copydoc PhysicsInspectorSession::Begin */
    bool PhysicsInspectorSession::Begin(const SceneObject &object, const SceneTransactionContext &context,
                                        const std::uint64_t selectionRevision) {
        if (object_ == object.id && context_.session == context.session && context_.revision == context.revision &&
            context_.state == context.state && context_.canEdit == context.canEdit && selectionRevision_ == selectionRevision)
            return false;
        object_ = object.id;
        context_ = context;
        selectionRevision_ = selectionRevision;
        baseline_ = {object.components.rigidBody, object.components.colliders, object.components.physicsConstraints};
        Cancel();
        return true;
    }

    /** @copydoc PhysicsInspectorSession::Clear */
    void PhysicsInspectorSession::Clear() noexcept {
        draft_.body.reset();
        draft_.colliders.clear();
        draft_.constraints.clear();
        baseline_ = {};
        context_ = {};
        object_ = {};
        draft_.dirty = false;
    }

    /** @copydoc PhysicsInspectorSession::Cancel */
    void PhysicsInspectorSession::Cancel() {
        draft_.body = baseline_.body;
        draft_.colliders.clear();
        draft_.constraints.clear();
        for (const auto &value : baseline_.colliders)
            draft_.colliders.push_back(CaptureCollider(value));
        for (const auto &value : baseline_.constraints)
            draft_.constraints.push_back(CaptureConstraint(value));
        draft_.dirty = false;
    }

    /** @copydoc PhysicsInspectorSession::Request */
    Result<PhysicsInspectorRequest> PhysicsInspectorSession::Request() const {
        using Return = Result<PhysicsInspectorRequest>;
        PhysicsInspectorRequest request{context_, selectionRevision_, {draft_.body, {}, {}}};
        for (const auto &draft : draft_.colliders) {
            if (const bool assetShape = std::holds_alternative<Runtime::PhysicsShapeAssetReference>(draft.value.source);
                !draft.bodyObject.valid || !draft.bodySlot.valid || !draft.profile.valid ||
                (assetShape && (!draft.shapeAsset.valid || !draft.shapePart.valid)))
                return Return::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Invalid)));
            auto value = draft.value;
            value.materials.clear();
            for (const auto &material : draft.materials) {
                if (!material.slot.valid || !material.asset.valid)
                    return Return::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Invalid)));
                value.materials.push_back(material.value);
            }
            request.values.colliders.push_back(std::move(value));
        }
        for (const auto &draft : draft_.constraints) {
            if (const bool secondBody = std::holds_alternative<Runtime::PhysicsConstraintBodyEndpoint>(draft.value.second);
                !draft.firstObject.valid || !draft.firstBody.valid ||
                (secondBody && (!draft.secondObject.valid || !draft.secondBody.valid)))
                return Return::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Invalid)));
            request.values.constraints.push_back(draft.value);
        }
        return Return::Success(std::move(request));
    }

    /** @copydoc PhysicsInspectorSession::NextComponentId */
    std::uint64_t PhysicsInspectorSession::NextComponentId() const noexcept {
        std::uint64_t highest = draft_.body ? draft_.body->id.value : 0;
        for (const auto &draft : draft_.colliders)
            highest = std::max(highest, draft.value.id.value);
        for (const auto &draft : draft_.constraints)
            highest = std::max(highest, draft.value.id.value);
        return highest == std::numeric_limits<std::uint64_t>::max() ? 0 : highest + 1;
    }

    /** @copydoc PhysicsInspectorSession::AddBody */
    void PhysicsInspectorSession::AddBody() {
        const auto next = NextComponentId();
        if (const bool exhaustedSlot = std::ranges::any_of(draft_.colliders,
                                                           [](const auto &draft) {
            return draft.value.collider.value == std::numeric_limits<std::uint64_t>::max();
        });
            draft_.body || next == 0 || next == std::numeric_limits<std::uint64_t>::max() || exhaustedSlot ||
            draft_.colliders.size() >= Runtime::MaximumPhysicsCollidersPerBody)
            return;
        Runtime::RigidBodyComponent value;
        value.id.value = NextComponentId();
        value.body.value = 1;
        draft_.body = value;
        AddCollider();
        draft_.dirty = true;
    }

    /** @copydoc PhysicsInspectorSession::AddCollider */
    void PhysicsInspectorSession::AddCollider(const bool trigger) {
        if (draft_.colliders.size() >= Runtime::MaximumPhysicsCollidersPerBody || NextComponentId() == 0)
            return;
        std::uint64_t slot = 0;
        for (const auto &draft : draft_.colliders)
            slot = std::max(slot, draft.value.collider.value);
        if (slot == std::numeric_limits<std::uint64_t>::max())
            return;
        if (!draft_.body) {
            if (NextComponentId() == std::numeric_limits<std::uint64_t>::max())
                return;
            Runtime::RigidBodyComponent body;
            body.id.value = NextComponentId();
            body.body.value = 1;
            draft_.body = body;
        }
        Runtime::ColliderComponent value;
        value.sensor = trigger;
        value.id.value = NextComponentId();
        value.collider.value = slot + 1;
        value.body = {Runtime::SceneObjectId{object_.value}, draft_.body ? draft_.body->body : Runtime::PhysicsBodySlotId{}};
        value.materials.push_back({Physics::PhysicsMaterialSlotId::FromValue(1), {}});
        draft_.colliders.push_back(CaptureCollider(value));
        draft_.dirty = true;
    }

    /** @copydoc PhysicsInspectorSession::AddConstraint */
    void PhysicsInspectorSession::AddConstraint() {
        if (draft_.constraints.size() >= Runtime::MaximumPhysicsConstraintsPerObject || NextComponentId() == 0)
            return;
        std::uint64_t slot = 0;
        for (const auto &draft : draft_.constraints)
            slot = std::max(slot, draft.value.constraint.value);
        if (slot == std::numeric_limits<std::uint64_t>::max())
            return;
        Runtime::PhysicsConstraintComponent value;
        value.id.value = NextComponentId();
        value.constraint.value = slot + 1;
        value.first.body = {Runtime::SceneObjectId{object_.value}, draft_.body ? draft_.body->body : Runtime::PhysicsBodySlotId{}};
        draft_.constraints.push_back(CaptureConstraint(value));
        draft_.dirty = true;
    }
}  // namespace Horo::Editor
