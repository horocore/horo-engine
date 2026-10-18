#include "Horo/Editor/PhysicsInspector.h"

#include "editor/document/SceneDocumentInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Preserve the transaction owner's stable rejection vocabulary. */
        Error Rejected(const SceneTransactionFailure reason) {
            return MakeError(SceneTransactionError(reason));
        }

        /** @brief Reject reassigned generations and advance only an actual existing payload change. */
        template <typename Component> Result<void> Advance(Component &after, const Component *before) {
            using enum SceneTransactionFailure;
            if (!before)
                return after.generation == 1 ? Result<void>::Success() : Result<void>::Failure(Rejected(Invalid));
            if (after.schemaVersion != before->schemaVersion || after.generation != before->generation)
                return Result<void>::Failure(Rejected(Stale));
            if (after == *before)
                return Result<void>::Success();
            if (after.generation == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(Rejected(Capacity));
            ++after.generation;
            return Result<void>::Success();
        }

        /** @brief Match stable object-local component identities without inferring references from hierarchy. */
        template <typename Component> Result<void> AdvanceList(std::vector<Component> &after, const std::vector<Component> &before) {
            for (auto &component : after) {
                const auto prior = std::ranges::find(before, component.id, &Component::id);
                if (auto result = Advance(component, prior == before.end() ? nullptr : std::to_address(prior)); result.HasError())
                    return result;
            }
            return Result<void>::Success();
        }

        /** @brief Bound nested edit storage before copying a foreign detached candidate. */
        bool Fits(const PhysicsInspectorValues &values, const std::size_t maximumBytes) {
            if (values.colliders.size() > Runtime::MaximumPhysicsConstraintsPerObject ||
                values.constraints.size() > Runtime::MaximumPhysicsConstraintsPerObject)
                return false;
            std::size_t bytes = sizeof(values) + values.colliders.size() * sizeof(Runtime::ColliderComponent) +
                                values.constraints.size() * sizeof(Runtime::PhysicsConstraintComponent);
            if (bytes > maximumBytes)
                return false;
            for (const auto &collider : values.colliders) {
                if (collider.materials.size() > Runtime::MaximumPhysicsColliderMaterialBindings)
                    return false;
                const auto nested = collider.materials.size() * sizeof(Runtime::PhysicsColliderMaterialBinding);
                if (nested > maximumBytes - bytes)
                    return false;
                bytes += nested;
            }
            return true;
        }

        /** @brief Validate all exact cross-object references against the proposed complete root. */
        Result<void> ValidateGraph(const SceneTransactionSnapshot &snapshot, const SceneSource::SceneObjectSnapshot &candidate) {
            std::vector<Runtime::PhysicsSceneComponentView> views;
            views.reserve(snapshot.objects.size());
            for (const auto &object : snapshot.objects) {
                const auto &current = object.id == candidate.id ? candidate : object;
                const auto &components = current.components;
                if (const bool canonical = components.rigidBody || !components.colliders.empty() || !components.physicsConstraints.empty();
                    canonical && components.triggerVolume && components.triggerVolume->enabled)
                    return Result<void>::Failure(Rejected(SceneTransactionFailure::Invalid));
                views.emplace_back(Runtime::SceneObjectId{current.id.value}, components.rigidBody ? &*components.rigidBody : nullptr,
                                   components.colliders, components.physicsConstraints);
            }
            return Runtime::ValidatePhysicsSceneComponentViews(views);
        }
    }  // namespace

    /** @copydoc PreparePhysicsInspectorEdit */
    Result<SceneObjectReplacement> PreparePhysicsInspectorEdit(const SceneTransactionSnapshot &snapshot,
                                                               const SceneSource::SceneObjectId object,
                                                               const PhysicsInspectorValues &values, const SceneTransactionLimits limits) {
        using Return = Result<SceneObjectReplacement>;
        if (limits.maximumChanges == 0 || snapshot.objects.size() > limits.maximumSnapshotObjects ||
            !Fits(values, limits.maximumPreparedBytes))
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        const auto baseline = std::ranges::find(snapshot.objects, object, &SceneSource::SceneObjectSnapshot::id);
        if (baseline == snapshot.objects.end() || baseline->generated)
            return Return::Failure(Rejected(SceneTransactionFailure::Unauthorized));
        const auto baselineBytes = SceneDocumentDetail::EstimateSceneObjectOwnedMemoryBytes(*baseline);
        if (const auto viewBytes = snapshot.objects.size() * sizeof(Runtime::PhysicsSceneComponentView);
            baselineBytes > limits.maximumPreparedBytes || viewBytes > limits.maximumPreparedBytes - baselineBytes ||
            !Fits(values, limits.maximumPreparedBytes - baselineBytes - viewBytes))
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        const auto fingerprint =
            FingerprintSceneObjectContent(*baseline, std::min(limits.maximumPreparedBytes, MaximumSceneObjectFingerprintBytes));
        if (fingerprint.HasError())
            return Return::Failure(fingerprint.ErrorValue());
        try {
            auto candidate = *baseline;
            auto &components = candidate.components;
            components.rigidBody = values.body;
            components.colliders = values.colliders;
            components.physicsConstraints = values.constraints;
            if (components.rigidBody) {
                const auto &prior = baseline->components.rigidBody;
                const auto *matching = prior && prior->id == components.rigidBody->id ? &*prior : nullptr;
                if (auto result = Advance(*components.rigidBody, matching); result.HasError())
                    return Return::Failure(result.ErrorValue());
            }
            if (auto result = AdvanceList(components.colliders, baseline->components.colliders); result.HasError())
                return Return::Failure(result.ErrorValue());
            if (auto result = AdvanceList(components.physicsConstraints, baseline->components.physicsConstraints); result.HasError())
                return Return::Failure(result.ErrorValue());
            if (auto result = ValidateGraph(snapshot, candidate); result.HasError())
                return Return::Failure(result.ErrorValue());
            return Return::Success(SceneObjectReplacement{object, fingerprint.Value(), {}, std::move(candidate), {}});
        } catch (const std::bad_alloc &) {
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        }
    }
}  // namespace Horo::Editor
