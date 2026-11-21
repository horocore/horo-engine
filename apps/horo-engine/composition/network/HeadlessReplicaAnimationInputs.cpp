#include "HeadlessReplicaAnimationInputs.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>

namespace Horo::Application::Internal::HeadlessNetworkDetail {
    namespace {
        /** @brief Retains the scalar type validated by the Scene apply owner. */
        Animation::GraphParameterValue GraphParameter(const Network::ReplicationRuntimeValue &value) {
            if (const auto *number = std::get_if<double>(&value))
                return static_cast<float>(*number);
            return static_cast<std::int32_t>(std::get<std::int64_t>(value));
        }

        /** @brief Matches one authored object and one field declared in the public Scene schema. */
        bool IsPublicBinding(const HeadlessSceneActivationInput &input, const HeadlessReplicaAnimationBinding &binding) {
            return binding.object.IsValid() && binding.field.IsValid() && binding.parameter.IsValid() &&
                   std::ranges::find(input.publicTransforms->fields, binding.field, &Network::SceneReplicationFieldBinding::field) !=
                       input.publicTransforms->fields.end() &&
                   std::ranges::any_of(input.replicatedSceneObjects, [&](const auto &object) {
                return object.authoredObject == binding.object && object.schema == input.publicTransforms->schema &&
                       object.schemaVersion == input.publicTransforms->version;
            });
        }
    }  // namespace

