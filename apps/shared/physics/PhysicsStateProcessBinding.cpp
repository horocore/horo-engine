#include "PhysicsStateProcessBinding.h"

#include "HoroPhysicsStateBuildFacts.h"
#include "PhysicsStateImagesInternal.h"
#include "ProcessImageIdentity.h"

namespace Horo::Application::Internal {
    namespace {
        const ErrorCodeDescriptor MissingCodeImage{.domain = ErrorDomainId{"horo.application.physics_state"},
                                                   .code = ErrorCode{"application.physics_state.image_unavailable"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The selected Physics composition has no verified static process image.",
                                                   .remediationHint =
                                                       "Provide the selected native adapter and solver in the actual executing image.",
                                                   .retryable = false,
                                                   .userActionable = true};

        /** @brief Requires both actual native adapter and actual solver implementations in the verified executable. */
        Result<void> VerifyStaticPhysics(const Platform::ProcessImageIdentity &identity,
                                         Physics::Detail::PhysicsStateImageAddresses addresses) {
            const auto adapter = identity.VerifyStaticCodeAddress(addresses.adapter);
            if (adapter.HasError())
                return adapter;
            return identity.VerifyStaticCodeAddress(addresses.solver);
        }
    }  // namespace

    /** @copydoc BindProcessStaticPhysicsState */
    Result<void> BindProcessStaticPhysicsState(Physics::PhysicsRuntime &runtime, Physics::PhysicsStateRuntimeBudget budget) {
        static_assert(Physics::Detail::StateBuildFacts::ContractVersion == 1);
        const auto addresses = Physics::Detail::ReadPhysicsStateImageAddresses();
        if (!addresses.adapter || !addresses.solver)
            return Result<void>::Failure(MakeError(MissingCodeImage));
        const auto identity = Platform::CaptureCurrentProcessImageIdentity();
        if (identity.HasError())
            return Result<void>::Failure(identity.ErrorValue());
        const auto verified = VerifyStaticPhysics(identity.Value(), addresses);
        if (verified.HasError())
            return verified;
        return runtime.BindStateSnapshotBuild({.executableArtifact = identity.Value().Artifact(),
                                               .physicsArtifact = identity.Value().Artifact(),
                                               .platformAbi = Physics::Detail::StateBuildFacts::PlatformAbi,
                                               .lifetime = Physics::PhysicsStateCodeLifetime::ProcessStatic,
                                               .codeLease = identity.Value().Lease()},
                                              budget);
    }
}  // namespace Horo::Application::Internal
