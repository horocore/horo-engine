#pragma once

/** @file GnsAuthoritativeAiSupport.h @brief Cooked AI qualification for the native peer pair. */

#include "HeadlessNetworkServices.h"

namespace Horo::Application::Internal {
    class NetworkProductHost;
}

namespace Horo::Network::NativeQualification {
    /** @brief Owns the same authored agent and committed Decision input on either peer. */
    class AuthoritativeAi final {
    public:
        /** @brief Compiles and commits the fixture's authored Decision plan for the selected target. */
        explicit AuthoritativeAi(const AssetCookTargetId &target);
        ~AuthoritativeAi();

        /** @brief Attaches the exact authored agent and controller to the Scene object. */
        void Attach(Runtime::RuntimeEntityDefinition &entity) const;
        /** @brief Supplies the committed plan and its bounded scheduler to the production composition. */
        void Configure(Application::Internal::HeadlessSceneActivationInput &input, JobSystem &jobs) const;
        /** @brief Verifies native task completion and Blackboard access belong to the authority session alone. */
        void Verify(const Application::Internal::NetworkProductHost &host, const INetworkModeService &scene, NetworkModeWorld world,
                    bool authority) const;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Network::NativeQualification
