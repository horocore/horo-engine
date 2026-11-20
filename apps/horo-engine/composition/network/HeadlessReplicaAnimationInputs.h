#pragma once

#include "HeadlessNetworkServices.h"
#include "Horo/Network/ReplicationInboundApply.h"

namespace Horo::Application::Internal::HeadlessNetworkDetail {
    /** @brief Validates the explicit client animation input selection at composition. */
    [[nodiscard]] Result<void> ValidateReplicaAnimationBindings(HeadlessSceneActivationInput &input);

    /** @brief Client Scene input owned by one World and fed only by committed replication transactions. */
    class ReplicaAnimationInputs final : public Runtime::SceneAnimationInputSource {
    public:
        /** @brief Copies the validated actor and field bindings for one client Scene. */
        explicit ReplicaAnimationInputs(std::span<const HeadlessReplicaAnimationBinding> bindings);

        /** @copydoc Runtime::SceneAnimationInputSource::Read */
        [[nodiscard]] Runtime::SceneAnimationInputSnapshot Read(Runtime::EntityRef entity,
                                                                const Runtime::FixedStepContext &context) const noexcept override;

        /** @brief Composes presentation publication with the existing Scene transaction owner. */
        [[nodiscard]] static std::shared_ptr<Network::IReplicationApplyOwner> Bind(std::shared_ptr<ReplicaAnimationInputs> inputs,
                                                                                   std::shared_ptr<Network::IReplicationApplyOwner> scene);

    private:
        struct Row final {
            Runtime::SceneObjectId object;
            Runtime::EntityRef entity;
            CancellationToken admission;
            CancellationToken world;
            std::vector<Animation::GraphParameterInput> parameters;
        };

        class ApplyOwner;
        class Candidate;

        /** @brief Copies only the changed actors without exposing the pending transaction to animation. */
        [[nodiscard]] std::vector<Row> Stage(std::span<const Network::ReplicationApplyUpdate> updates) const;

        /** @brief Replaces staged actors using storage reserved for the admitted bindings. */
        void Publish(std::vector<Row> &rows) noexcept;

        std::vector<HeadlessReplicaAnimationBinding> bindings_;
        std::vector<Row> published_;
    };
}  // namespace Horo::Application::Internal::HeadlessNetworkDetail
