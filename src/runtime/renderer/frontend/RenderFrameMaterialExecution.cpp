#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderResourceRegistry.h"
#include "RenderSubmissionAllocator.h"

#include <array>
#include <memory>
#include <type_traits>
#include <variant>

namespace Horo::Render {
    namespace {
        /** @brief Checks the bounded immutable draw envelope before any frontend lease is acquired. */
        bool ValidMaterialDrawEnvelope(const RenderMaterialPass &pass) {
            return pass.color.IsValid() && (!pass.draws.empty() || (pass.depthOnly && pass.depthClear.has_value())) &&
                   pass.draws.size() <= RenderMaterialPass::MaximumDraws;
        }

        Result<void> ValidateBatchCapability(const RenderMaterialPass &pass, const RenderBackendCapabilities &capabilities) {
            if (pass.precedingPasses.size() >= RenderMaterialPass::MaximumPasses)
                return Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady));
            if (pass.submission && !capabilities.supportsExplicitMaterialSubmissions)
                return Result<void>::Failure(MakeError(MaterialBindingErrors::Unsupported));
            const bool extended = !pass.precedingPasses.empty() || pass.depthOnly || pass.region.has_value();
            if (extended && !capabilities.supportsMaterialPassBatches)
                return Result<void>::Failure(MakeError(MaterialBindingErrors::Unsupported));
            return Result<void>::Success();
        }

        /** @brief Allocation-free evidence that execution unwound after its frame cleanup. */
        struct MaterialExecutionExceptionTag {};
    }  // namespace

    /** @brief Bounded borrowed draw identities resolved before one completion lease is acquired. */
    struct RenderFrameScope::MaterialExecutionInstances {
        std::array<RenderMaterialDrawInstance, RenderMaterialPass::MaximumBatchDraws> draws;
        std::array<RenderMaterialDraw, RenderMaterialPass::MaximumBatchDraws> sources;
        std::array<Detail::RenderGraphResidentIdentity, RenderMaterialPass::MaximumBatchDraws + RenderMaterialPass::MaximumPasses> pins;
        std::array<RenderMaterialExecutionRequest, RenderMaterialPass::MaximumPasses> requests;
        std::size_t drawCount{}, pinCount{}, passCount{};

        Result<void> Prepare(RenderFrameScope &frame, const RenderMaterialPass &pass) {
            if (auto valid = ValidateBatchCapability(pass, frame.backend_->Capabilities()); valid.HasError())
                return valid;
            if (auto preceding = PreparePreceding(frame, pass.precedingPasses); preceding.HasError())
                return preceding;
            return frame.ResolveMaterialDraws(pass, *this);
        }

        Result<void> PreparePreceding(RenderFrameScope &frame, std::span<const RenderMaterialPass> preceding) {
            for (const auto &pass : preceding) {
                if (!pass.precedingPasses.empty() || pass.submission)
                    return Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady));
                if (auto resolved = frame.ResolveMaterialDraws(pass, *this); resolved.HasError())
                    return resolved;
            }
            return Result<void>::Success();
        }

        Result<void> Execute(RenderFrameScope &frame) {
            const auto retained =
                frame.owner_->graphResourceLeases_->AcquireMaterial(std::span{pins}.first(pinCount), std::span{sources}.first(drawCount));
            if (retained.HasError())
                return Result<void>::Failure(retained.ErrorValue());
            auto request = requests[passCount - 1];
            request.lease = retained.Value();
            request.precedingPasses = std::span{requests}.first(passCount - 1);
            return frame.ExecuteMaterialRequest(request);
        }
    };

    /** @copydoc RenderFrameScope::ResolveMaterialDraws */
    Result<void> RenderFrameScope::ResolveMaterialDraws(const RenderMaterialPass &pass, MaterialExecutionInstances &instances) const {
        if (!owner_->IsLiveTarget(pass.target, pass.extent) || !ValidMaterialDrawEnvelope(pass) ||
            pass.draws.size() > instances.draws.size() - instances.drawCount)
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady));
        const auto target = owner_->BackendInstance(pass.target);
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        const auto begin = instances.drawCount;
        instances.pins[instances.pinCount++] = {Detail::RenderResourceClass::RenderTarget,
                                                {pass.target.owner, pass.target.slot, pass.target.generation}};
        for (std::size_t index = 0; index < pass.draws.size(); ++index) {
            const auto &draw = pass.draws[index];
            if (!draw.material || draw.material->Id().owner != owner_->resourceRegistry_->Owner())
                return Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady));
            const auto mesh = owner_->BackendInstance(draw.mesh);
            if (mesh.HasError())
                return Result<void>::Failure(mesh.ErrorValue());
            instances.sources[instances.drawCount] = draw;
            instances.draws[instances.drawCount++] = {mesh.Value(), &draw.material->BackendBinding().NativeBinding()};
            instances.pins[instances.pinCount++] = {Detail::RenderResourceClass::Mesh,
                                                    {draw.mesh.owner, draw.mesh.slot, draw.mesh.generation}};
        }
        instances.requests[instances.passCount++] = {frame_,          target.Value(),
                                                     pass.extent,     pass.color,
                                                     pass.depthClear, std::span{instances.draws}.subspan(begin, pass.draws.size()),
                                                     nullptr,         nullptr,
                                                     pass.submission, pass.depthOnly,
                                                     pass.region,     {}};
        return Result<void>::Success();
    }

    /** @copydoc RenderFrameScope::ExecuteMaterialRequest */
    Result<void> RenderFrameScope::ExecuteMaterialRequest(RenderMaterialExecutionRequest request) {
        auto submissionHistory = *owner_->submissionAllocator_;
        RenderGraphLeaseTransfer transfer;
        const auto release = [&transfer](IRenderGraphResourceLease *lease) {
            if (transfer.authority == RenderGraphLeaseAuthority::Frontend)
                lease->Release();
        };
        std::unique_ptr<IRenderGraphResourceLease, decltype(release)> lease{request.lease, release};
        if (request.submission) {
            if (auto imported = submissionHistory.Admit(std::span{&*request.submission, 1}); imported.HasError()) {
                Abort();
                return imported;
            }
        }
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
        completionLease_ = lease.release();
        *owner_->submissionAllocator_ = submissionHistory;
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
            MaterialExecutionInstances instances;
            if (auto prepared = instances.Prepare(*this, pass); prepared.HasError()) {
                Abort();
                return MaterialExecutionOutcome{prepared};
            }
            if (auto consumed = instances.Execute(*this); consumed.HasError()) {
                if (backend_ != nullptr)
                    Abort();
                return MaterialExecutionOutcome{consumed};
            }
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
