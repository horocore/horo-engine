#include "Horo/Network/SceneReplicationCommitSource.h"

#include <algorithm>
#include <bitset>
#include <limits>
#include <new>

namespace Horo::Network {
    namespace {
        /** @brief Copies the same bounded field contract for either Scene ownership mode. */
        Result<std::vector<SceneReplicationFieldBinding>> CaptureBindings(std::span<const SceneReplicationFieldBinding> fields) {
            using Bindings = Result<std::vector<SceneReplicationFieldBinding>>;
            if (fields.empty() || fields.size() > 1024)
                return Bindings::Failure(MakeError(ReplicationCaptureErrors::Invalid));
            std::vector<SceneReplicationFieldBinding> bindings{fields.begin(), fields.end()};
            std::ranges::sort(bindings, {}, &SceneReplicationFieldBinding::field);
            for (std::size_t index{}; index < bindings.size(); ++index) {
                if (!bindings[index].field.IsValid() || bindings[index].property > SceneReplicationProperty::TranslationZ ||
                    (index > 0 && bindings[index - 1].field == bindings[index].field))
                    return Bindings::Failure(MakeError(ReplicationCaptureErrors::Invalid));
            }
            return Bindings::Success(std::move(bindings));
        }

        /** @brief Accepts only the committed attempt of the current dispatch. */
        bool IsCommittedAttempt(const Runtime::RuntimeDispatchFacts &facts) noexcept {
            return facts.committedTick == facts.fixedTick && facts.committedAttempt == facts.fixedAttempt &&
                   facts.committedFrame == facts.frame;
        }

        /** @brief The explicit observed Scene projection owns one field per supported transform axis. */
        bool UniqueObservedAxes(std::span<const SceneReplicationFieldBinding> validated) {
            std::bitset<static_cast<std::size_t>(SceneReplicationProperty::TranslationZ) + 1> used;
            for (const auto &binding : validated) {
                const auto axis = static_cast<std::size_t>(binding.property);
                if (used.test(axis))
                    return false;
                used.set(axis);
            }
            return true;
        }
    }  // namespace

    /** @copydoc SceneReplicationCommitSource::SceneReplicationCommitSource */
    SceneReplicationCommitSource::SceneReplicationCommitSource(ConstructionKey, std::shared_ptr<Runtime::RuntimeScene> scene,
                                                               std::vector<SceneReplicationFieldBinding> fields)
        : scene_(std::move(scene)), fields_(std::move(fields)) {}

    /** @copydoc SceneReplicationCommitSource::SceneReplicationCommitSource */
    SceneReplicationCommitSource::SceneReplicationCommitSource(ConstructionKey, Runtime::SceneRuntimeId scene,
                                                               Runtime::RuntimeDispatchSource dispatch, ReplicationSchemaId schema,
                                                               ReplicationSchemaVersion version,
                                                               std::vector<SceneReplicationFieldBinding> fields,
                                                               std::weak_ptr<const void> sceneOwner)
        : expectedScene_(scene), dispatch_(std::move(dispatch)), schema_(schema), version_(version), fields_(std::move(fields)),
          sceneOwner_(std::move(sceneOwner)) {}

    /** @copydoc SceneReplicationCommitSource::Create */
    Result<std::shared_ptr<SceneReplicationCommitSource>> SceneReplicationCommitSource::Create(
        std::shared_ptr<Runtime::RuntimeScene> scene, const std::span<const SceneReplicationFieldBinding> fields) {
        if (!scene)
            return Result<std::shared_ptr<SceneReplicationCommitSource>>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
        try {
            auto bindings = CaptureBindings(fields);
            if (bindings.HasError())
                return Result<std::shared_ptr<SceneReplicationCommitSource>>::Failure(std::move(bindings).ErrorValue());
            return Result<std::shared_ptr<SceneReplicationCommitSource>>::Success(
                std::make_shared<SceneReplicationCommitSource>(ConstructionKey{}, std::move(scene), std::move(bindings).Value()));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<SceneReplicationCommitSource>>::Failure(MakeError(ReplicationCaptureErrors::Capacity));
        }
    }

    /** @copydoc SceneReplicationCommitSource::CreateObserved */
    Result<std::shared_ptr<SceneReplicationCommitSource>> SceneReplicationCommitSource::CreateObserved(
        Runtime::SceneRuntimeId scene, Runtime::RuntimeDispatchSource dispatch, ReplicationSchemaId schema,
        ReplicationSchemaVersion version, const std::span<const SceneReplicationFieldBinding> fields,
        std::weak_ptr<const void> sceneOwner) {
        using Source = Result<std::shared_ptr<SceneReplicationCommitSource>>;
        if (!scene.IsValid() || !dispatch.IsValid() || !schema.IsValid() || !version.IsValid() || sceneOwner.expired())
            return Source::Failure(MakeError(ReplicationCaptureErrors::Invalid));
        try {
            auto bindings = CaptureBindings(fields);
            if (bindings.HasError())
                return Source::Failure(std::move(bindings).ErrorValue());
            if (!UniqueObservedAxes(bindings.Value()))
                return Source::Failure(MakeError(ReplicationCaptureErrors::Invalid));
            return Source::Success(std::make_shared<SceneReplicationCommitSource>(ConstructionKey{}, scene, std::move(dispatch), schema,
                                                                                  version, std::move(bindings).Value(),
                                                                                  std::move(sceneOwner)));
        } catch (const std::bad_alloc &) {
            return Source::Failure(MakeError(ReplicationCaptureErrors::Capacity));
        }
    }

