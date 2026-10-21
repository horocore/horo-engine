#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderResourceRegistry.h"

#include <array>
#include <memory>
#include <type_traits>
#include <variant>

namespace Horo::Render {
    namespace {
        /** @brief Allocation-free evidence that execution unwound after its frame cleanup. */
        struct MaterialExecutionExceptionTag {};
    }  // namespace

    /** @brief Bounded borrowed draw identities resolved before one completion lease is acquired. */
    struct RenderFrameScope::MaterialExecutionInstances {
        std::array<RenderMaterialDrawInstance, RenderMaterialPass::MaximumDraws> draws;
        std::array<Detail::RenderGraphResidentIdentity, RenderMaterialPass::MaximumDraws + 1> pins;
    };

    /** @copydoc RenderFrameScope::ResolveMaterialDraws */
    Result<void> RenderFrameScope::ResolveMaterialDraws(const RenderMaterialPass &pass, MaterialExecutionInstances &instances) const {
        instances.pins[0] = {Detail::RenderResourceClass::RenderTarget, {pass.target.owner, pass.target.slot, pass.target.generation}};
        for (std::size_t index = 0; index < pass.draws.size(); ++index) {
            const auto &draw = pass.draws[index];
            if (!draw.material || draw.material->Id().owner != owner_->resourceRegistry_->Owner())
                return Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady));
            const auto mesh = owner_->BackendInstance(draw.mesh);
            if (mesh.HasError())
                return Result<void>::Failure(mesh.ErrorValue());
            instances.draws[index] = {mesh.Value(), &draw.material->BackendBinding().NativeBinding()};
            instances.pins[index + 1] = {Detail::RenderResourceClass::Mesh, {draw.mesh.owner, draw.mesh.slot, draw.mesh.generation}};
        }
        return Result<void>::Success();
    }

    /** @copydoc RenderFrameScope::ExecuteMaterialRequest */
    Result<void> RenderFrameScope::ExecuteMaterialRequest(RenderMaterialExecutionRequest request) {
        RenderGraphLeaseTransfer transfer;
        const auto release = [&transfer](IRenderGraphResourceLease *lease) {
            if (transfer.authority == RenderGraphLeaseAuthority::Frontend)
                lease->Release();
        };
        std::unique_ptr<IRenderGraphResourceLease, decltype(release)> lease{request.lease, release};
        request.transfer = &transfer;
        if (const auto consumed = backend_->ExecuteMaterialPass(request); consumed.HasError()) {
            Abort();
            return consumed;
        }
        // Unsupported/default adapters never accept a lease; fail closed if a success omitted completion ownership.
        if (transfer.authority != RenderGraphLeaseAuthority::BackendCompletion) {
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady));
        }
        static_cast<void>(lease.release());
        return Result<void>::Success();
    }

    /** @brief Stack-only transport separating completed exception cleanup from fallible owned error construction. */
    struct RenderFrameScope::MaterialExecutionOutcome {
        std::variant<Result<void>, MaterialExecutionExceptionTag> value;

        MaterialExecutionOutcome() noexcept : value(std::in_place_type<MaterialExecutionExceptionTag>) {}

        explicit MaterialExecutionOutcome(Result<void> result) noexcept : value(std::move(result)) {}
    };

    /** @copydoc RenderFrameScope::ExecuteMaterialPassBoundary */
    RenderFrameScope::MaterialExecutionOutcome RenderFrameScope::ExecuteMaterialPassBoundary(const RenderMaterialPass &pass) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<Error> && std::is_nothrow_move_constructible_v<Result<void>> &&
                      std::is_nothrow_move_constructible_v<MaterialExecutionOutcome>);
        try {
            if (const auto admitted = ValidateExecutionAdmission(); admitted.HasError())
                return MaterialExecutionOutcome{admitted};
            if (!owner_->IsLiveTarget(pass.target, pass.extent) || !pass.color.IsValid() || pass.draws.empty() ||
                pass.draws.size() > RenderMaterialPass::MaximumDraws) {
                Abort();
                return MaterialExecutionOutcome{Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady))};
            }
            if (pass.submission.has_value() && !backend_->Capabilities().supportsExplicitMaterialSubmissions) {
                Abort();
                return MaterialExecutionOutcome{Result<void>::Failure(MakeError(MaterialBindingErrors::Unsupported))};
            }
            const auto target = owner_->BackendInstance(pass.target);
            if (target.HasError()) {
                Abort();
                return MaterialExecutionOutcome{Result<void>::Failure(target.ErrorValue())};
            }
            MaterialExecutionInstances instances;
            if (const auto resolved = ResolveMaterialDraws(pass, instances); resolved.HasError()) {
                Abort();
                return MaterialExecutionOutcome{resolved};
            }
            const auto retained =
                owner_->graphResourceLeases_->AcquireMaterial(std::span{instances.pins}.first(pass.draws.size() + 1), pass.draws);
            if (retained.HasError()) {
                Abort();
                return MaterialExecutionOutcome{Result<void>::Failure(retained.ErrorValue())};
            }
            if (const auto consumed = ExecuteMaterialRequest({frame_, target.Value(), pass.extent, pass.color, pass.depthClear,
                                                              std::span{instances.draws}.first(pass.draws.size()), retained.Value(),
                                                              nullptr, pass.submission});
                consumed.HasError())
                return MaterialExecutionOutcome{consumed};
            executed_ = true;
            return MaterialExecutionOutcome{Result<void>::Success()};
        } catch (...) {
            // Expected failures may already have aborted before their owned error copy threw.
            if (backend_ != nullptr)
                Abort();
            return MaterialExecutionOutcome{};
        }
    }

    /** @copydoc RenderFrameScope::ExecuteMaterialPass */
    Result<void> RenderFrameScope::ExecuteMaterialPass(const RenderMaterialPass &pass) {
        auto outcome = ExecuteMaterialPassBoundary(pass);
        if (auto *original = std::get_if<Result<void>>(&outcome.value))
            return std::move(*original);
        // Cleanup and lease disposition are complete before this fallible human-facing error construction.
        return Result<void>::Failure(MakeError(FrontendErrors::FrameException));
    }
}  // namespace Horo::Render
