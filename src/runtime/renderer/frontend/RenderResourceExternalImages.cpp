#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "Horo/Runtime/Render/RenderGraphSynchronizationErrors.h"
#include "RenderExternalImages.h"
#include "RenderFrontendErrors.h"
#include "RenderResourceRegistry.h"

namespace Horo::Render::Detail {
    /** @copydoc RenderResourceRegistry::PrepareExternalNativeLifetime */
    Result<RenderResourceNativeLifetime> RenderResourceRegistry::PrepareExternalNativeLifetime(const RenderResourceIdentity identity) {
        const auto validated = slots_.Validate(RenderResourceClass::Texture, identity);
        if (validated.HasError())
            return Result<RenderResourceNativeLifetime>::Failure(validated.ErrorValue());
        auto &entry = slots_.At(validated.Value());
        if (entry.state != RenderResourceState::Pending || entry.nativeLease.claimed || !entry.externalSource)
            return Result<RenderResourceNativeLifetime>::Failure(MakeError(FrontendErrors::ResourceNotPending));
        // Foreign allocations have no Horo allocation charge but still use the same native lease.
        return Result<RenderResourceNativeLifetime>::Success(RenderResourceNativeLifetime{entry.nativeLease});
    }

    /** @copydoc RenderResourceRegistry::AttachExternalSource */
    Result<void> RenderResourceRegistry::AttachExternalSource(const RenderResourceIdentity identity,
                                                              std::shared_ptr<const IRenderExternalTextureSource> source) {
        const auto validated = slots_.Validate(RenderResourceClass::Texture, identity);
        if (validated.HasError())
            return Result<void>::Failure(validated.ErrorValue());
        auto &entry = slots_.At(validated.Value());
        if (entry.state != RenderResourceState::Pending || entry.externalSource || !source)
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceNotPending));
        if (auto current = source->Validate(); current.HasError())
            return current;
        entry.externalSource = std::move(source);
        return Result<void>::Success();
    }

    /** @copydoc RenderResourceRegistry::ValidateExternalRelease */
    Result<void> RenderResourceRegistry::ValidateExternalRelease(const RenderResourceIdentity identity,
                                                                 const CompiledRenderGraphExecution &graph,
                                                                 const RenderGraphResourceId resource) const {
        if (identity.slot >= slots_.Size() || identity.owner != owner_)
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceWrongOwner));
        const auto &entry = slots_.At(identity.slot);
        if (!entry.externalSource)
            return Result<void>::Success();
        if (identity.generation != entry.generation || entry.state != RenderResourceState::Ready)
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceStale));
        if (auto current = entry.externalSource->Validate(); current.HasError())
            return current;
        if (resource.owner != graph.Owner() || resource.value == 0 || resource.value > graph.Resources().size() ||
            graph.Resources()[resource.value - 1].resourceClass != RenderGraphResourceClass::External)
            return Result<void>::Failure(MakeError(RenderGraphSynchronizationErrors::InvalidExternalRelease));
        const auto required = entry.externalSource->RequiredFinalState();
        std::uint32_t matching{};
        for (const auto &release : graph.ExternalReleases()) {
            if (release.resource != resource)
                continue;
            if (release.newState != required)
                return Result<void>::Failure(MakeError(RenderGraphSynchronizationErrors::InvalidExternalRelease));
            ++matching;
        }
        return matching == 1 ? Result<void>::Success()
                             : Result<void>::Failure(MakeError(RenderGraphSynchronizationErrors::InvalidExternalRelease));
    }

    /** @copydoc RenderResourceRegistry::ExternalRetirementComplete */
    Result<bool> RenderResourceRegistry::ExternalRetirementComplete(const RenderResourceIdentity identity) const {
        if (identity.owner != owner_ || identity.slot == 0 || identity.slot >= slots_.Size() || identity.generation == 0)
            return Result<bool>::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
        const auto &entry = slots_.At(identity.slot);
        // Canonical registry retirement advances a generation only after its native lease is terminal.
        // The caller must retain the exact external handle issued to it, never synthesize a generation.
        if (identity.generation < entry.generation)
            return Result<bool>::Success(true);
        if (identity.generation != entry.generation)
            return Result<bool>::Failure(MakeError(FrontendErrors::ResourceStale));
        if (entry.state == RenderResourceState::Retired && entry.nativeLease.claimed && entry.nativeLease.released)
            return Result<bool>::Success(true);
        if (entry.resourceClass != RenderResourceClass::Texture || !entry.externalSource)
            return Result<bool>::Failure(MakeError(FrontendErrors::ResourceWrongType));
        return Result<bool>::Success(entry.state == RenderResourceState::Retired && entry.nativeLease.claimed &&
                                     entry.nativeLease.released);
    }

    /** @copydoc RenderResourceRegistry::DescribeExternalTexture */
    Result<RenderExternalTextureFacts> RenderResourceRegistry::DescribeExternalTexture(const RenderResourceIdentity identity) const {
        const auto validated = slots_.Validate(RenderResourceClass::Texture, identity);
        if (validated.HasError())
            return Result<RenderExternalTextureFacts>::Failure(validated.ErrorValue());
        const auto &entry = slots_.At(validated.Value());
        if (entry.state != RenderResourceState::Ready || !entry.externalSource)
            return Result<RenderExternalTextureFacts>::Failure(MakeError(FrontendErrors::ResourceNotReady));
        if (const auto live = entry.externalSource->Validate(); live.HasError())
            return Result<RenderExternalTextureFacts>::Failure(live.ErrorValue());
        return Result<RenderExternalTextureFacts>::Success({entry.externalSource->Descriptor(), entry.externalSource->AcquiredState(),
                                                            entry.externalSource->RequiredFinalState(),
                                                            entry.externalSource->DeviceIncarnation()});
    }
}  // namespace Horo::Render::Detail
