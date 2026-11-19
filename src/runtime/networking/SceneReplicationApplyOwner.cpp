#include "Horo/Network/SceneReplicationApplyOwner.h"

#include "SceneReplicationTransformFields.h"

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
            SceneCandidate(std::shared_ptr<Runtime::RuntimeScene> scene, std::weak_ptr<Runtime::RuntimeSceneService> service,
                           Runtime::RuntimeDispatchEvidence evidence, Runtime::RuntimeSceneView view,
                           std::vector<TransformReplacement> replacements)
                : scene_(std::move(scene)), service_(std::move(service)), evidence_(std::move(evidence)), view_(view),
                  replacements_(std::move(replacements)) {}

            /** @copydoc IReplicationApplyCandidate::Commit */
            Result<void> Commit(const CancellationToken &cancellation) override {
                if (thread_ != std::this_thread::get_id() || committed_)
                    return Result<void>::Failure(MakeError(ReplicationStateErrors::Stale));
                const auto service = service_.lock();
                if ((!scene_ && !service) || !view_.IsCurrent())
                    return Result<void>::Failure(MakeError(ReplicationStateErrors::Stale));
                Runtime::SceneCommandBuffer commands;
                for (const auto &replacement : replacements_) {
                    if (cancellation.IsCancellationRequested() || replacement.receipt.IsCancellationRequested() ||
                        replacement.world.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(ReplicationStateErrors::Stale));
                    commands.SetLocalTransform(replacement.entity, replacement.transform,
                                               {view_.RuntimeId(), {}, cancellation, replacement.receipt, replacement.world});
                }
                auto committed = service ? service->CommitReplicaTransformBatch(view_, commands, evidence_) : CommitDirect(commands);
                committed_ = committed.HasValue();
                return committed;
            }

        private:
            Result<void> CommitDirect(const Runtime::SceneCommandBuffer &commands) const {
                const auto result = scene_->Commit(commands);
                return result.HasValue() ? Result<void>::Success() : Result<void>::Failure(result.ErrorValue());
            }

            std::shared_ptr<Runtime::RuntimeScene> scene_;
            std::weak_ptr<Runtime::RuntimeSceneService> service_;
            Runtime::RuntimeDispatchEvidence evidence_;
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
            Detail::TransformScalar(transform, property) = static_cast<float>(*number);
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

        /** @brief Owns exactly one field per transform property for both Scene ownership modes. */
        Result<std::vector<SceneReplicationFieldBinding>> ValidateBindings(ReplicationSchemaId schema, ReplicationSchemaVersion version,
                                                                           std::span<const SceneReplicationFieldBinding> fields) {
            using Bindings = Result<std::vector<SceneReplicationFieldBinding>>;
            if (!schema.IsValid() || !version.IsValid() || fields.empty() || fields.size() > Detail::SceneTransformPropertyCount)
                return Bindings::Failure(MakeError(ReplicationStateErrors::Invalid));
            std::vector<SceneReplicationFieldBinding> bindings{fields.begin(), fields.end()};
            std::ranges::sort(bindings, {}, &SceneReplicationFieldBinding::field);
            std::uint8_t properties{};
            for (std::size_t index{}; index < bindings.size(); ++index) {
                const auto &field = bindings[index];
                if (!field.field.IsValid() || field.property > SceneReplicationProperty::RotationW ||
                    (index > 0 && bindings[index - 1].field == field.field))
                    return Bindings::Failure(MakeError(ReplicationStateErrors::Invalid));
                const auto bit = Detail::TransformPropertyBit(field.property);
                if ((properties & bit) != 0)
                    return Bindings::Failure(MakeError(ReplicationStateErrors::Invalid));
                properties |= bit;
            }
            if (!Detail::HasCompleteRotation(properties))
                return Bindings::Failure(MakeError(ReplicationStateErrors::Invalid));
            return Bindings::Success(std::move(bindings));
        }

        /** @brief Applies projected fields and validates any complete rotation before staging a Scene command. */
        Result<void> ProjectFields(Math::Transform &transform, const ReplicationDecodedState &state,
                                   std::span<const SceneReplicationFieldBinding> bindings) {
            std::uint8_t properties{};
            for (const auto &field : state.Fields()) {
                const auto binding = std::ranges::find(bindings, field.field, &SceneReplicationFieldBinding::field);
                if (binding == bindings.end())
                    return Result<void>::Failure(MakeError(ReplicationStateErrors::Invalid));
                if (const auto set = SetField(transform, binding->property, field.value); set.HasError())
                    return set;
                properties |= Detail::TransformPropertyBit(binding->property);
            }
            if (!Detail::HasCompleteRotation(properties))
                return Result<void>::Failure(MakeError(ReplicationStateErrors::Invalid));
            if ((properties & Detail::SceneRotationProperties) != 0) {
                const auto rotation = transform.rotation.TryNormalized();
                if (rotation.HasError())
                    return Result<void>::Failure(MakeError(ReplicationStateErrors::Invalid));
                transform.rotation = rotation.Value();
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc SceneReplicationApplyOwner::SceneReplicationApplyOwner */
    SceneReplicationApplyOwner::SceneReplicationApplyOwner(ConstructionKey, std::shared_ptr<Runtime::RuntimeScene> scene,
                                                           const ReplicationSchemaId schema, const ReplicationSchemaVersion version,
                                                           std::vector<SceneReplicationFieldBinding> fields)
        : scene_(std::move(scene)), schema_(schema), version_(version), fields_(std::move(fields)) {}

    /** @copydoc SceneReplicationApplyOwner::SceneReplicationApplyOwner */
    SceneReplicationApplyOwner::SceneReplicationApplyOwner(ConstructionKey, std::weak_ptr<Runtime::RuntimeSceneService> service,
                                                           Runtime::RuntimeSceneView view, Runtime::RuntimeDispatchEvidence evidence,
                                                           const ReplicationSchemaId schema, const ReplicationSchemaVersion version,
                                                           std::vector<SceneReplicationFieldBinding> fields)
        : service_(std::move(service)), serviceView_(std::move(view)), evidence_(std::move(evidence)), schema_(schema), version_(version),
          fields_(std::move(fields)) {}

    /** @copydoc SceneReplicationApplyOwner::Create */
    Result<std::shared_ptr<SceneReplicationApplyOwner>> SceneReplicationApplyOwner::Create(
        std::shared_ptr<Runtime::RuntimeScene> scene, const ReplicationSchemaId schema, const ReplicationSchemaVersion version,
        const std::span<const SceneReplicationFieldBinding> fields) {
        using Owner = Result<std::shared_ptr<SceneReplicationApplyOwner>>;
        if (!scene)
            return Owner::Failure(MakeError(ReplicationStateErrors::Invalid));
        try {
            auto bindings = ValidateBindings(schema, version, fields);
            if (bindings.HasError())
                return Owner::Failure(std::move(bindings).ErrorValue());
            return Owner::Success(std::make_shared<SceneReplicationApplyOwner>(ConstructionKey{}, std::move(scene), schema, version,
                                                                               std::move(bindings).Value()));
        } catch (const std::bad_alloc &) {
            return Owner::Failure(MakeError(ReplicationStateErrors::Capacity));
        }
    }

    /** @copydoc SceneReplicationApplyOwner::CreateForService */
    Result<std::shared_ptr<SceneReplicationApplyOwner>> SceneReplicationApplyOwner::CreateForService(
        std::weak_ptr<Runtime::RuntimeSceneService> service, Runtime::RuntimeSceneView view, Runtime::RuntimeDispatchEvidence evidence,
        const ReplicationSchemaId schema, const ReplicationSchemaVersion version,
        const std::span<const SceneReplicationFieldBinding> fields) {
        using Owner = Result<std::shared_ptr<SceneReplicationApplyOwner>>;
        const auto retained = service.lock();
        if (!retained || !view.IsCurrent())
            return Owner::Failure(MakeError(ReplicationStateErrors::Stale));
        try {
            auto bindings = ValidateBindings(schema, version, fields);
            if (bindings.HasError())
                return Owner::Failure(std::move(bindings).ErrorValue());
            return Owner::Success(std::make_shared<SceneReplicationApplyOwner>(ConstructionKey{}, std::move(service), std::move(view),
                                                                               std::move(evidence), schema, version,
                                                                               std::move(bindings).Value()));
        } catch (const std::bad_alloc &) {
            return Owner::Failure(MakeError(ReplicationStateErrors::Capacity));
        }
    }

    /** @copydoc SceneReplicationApplyOwner::Prepare */
    Result<std::unique_ptr<IReplicationApplyCandidate>> SceneReplicationApplyOwner::Prepare(
        const std::span<const ReplicationApplyUpdate> updates) {
        if (thread_ != std::this_thread::get_id() || updates.empty() || updates.size() > 4096)
            return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(MakeError(ReplicationStateErrors::Invalid));
        const auto service = service_.lock();
        if (!scene_ && !service)
            return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(MakeError(ReplicationStateErrors::Stale));
        const auto view = scene_ ? scene_->View() : serviceView_;
        if (!view.IsCurrent())
            return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(MakeError(ReplicationStateErrors::Stale));
        std::vector<TransformReplacement> replacements;
        replacements.reserve(updates.size());
        for (const auto &update : updates) {
            if (std::ranges::find(replacements, update.mapping.entity, &TransformReplacement::entity) != replacements.end())
                return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(MakeError(ReplicationStateErrors::Invalid));
            auto current = ResolveLocalTransform(view, update, schema_, version_);
            if (current.HasError())
                return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(current.ErrorValue());
            auto transform = std::move(current).Value();
            if (const auto projected = ProjectFields(transform, update.state, fields_); projected.HasError())
                return Result<std::unique_ptr<IReplicationApplyCandidate>>::Failure(projected.ErrorValue());
            replacements.emplace_back(update.mapping.entity, transform, update.cancellation, update.worldCancellation);
        }
        return Result<std::unique_ptr<IReplicationApplyCandidate>>::Success(
            std::make_unique<SceneCandidate>(scene_, service_, evidence_, view, std::move(replacements)));
    }
}  // namespace Horo::Network