    /** @copydoc ValidateReplicaAnimationBindings */
    Result<void> ValidateReplicaAnimationBindings(HeadlessSceneActivationInput &input) {
        auto &bindings = input.replicaAnimationBindings;
        if (bindings.empty())
            return Result<void>::Success();
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
        };
        constexpr auto maximumObjects = Network::ReplicationInboundLimits{}.maximumObjects;
        if (!input.animationIdentities || input.replicaAnimationInputs || !input.publicTransforms || !input.initialSceneState)
            return invalid();
        const auto maximumParametersPerActor = input.publicTransforms->fields.size();
        if (maximumParametersPerActor == 0 || (bindings.size() - 1) / maximumParametersPerActor >= maximumObjects)
            return invalid();
        std::ranges::sort(bindings, {}, [](const auto &binding) {
            return std::pair{binding.object, binding.parameter};
        });
        std::size_t actorCount{};
        for (auto first = bindings.begin(); first != bindings.end();) {
            const auto end = std::ranges::upper_bound(std::ranges::subrange{first, bindings.end()}, first->object, {},
                                                      &HeadlessReplicaAnimationBinding::object);
            if (const std::span group{first, end};
                ++actorCount > maximumObjects || group.size() > maximumParametersPerActor ||
                std::ranges::adjacent_find(group, {}, &HeadlessReplicaAnimationBinding::parameter) != group.end() ||
                !std::ranges::all_of(group, [&](const auto &binding) {
                return IsPublicBinding(input, binding);
            }))
                return invalid();
            first = end;
        }
        return Result<void>::Success();
    }

    /** @copydoc ReplicaAnimationInputs::ReplicaAnimationInputs */
    ReplicaAnimationInputs::ReplicaAnimationInputs(std::span<const HeadlessReplicaAnimationBinding> bindings)
        : bindings_(bindings.begin(), bindings.end()) {
        std::ranges::sort(bindings_, {}, &HeadlessReplicaAnimationBinding::object);
        published_.reserve(std::min(bindings_.size(), Network::ReplicationInboundLimits{}.maximumObjects));
    }

    /** @copydoc ReplicaAnimationInputs::Read */
    Runtime::SceneAnimationInputSnapshot ReplicaAnimationInputs::Read(Runtime::EntityRef entity,
                                                                      const Runtime::FixedStepContext &) const noexcept {
        const auto row = std::ranges::lower_bound(published_, entity, {}, &Row::entity);
        if (row == published_.end() || row->entity != entity || row->admission.IsCancellationRequested() ||
            row->world.IsCancellationRequested())
            return {};
        return {.parameters = row->parameters};
    }

    /** @copydoc ReplicaAnimationInputs::Stage */
    std::vector<ReplicaAnimationInputs::Row> ReplicaAnimationInputs::Stage(std::span<const Network::ReplicationApplyUpdate> updates) const {
        std::vector<Row> rows;
        rows.reserve(std::min(updates.size(), published_.capacity()));
        for (const auto &update : updates) {
            const auto object = update.mapping.provenance.authoredObject;
            if (!object)
                continue;
            const auto bindings = std::ranges::equal_range(bindings_, *object, {}, &HeadlessReplicaAnimationBinding::object);
            if (bindings.empty())
                continue;
            Row row{*object, update.mapping.entity, update.cancellation, update.worldCancellation, {}};
            row.parameters.reserve(static_cast<std::size_t>(bindings.size()));
            const auto fields = update.state.Fields();
            for (const auto &binding : bindings) {
                const auto field = std::ranges::find(fields, binding.field, &Network::ReplicationCapturedField::field);
                if (field != fields.end())
                    row.parameters.push_back({binding.parameter, GraphParameter(field->value)});
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    /** @copydoc ReplicaAnimationInputs::Publish */
    void ReplicaAnimationInputs::Publish(std::vector<Row> &rows) noexcept {
        if (rows.empty())
            return;
        for (auto &row : rows) {
            const auto previous = std::ranges::find(published_, row.object, &Row::object);
            if (previous == published_.end())
                published_.push_back(std::move(row));
            else
                *previous = std::move(row);
        }
        std::ranges::sort(published_, {}, &Row::entity);
    }

    /** @brief Publishes prepared graph inputs only after the exact Scene candidate commits. */
    class ReplicaAnimationInputs::Candidate final : public Network::IReplicationApplyCandidate {
    public:
        Candidate(std::shared_ptr<ReplicaAnimationInputs> inputs, std::unique_ptr<Network::IReplicationApplyCandidate> scene,
                  std::vector<Row> rows)
            : inputs_(std::move(inputs)), scene_(std::move(scene)), rows_(std::move(rows)) {}

        /** @copydoc Network::IReplicationApplyCandidate::Commit */
        Result<void> Commit(const CancellationToken &cancellation) override {
            auto committed = scene_->Commit(cancellation);
            if (committed.HasValue())
                inputs_->Publish(rows_);
            return committed;
        }

    private:
        std::shared_ptr<ReplicaAnimationInputs> inputs_;
        std::unique_ptr<Network::IReplicationApplyCandidate> scene_;
        std::vector<Row> rows_;
    };

    /** @brief Retains the Scene transaction boundary for Spawn and Update records. */
    class ReplicaAnimationInputs::ApplyOwner final : public Network::IReplicationApplyOwner {
    public:
        ApplyOwner(std::shared_ptr<ReplicaAnimationInputs> inputs, std::shared_ptr<Network::IReplicationApplyOwner> scene)
            : inputs_(std::move(inputs)), scene_(std::move(scene)) {}

        /** @copydoc Network::IReplicationApplyOwner::Prepare */
        Result<std::unique_ptr<Network::IReplicationApplyCandidate>> Prepare(
            std::span<const Network::ReplicationApplyUpdate> updates) override {
            using Prepared = Result<std::unique_ptr<Network::IReplicationApplyCandidate>>;
            auto scene = scene_->Prepare(updates);
            if (scene.HasError())
                return Prepared::Failure(scene.ErrorValue());
            return Prepared::Success(std::make_unique<Candidate>(inputs_, std::move(scene).Value(), inputs_->Stage(updates)));
        }

    private:
        std::shared_ptr<ReplicaAnimationInputs> inputs_;
        std::shared_ptr<Network::IReplicationApplyOwner> scene_;
    };

    /** @copydoc ReplicaAnimationInputs::Bind */
    std::shared_ptr<Network::IReplicationApplyOwner> ReplicaAnimationInputs::Bind(std::shared_ptr<ReplicaAnimationInputs> inputs,
                                                                                  std::shared_ptr<Network::IReplicationApplyOwner> scene) {
        return std::make_shared<ApplyOwner>(std::move(inputs), std::move(scene));
    }
}  // namespace Horo::Application::Internal::HeadlessNetworkDetail
