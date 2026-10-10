#include "UiHotReloadInternal.h"

namespace Horo::Runtime::Ui {
    /** @copydoc UiHotReload::PrepareSceneRebind */
    Result<UiHotReload::Prepared> UiHotReload::PrepareSceneRebind(UiReloadGeneration replacement, const bool removing,
                                                                  const CancellationToken &cancellation) {
        if (!storage_ || storage_->stopped || storage_->collecting || !replacement.storage_)
            return Result<Prepared>::Failure(MakeError(UiErrors::InstanceStateInvalid));
        const auto &source = *storage_->current;
        if (source.Asset() != replacement.Asset() || source.RegistryRevision() != replacement.RegistryRevision() ||
            source.Instance().InstanceId() != replacement.Instance().InstanceId() ||
            source.Instance().DocumentId() != replacement.Instance().DocumentId() ||
            source.Instance().DocumentRevision() != replacement.Instance().DocumentRevision() ||
            source.Canvases().size() != replacement.Canvases().size())
            return Result<Prepared>::Failure(MakeError(UiErrors::RevisionStale));
        for (const auto &old : source.Canvases()) {
            const auto *next = replacement.Canvas(old.id);
            if (!next || old.bindings.has_value() != next->bindings.has_value())
                return Result<Prepared>::Failure(MakeError(UiErrors::BindingDescriptorConflict));
            if (old.bindings) {
                const auto valid = next->bindings->ValidateSceneRebind(*old.bindings, old.tree, next->tree, removing);
                if (valid.HasError())
                    return Result<Prepared>::Failure(valid.ErrorValue());
            }
        }
        auto prepared = PrepareQualified(std::move(replacement), cancellation);
        if (prepared.HasError())
            return prepared;
        auto candidate = std::move(prepared).Value();
        candidate.storage_->sceneRebind = true;
        candidate.storage_->removingScene = removing;
        return Result<Prepared>::Success(std::move(candidate));
    }
}  // namespace Horo::Runtime::Ui
