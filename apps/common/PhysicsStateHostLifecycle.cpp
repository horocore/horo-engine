#include "PhysicsStateHostLifecycle.h"

#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateProcessBinding.h"

namespace Horo::Application::Internal {
    /** @copydoc ProductPhysicsStateBudget */
    Physics::PhysicsStateRuntimeBudget ProductPhysicsStateBudget() noexcept {
        return {.maximumCaptureOwners = 8,
                .maximumRetainedBytes = 256U * 1024U * 1024U,
                .construction = {4096, 16384, 4096, 128, 16U * 1024U * 1024U},
                .constructionTracking = Physics::PhysicsStateConstructionTracking::Required};
    }

    /** @copydoc ProductPhysicsSaveWorldSettings */
    Result<Physics::PhysicsWorldSettings> ProductPhysicsSaveWorldSettings() {
        Physics::PhysicsWorldSettingsDescriptor descriptor;
        descriptor.world.capacity = {1024, 2048, 1024, 16U * 1024U * 1024U};
        descriptor.budgets.maximumShapes = 2048;
        descriptor.budgets.maximumContactPairs = 4096;
        descriptor.budgets.maximumContactConstraints = 1024;
        descriptor.budgets.maximumInFlightPairs = 4096;
        descriptor.budgets.maximumEvents = 4096;
        descriptor.budgets.scratchBytes = 8U * 1024U * 1024U;
        return Physics::PhysicsWorldSettings::Capture(descriptor);
    }

    /** @copydoc PrepareProductPhysicsState */
    Result<std::optional<Error>> PrepareProductPhysicsState(Physics::PhysicsRuntime &runtime) {
        using Outcome = Result<std::optional<Error>>;
        if (runtime.Mode() == Physics::PhysicsRuntimeMode::Null)
            return Outcome::Success(std::nullopt);
        auto bound = BindProcessStaticPhysicsState(runtime, ProductPhysicsStateBudget());
        if (bound.HasValue())
            return Outcome::Success(std::nullopt);
        const auto &error = bound.ErrorValue();
        if (error.domain.Value() == "horo.platform.process_image" && error.code.Value() == "platform.process_image.unavailable")
            return Outcome::Success(error);
        return Outcome::Failure(error);
    }

    /** @copydoc PollProductPhysicsRetirement */
    Result<bool> PollProductPhysicsRetirement(std::unique_ptr<Physics::PhysicsRuntime> &runtime,
                                              std::optional<Physics::PhysicsRuntimeRetirement> &retirement, std::uint32_t maximumRecords) {
        using Outcome = Result<bool>;
        if (!maximumRecords)
            return Outcome::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid));
        if (!retirement) {
            if (!runtime)
                return Outcome::Success(true);
            auto begun = runtime->BeginRetirement();
            if (begun.HasError())
                return Outcome::Failure(begun.ErrorValue());
            retirement.emplace(std::move(begun).Value());
        }
        auto collected = retirement->Collect(maximumRecords);
        if (collected.HasError())
            return Outcome::Failure(collected.ErrorValue());
        if (collected.Value().phase != Physics::PhysicsRuntimeRetirementPhase::Drained)
            return Outcome::Success(false);
        runtime.reset();
        retirement.reset();
        return Outcome::Success(true);
    }
}  // namespace Horo::Application::Internal
