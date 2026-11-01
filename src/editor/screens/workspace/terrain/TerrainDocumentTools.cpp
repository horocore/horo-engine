#include "TerrainDocumentHost.h"
#include "TerrainDocumentHostInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Editor {
    /** @copydoc TerrainDocumentHost::Paint */
    Result<TerrainEditChange> TerrainDocumentHost::Paint(const TerrainPaintFence &fence, TerrainEditOperationId operation,
                                                         const TerrainPaintBrush &brush, std::span<const TerrainSculptStamp> stamps,
                                                         const ProjectMutationCoordinator &mutations,
                                                         const CancellationToken &cancellation) {
        using Return = Result<TerrainEditChange>;
        if (auto admitted = Admit(fence.document); admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        if (!containerSource_)
            return Return::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
        const TerrainHostBusy busy(busy_);
        try {
            auto lease = mutations.TryAcquire({root_, ProjectMutationOwner::Save, "terrain.paint"});
            if (lease.HasError())
                return Return::Failure(std::move(lease).ErrorValue());
            if (auto valid = ValidateResources(); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            auto produced = ProduceTerrainPaint(document_.Source(), layers_, fence, operation, brush, stamps, {}, cancellation);
            if (produced.HasError())
                return Return::Failure(std::move(produced).ErrorValue());
            return document_.Execute(produced.Value(), cancellation);
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        } catch (...) {
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }

    /** @copydoc TerrainDocumentHost::Execute */
    Result<TerrainEditChange> TerrainDocumentHost::Execute(const TerrainEditFence &fence, const TerrainEditOperation &operation,
                                                           const CancellationToken &cancellation) {
        using Return = Result<TerrainEditChange>;
        if (auto admitted = Admit(fence); admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        if (!containerSource_ || !operation.patches.empty() || std::ranges::any_of(operation.placements, [](const auto &edit) {
            return edit.value.has_value();
        }))
            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
        const TerrainHostBusy busy(busy_);
        return document_.Execute(operation, cancellation);
    }

    /** @copydoc TerrainDocumentHost::ExecuteFoliage */
    Result<TerrainEditChange> TerrainDocumentHost::ExecuteFoliage(const TerrainEditFence &fence, const TerrainEditOperation &operation,
                                                                  const Terrain::FoliageAuthoringDefinitionReference &definition,
                                                                  const ProjectMutationCoordinator &mutations,
                                                                  const CancellationToken &cancellation) {
        using Return = Result<TerrainEditChange>;
        if (auto admitted = Admit(fence); admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        if (!containerSource_ || !operation.patches.empty())
            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
        if (std::ranges::any_of(operation.placements, [&](const auto &edit) {
            return edit.value && edit.value->type != definition.type;
        }))
            return Return::Failure(MakeError(TerrainEditErrors::Invalid));
        const TerrainHostBusy busy(busy_);
        try {
            auto lease = mutations.TryAcquire({root_, ProjectMutationOwner::Save, "terrain.foliage.paint"});
            if (lease.HasError())
                return Return::Failure(std::move(lease).ErrorValue());
            if (auto valid = ValidateDefinition(definition); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            return document_.Execute(operation, cancellation);
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        } catch (...) {
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }
}  // namespace Horo::Editor
