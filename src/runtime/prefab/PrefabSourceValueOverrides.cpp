#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabValueOverrides.h"

#include <new>

namespace Horo::Prefab {
    /** @brief Source adapter retains original graph metadata without giving the
     * runtime delta core a source loader. */
    class PrefabValueOverrideResolver final {
    public:
        /** @brief Adopts one complete resolved object set after the canonical core
         * succeeds. */
        static EffectivePrefabCandidate Publish(const EffectivePrefabCandidate &source, std::vector<ResolvedPrefabObject> objects,
                                                const Sha256Digest &digest) {
            return EffectivePrefabCandidate{source.RootAsset(), source.Revision(), std::move(objects), digest, source.Metrics()};
        }
    };

    namespace {
        /** @brief Borrows only actual immutable values; graph and document ownership
         * stay with source preparation. */
        PrefabOverrideBaseline Baseline(const EffectivePrefabCandidate &source) noexcept {
            return {source.RootAsset(), source.Revision().rootSource, source.Objects(), source.ValueOverridesDigest(), source.Metrics()};
        }

        /** @brief Reattaches original source context only to a complete canonical
         * result. */
        Result<PrefabValueOverrideResolution> Adopt(const EffectivePrefabCandidate &source, Result<PrefabOverrideResolution> result) {
            if (result.HasError())
                return Result<PrefabValueOverrideResolution>::Failure(result.ErrorValue());
            try {
                auto value = std::move(result).Value();
                std::optional<EffectivePrefabCandidate> effective;
                if (value.effective)
                    effective = PrefabValueOverrideResolver::Publish(source, std::move(*value.effective), value.appliedDigest);
                return Result<PrefabValueOverrideResolution>::Success(
                    {std::move(effective), std::move(value.rebased), std::move(value.provenance), value.redundantRecords});
            } catch (const std::bad_alloc &) {
                return Result<PrefabValueOverrideResolution>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
            }
        }
    }  // namespace

    /** @copydoc ResolvePrefabValueOverrides */
    Result<PrefabValueOverrideResolution> ResolvePrefabValueOverrides(const EffectivePrefabCandidate &source,
                                                                      const PrefabValueOverrideSet &set,
                                                                      const PrefabValueSchemaSnapshot *schemas,
                                                                      const PrefabLimitProfile &limits,
                                                                      const CancellationToken &cancellation) {
        return Adopt(source, ResolvePrefabValueOverrides(Baseline(source), set, schemas, limits, cancellation));
    }

    /** @copydoc PreparePrefabValueEdit */
    Result<PrefabValueOverrideResolution> PreparePrefabValueEdit(const EffectivePrefabCandidate &source,
                                                                 const std::optional<PrefabValueOverrideSet> &current,
                                                                 const PrefabValueEdit &edit, const PrefabValueSchemaSnapshot *schemas,
                                                                 const PrefabLimitProfile &limits, const CancellationToken &cancellation) {
        return Adopt(source, PreparePrefabValueEdit(Baseline(source), current, edit, schemas, limits, cancellation));
    }

    /** @copydoc PreparePrefabComponentEdit */
    Result<PrefabValueOverrideResolution> PreparePrefabComponentEdit(
        const EffectivePrefabCandidate &source, const std::optional<PrefabValueOverrideSet> &current, const PrefabComponentEdit &edit,
        const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits, const CancellationToken &cancellation) {
        return Adopt(source, PreparePrefabComponentEdit(Baseline(source), current, edit, schemas, limits, cancellation));
    }
}  // namespace Horo::Prefab
