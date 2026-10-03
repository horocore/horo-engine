#pragma once

#include "Horo/Runtime/Scene/RuntimeScene.h"

namespace Horo::Runtime::TestSupport {
    /** @brief Aggregate rejection fixture; borrowed flag and static descriptor outlive the service. */
    class PublicationGate final : public SceneActivationCandidate {
    public:
        PublicationGate(const bool &fail, const ErrorCodeDescriptor &error) noexcept : fail_(&fail), error_(&error) {}

        [[nodiscard]] Result<void> ValidatePublication() const override {
            return *fail_ ? Result<void>::Failure(MakeError(*error_)) : Result<void>::Success();
        }

        void Shutdown() noexcept override {}

    private:
        const bool *fail_{};
        const ErrorCodeDescriptor *error_{};
    };

    /** @brief Explicitly owned host participant used to reject a later aggregate publication barrier. */
    class PublicationGateParticipant final : public SceneActivationParticipant {
    public:
        PublicationGateParticipant(const bool &fail, const ErrorCodeDescriptor &error) noexcept : fail_(&fail), error_(&error) {}

        [[nodiscard]] Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &, RuntimeSceneView) override {
            return Result<std::unique_ptr<SceneActivationCandidate>>::Success(std::make_unique<PublicationGate>(*fail_, *error_));
        }

    private:
        const bool *fail_{};
        const ErrorCodeDescriptor *error_{};
    };
}  // namespace Horo::Runtime::TestSupport
