#include "Horo/Network/SceneReplicationApplyOwner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Network {
    namespace {
        /** @brief Detached complete replacements and cancellation evidence owned by one Scene transaction. */
        struct TransformReplacement final {
            Runtime::EntityRef entity;
            Math::Transform transform;
            CancellationToken receipt;
            CancellationToken world;
        };

        /** @brief Owns a detached transform batch; RuntimeScene publishes only after complete command validation. */
        class SceneCandidate final : public IReplicationApplyCandidate {
        public:
            SceneCandidate(std::shared_ptr<Runtime::RuntimeScene> scene, Runtime::RuntimeSceneView view,
                           std::vector<TransformReplacement> replacements)
                : scene_(std::move(scene)), view_(view), replacements_(std::move(replacements)) {}

            /** @copydoc IReplicationApplyCandidate::Commit */
            Result<void> Commit(const CancellationToken &cancellation) override {
                if (thread_ != std::this_thread::get_id() || committed_ || !view_.IsCurrent())
                    return Result<void>::Failure(MakeError(ReplicationStateErrors::Stale));
                Runtime::SceneCommandBuffer commands;
                for (const auto &replacement : replacements_) {
                    if (cancellation.IsCancellationRequested() || replacement.receipt.IsCancellationRequested() ||
                        replacement.world.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(ReplicationStateErrors::Stale));
                    commands.SetLocalTransform(replacement.entity, replacement.transform,
                                               {view_.RuntimeId(), {}, cancellation, replacement.receipt, replacement.world});
                }
                auto result = scene_->Commit(commands);
                if (result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
                committed_ = true;
                return Result<void>::Success();
            }

        private:
            std::shared_ptr<Runtime::RuntimeScene> scene_;
            Runtime::RuntimeSceneView view_;
            std::vector<TransformReplacement> replacements_;
            std::thread::id thread_{std::this_thread::get_id()};
            bool committed_{};
        };

        /** @brief Applies one finite representable network scalar to a declared typed property. */
        Result<void> SetField(Math::Transform &transform, const SceneReplicationProperty property, const ReplicationRuntimeValue &value) {
            const auto *number = std::get_if<double>(&value);
            if (!number || !std::isfinite(*number) || std::abs(*number) > std::numeric_limits<float>::max())
                return Result<void>::Failure(MakeError(ReplicationStateErrors::Invalid));
            const auto scalar = static_cast<float>(*number);
            using enum SceneReplicationProperty;
            switch (property) {
                case TranslationX:
                    transform.translation.x = scalar;
                    break;
                case TranslationY:
                    transform.translation.y = scalar;
                    break;
                case TranslationZ:
                    transform.translation.z = scalar;
                    break;
            }
            return Result<void>::Success();
        }

        /** @brief Revalidates decoded provenance and copies the exact receiving Scene transform before semantic edits. */
        Result<Math::Transform> ResolveLocalTransform(const Runtime::RuntimeSceneView &view, const ReplicationApplyUpdate &update,
                                                      const ReplicationSchemaId schema, const ReplicationSchemaVersion version) {
            if (!update.state.IsCurrent() || update.mapping.provenance.schema != schema ||
                update.mapping.provenance.schemaVersion != version || update.mapping.entity.runtime != view.RuntimeId() ||
                update.role.object != update.mapping.object || update.state.Object() != update.mapping.object ||
                (update.role.role != ReplicationExecutionRole::SimulatedClient &&
                 update.role.role != ReplicationExecutionRole::AutonomousClient))
                return Result<Math::Transform>::Failure(MakeError(ReplicationStateErrors::Stale));
            auto entity = view.Get(update.mapping.entity);
            if (entity.HasError())
                return Result<Math::Transform>::Failure(entity.ErrorValue());
            return Result<Math::Transform>::Success(*entity.Value().localTransform);
        }
    }  // namespace

    /** @copydoc SceneReplicationApplyOwner::SceneReplicationApplyOwner */
    SceneReplicationApplyOwner::SceneReplicationApplyOwner(ConstructionKey, std::shared_ptr<Runtime::RuntimeScene> scene,
                                                           const ReplicationSchemaId schema, const ReplicationSchemaVersion version,
                                                           std::vector<SceneReplicationFieldBinding> fields)
        : scene_(std::move(scene)), schema_(schema), version_(version), fields_(std::move(fields)) {}

    /** @copydoc SceneReplicationApplyOwner::Create */
    Result<std::shared_ptr<SceneReplicationApplyOwner>> SceneReplicationApplyOwner::Create(
        std::shared_ptr<Runtime::RuntimeScene> scene, const ReplicationSchemaId schema, const ReplicationSchemaVersion version,
        const std::span<const SceneReplicationFieldBinding> fields) {
        if (!scene || !schema.IsValid() || !version.IsValid() || fields.empty() || fields.size() > 3)
            return Result<std::shared_ptr<SceneReplicationApplyOwner>>::Failure(MakeError(ReplicationStateErrors::Invalid));
        try {
            std::vector<SceneReplicationFieldBinding> bindings{fields.begin(), fields.end()};
            std::ranges::sort(bindings, {}, &SceneReplicationFieldBinding::field);
            std::uint8_t properties{};
            for (std::size_t index{}; index < bindings.size(); ++index) {
                const auto &field = bindings[index];
                if (!field.field.IsValid() || field.property > SceneReplicationProperty::TranslationZ ||
                    (index > 0 && bindings[index - 1].field == field.field))
                    return Result<std::shared_ptr<SceneReplicationApplyOwner>>::Failure(MakeError(ReplicationStateErrors::Invalid));
                const auto bit = static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(field.property));
                if ((properties & bit) != 0)
                    return Result<std::shared_ptr<SceneReplicationApplyOwner>>::Failure(MakeError(ReplicationStateErrors::Invalid));
                properties |= bit;
            }
            return Result<std::shared_ptr<SceneReplicationApplyOwner>>::Success(
                std::make_shared<SceneReplicationApplyOwner>(ConstructionKey{}, std::move(scene), schema, version, std::move(bindings)));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<SceneReplicationApplyOwner>>::Failure(MakeError(ReplicationStateErrors::Capacity));
        }
    }

    /** @copydoc SceneReplicationApplyOwner::Prepare */
    Result<std::unique_ptr<IReplicationApplyCandidate>> SceneReplicationApplyOwner::Prepare(
        const std::span<const ReplicationApplyUpdate> updates) {
        if (thread_ != std::this_thread::get_id() || updates.empty() || updates.size() > 4096)
            return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(MakeError(ReplicationStateErrors::Invalid));
        const auto view = scene_->View();
        std::vector<TransformReplacement> replacements;
        replacements.reserve(updates.size());
        for (const auto &update : updates) {
            if (std::ranges::find(replacements, update.mapping.entity, &TransformReplacement::entity) != replacements.end())
                return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(MakeError(ReplicationStateErrors::Invalid));
            auto current = ResolveLocalTransform(view, update, schema_, version_);
            if (current.HasError())
                return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(current.ErrorValue());
            auto transform = std::move(current).Value();
            for (const auto &field : update.state.Fields()) {
                const auto binding = std::ranges::find(fields_, field.field, &SceneReplicationFieldBinding::field);
                if (binding == fields_.end())
                    return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(MakeError(ReplicationStateErrors::Invalid));
                if (const auto set = SetField(transform, binding->property, field.value); set.HasError())
                    return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(set.ErrorValue());
            }
            replacements.push_back({update.mapping.entity, transform, update.cancellation, update.worldCancellation});
        }
        return Result<std::unique_ptr<IReplicationApplyCandidate>>::Success(
            std::make_unique<SceneCandidate>(scene_, view, std::move(replacements)));
    }
}  // namespace Horo::Network
