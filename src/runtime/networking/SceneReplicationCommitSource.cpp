#include "Horo/Network/SceneReplicationCommitSource.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Network {
    /** @copydoc SceneReplicationCommitSource::SceneReplicationCommitSource */
    SceneReplicationCommitSource::SceneReplicationCommitSource(ConstructionKey, std::shared_ptr<Runtime::RuntimeScene> scene,
                                                               std::vector<SceneReplicationFieldBinding> fields)
        : scene_(std::move(scene)), fields_(std::move(fields)) {}

    /** @copydoc SceneReplicationCommitSource::Create */
    Result<std::shared_ptr<SceneReplicationCommitSource>> SceneReplicationCommitSource::Create(
        std::shared_ptr<Runtime::RuntimeScene> scene, const std::span<const SceneReplicationFieldBinding> fields) {
        if (!scene || fields.empty() || fields.size() > 1024)
            return Result<std::shared_ptr<SceneReplicationCommitSource>>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
        try {
            std::vector<SceneReplicationFieldBinding> bindings{fields.begin(), fields.end()};
            std::ranges::sort(bindings, {}, &SceneReplicationFieldBinding::field);
            for (std::size_t index{}; index < bindings.size(); ++index)
                if (!bindings[index].field.IsValid() || bindings[index].property > SceneReplicationProperty::TranslationZ ||
                    (index > 0 && bindings[index - 1].field == bindings[index].field))
                    return Result<std::shared_ptr<SceneReplicationCommitSource>>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
            return Result<std::shared_ptr<SceneReplicationCommitSource>>::Success(
                std::make_shared<SceneReplicationCommitSource>(ConstructionKey{}, std::move(scene), std::move(bindings)));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<SceneReplicationCommitSource>>::Failure(MakeError(ReplicationCaptureErrors::Capacity));
        }
    }

    /** @copydoc SceneReplicationCommitSource::CommitSimulationTick */
    Result<Runtime::StructuralCommitResult> SceneReplicationCommitSource::CommitSimulationTick(const Runtime::SceneCommandBuffer &commands,
                                                                                               const std::uint64_t simulationTick) {
        if (owner_ != std::this_thread::get_id() || reading_.has_value() || simulationTick == 0 || simulationTick <= tick_ ||
            revision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<Runtime::StructuralCommitResult>::Failure(MakeError(ReplicationCaptureErrors::Uncommitted));
        auto committed = scene_->Commit(commands);
        if (committed.HasError())
            return committed;
        committed_ = scene_->View();
        tick_ = simulationTick;
        ++revision_;
        return committed;
    }

    /** @copydoc SceneReplicationCommitSource::CaptureAfterCommit */
    Result<ReplicationCaptureReport> SceneReplicationCommitSource::CaptureAfterCommit(const ReplicationWorldLifecycle &world,
                                                                                      ReplicationStateCapture &capture,
                                                                                      const Runtime::RuntimePhase phase,
                                                                                      const CancellationToken &cancellation) const {
        if (owner_ != std::this_thread::get_id() || tick_ == 0 || !committed_.IsCurrent())
            return Result<ReplicationCaptureReport>::Failure(MakeError(ReplicationCaptureErrors::Uncommitted));
        const auto active = world.ActiveDescriptor();
        if (active.HasError())
            return Result<ReplicationCaptureReport>::Failure(active.ErrorValue());
        const auto read = world.AcquireCaptureRead({scene_->View().RuntimeId(), active.Value().session, phase, tick_, cancellation});
        if (read.HasError())
            return Result<ReplicationCaptureReport>::Failure(read.ErrorValue());
        return capture.CaptureAtCommit(read.Value(), tick_, cancellation);
    }

    /** @copydoc SceneReplicationCommitSource::BeginRead */
    Result<ReplicationCommittedRead> SceneReplicationCommitSource::BeginRead(const NetworkObjectMappingEntry &object,
                                                                             const std::uint64_t tick) const {
        if (owner_ != std::this_thread::get_id() || reading_.has_value() || tick != tick_ || tick_ == 0 || !committed_.IsCurrent())
            return Result<ReplicationCommittedRead>::Failure(MakeError(ReplicationCaptureErrors::Uncommitted));
        if (const auto entity = committed_.Get(object.entity); entity.HasError())
            return Result<ReplicationCommittedRead>::Failure(entity.ErrorValue());
        reading_ = object.entity;
        return Result<ReplicationCommittedRead>::Success({tick_, revision_, revision_});
    }

    /** @copydoc SceneReplicationCommitSource::Capture */
    Result<void> SceneReplicationCommitSource::Capture(const ReplicationCommittedRead &read, ReplicationCaptureWriter &writer) const {
        if (!IsCurrent(read))
            return Result<void>::Failure(MakeError(ReplicationCaptureErrors::Stale));
        const auto entity = committed_.Get(*reading_);
        if (entity.HasError())
            return Result<void>::Failure(entity.ErrorValue());
        using enum SceneReplicationProperty;
        const Math::Vec3 translation = entity.Value().localTransform->translation;
        for (const auto &field : fields_) {
            double value{};
            switch (field.property) {
                case TranslationX:
                    value = translation.x;
                    break;
                case TranslationY:
                    value = translation.y;
                    break;
                case TranslationZ:
                    value = translation.z;
                    break;
            }
            if (const auto written = writer.Write(field.field, value); written.HasError())
                return written;
        }
        return Result<void>::Success();
    }

    /** @copydoc SceneReplicationCommitSource::IsCurrent */
    bool SceneReplicationCommitSource::IsCurrent(const ReplicationCommittedRead &read) const noexcept {
        return owner_ == std::this_thread::get_id() && reading_.has_value() && committed_.IsCurrent() && read.simulationTick == tick_ &&
               read.sourceRevision == revision_ && read.commitRevision == revision_;
    }

    /** @copydoc SceneReplicationCommitSource::EndRead */
    void SceneReplicationCommitSource::EndRead(const ReplicationCommittedRead &) const noexcept {
        if (owner_ == std::this_thread::get_id())
            reading_.reset();
    }
}  // namespace Horo::Network