    /** @copydoc SceneReplicationCommitSource::ObserveCommittedTick */
    bool SceneReplicationCommitSource::ObserveCommittedTick(Runtime::RuntimeSceneView view,
                                                            const Runtime::RuntimeDispatchEvidence &evidence) noexcept {
        if (owner_ != std::this_thread::get_id() || reading_.has_value() || revision_ == std::numeric_limits<std::uint64_t>::max())
            return false;
        Runtime::RuntimeDispatchFacts facts;
        if (evidence.Read(dispatch_, Runtime::RuntimePhase::FixedUpdate, facts) != Runtime::RuntimeDispatchStatus::Valid ||
            !IsCommittedAttempt(facts) || facts.committedTick <= tick_)
            return false;
        const auto owner = PinSceneOwner();
        if (!owner || view.RuntimeId() != expectedScene_ || !view.IsCurrent())
            return false;
        committed_ = std::move(view);
        tick_ = facts.committedTick;
        ++revision_;
        return true;
    }

    /** @copydoc SceneReplicationCommitSource::CommitSimulationTick */
    Result<Runtime::StructuralCommitResult> SceneReplicationCommitSource::CommitSimulationTick(const Runtime::SceneCommandBuffer &commands,
                                                                                               const std::uint64_t simulationTick) {
        if (owner_ != std::this_thread::get_id() || !scene_ || reading_.has_value() || simulationTick == 0 || simulationTick <= tick_ ||
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

    /** @copydoc SceneReplicationCommitSource::AcquireCaptureReadAfterCommit */
    Result<ReplicationWorldCaptureRead> SceneReplicationCommitSource::AcquireCaptureReadAfterCommit(
        const ReplicationWorldLifecycle &world, const Runtime::RuntimePhase phase, const CancellationToken &cancellation) const {
        using Read = Result<ReplicationWorldCaptureRead>;
        if (owner_ != std::this_thread::get_id() || tick_ == 0)
            return Read::Failure(MakeError(ReplicationCaptureErrors::Uncommitted));
        const auto owner = PinSceneOwner();
        if (!owner || !committed_.IsCurrent())
            return Read::Failure(MakeError(ReplicationCaptureErrors::Uncommitted));
        const auto active = world.ActiveDescriptor();
        if (active.HasError())
            return Read::Failure(active.ErrorValue());
        return world.AcquireCaptureRead({committed_.RuntimeId(), active.Value().session, phase, tick_, cancellation});
    }

    /** @copydoc SceneReplicationCommitSource::CaptureAfterCommit */
    Result<ReplicationCaptureReport> SceneReplicationCommitSource::CaptureAfterCommit(const ReplicationWorldLifecycle &world,
                                                                                      ReplicationStateCapture &capture,
                                                                                      const Runtime::RuntimePhase phase,
                                                                                      const CancellationToken &cancellation) const {
        const auto read = AcquireCaptureReadAfterCommit(world, phase, cancellation);
        if (read.HasError())
            return Result<ReplicationCaptureReport>::Failure(read.ErrorValue());
        return capture.CaptureAtCommit(read.Value(), read.Value().SimulationTick(), cancellation);
    }

    /** @copydoc SceneReplicationCommitSource::BeginRead */
    Result<ReplicationCommittedRead> SceneReplicationCommitSource::BeginRead(const NetworkObjectMappingEntry &object,
                                                                             const std::uint64_t tick) const {
        if (owner_ != std::this_thread::get_id() || reading_.has_value() || tick != tick_ || tick_ == 0)
            return Result<ReplicationCommittedRead>::Failure(MakeError(ReplicationCaptureErrors::Uncommitted));
        if (schema_.IsValid() && (object.provenance.schema != schema_ || object.provenance.schemaVersion != version_))
            return Result<ReplicationCommittedRead>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
        auto pin = PinSceneOwner();
        if (!pin)
            return Result<ReplicationCommittedRead>::Failure(MakeError(ReplicationCaptureErrors::Stale));
        if (!committed_.IsCurrent())
            return Result<ReplicationCommittedRead>::Failure(MakeError(ReplicationCaptureErrors::Uncommitted));
        if (const auto entity = committed_.Get(object.entity); entity.HasError())
            return Result<ReplicationCommittedRead>::Failure(entity.ErrorValue());
        readOwner_ = std::move(pin);
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
        return owner_ == std::this_thread::get_id() && reading_.has_value() && readOwner_ && read.simulationTick == tick_ &&
               read.sourceRevision == revision_ && read.commitRevision == revision_ && committed_.IsCurrent();
    }

    /** @copydoc SceneReplicationCommitSource::EndRead */
    void SceneReplicationCommitSource::EndRead(const ReplicationCommittedRead &) const noexcept {
        if (owner_ == std::this_thread::get_id()) {
            reading_.reset();
            readOwner_.reset();
        }
    }

    /** @copydoc SceneReplicationCommitSource::PinSceneOwner */
    std::shared_ptr<const void> SceneReplicationCommitSource::PinSceneOwner() const noexcept {
        return scene_ ? std::shared_ptr<const void>{scene_} : sceneOwner_.lock();
    }
}  // namespace Horo::Network
