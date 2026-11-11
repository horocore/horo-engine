#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabSourceExpansionInternal.h"

#include <algorithm>

namespace Horo::Prefab::Detail {
    namespace {
        /** @brief No-materialization traversal scratch charged to the resolver operation. */
        struct Inspection final {
            SourceExpansionState &operation;
            std::size_t expandedObjects{};
        };

        /** @brief Retires only the current traversal stack entry while preserving a separately owned failure path. */
        struct PopActive final {
            std::vector<Assets::AssetId> &active;

            explicit PopActive(std::vector<Assets::AssetId> &value) noexcept : active(value) {}

            PopActive(const PopActive &) = delete;
            PopActive &operator=(const PopActive &) = delete;

            ~PopActive() {
                active.pop_back();
            }
        };

        /** @brief Retires only the current exact semantic edge after a recursive visit. */
        struct PopEdge final {
            std::vector<PrefabCompositionOriginStep> &edges;

            explicit PopEdge(std::vector<PrefabCompositionOriginStep> &value) noexcept : edges(value) {}

            PopEdge(const PopEdge &) = delete;
            PopEdge &operator=(const PopEdge &) = delete;

            ~PopEdge() {
                edges.pop_back();
            }
        };

        /** @brief Captures exact canonical graph failure context without publishing any source hierarchy. */
        Result<void> Fail(Inspection &inspection, const ErrorCodeDescriptor &error, std::optional<Assets::AssetId> next = {}) {
            inspection.operation.failureChain = inspection.operation.active;
            if (next)
                inspection.operation.failureChain.push_back(*next);
            auto failure = MakeError(error);
            for (const auto &edge : inspection.operation.originChain) {
                failure.message += edge.kind == PrefabCompositionOrigin::Variant ? " VariantParent " : " NestedPlacement ";
                failure.message += edge.owner.ToString() + " -> " + edge.source.ToString();
                if (edge.placement)
                    failure.message += " placement=" + std::to_string(edge.placement->value);
            }
            return Result<void>::Failure(std::move(failure));
        }

        /** @brief Looks up only the sorted immutable source inventory admitted by the existing graph builder. */
        const PrefabDependencySource *Find(const Inspection &inspection, Assets::AssetId asset) {
            const auto sources = inspection.operation.sources;
            const auto found = std::ranges::lower_bound(sources, asset, {}, [](const auto &source) {
                return source.document.Data().assetId;
            });
            return found != sources.end() && found->document.Data().assetId == asset ? &*found : nullptr;
        }

        Result<void> Visit(Inspection &inspection, const PrefabDependencySource &source, std::size_t nestedEdges, std::size_t variantEdges);

        /** @brief Checks the next variant edge before recursion; a root itself consumes no edge allowance. */
        Result<void> VisitVariant(Inspection &inspection, const PrefabComposition &composition, std::size_t nestedEdges,
                                  std::size_t variantEdges) {
            if (variantEdges >= inspection.operation.limits.Policy().maximumVariantInheritanceDepth)
                return Fail(inspection, PrefabErrors::HierarchyDepthExceeded);
            const auto asset = composition.variantParent->Asset();
            const auto *parent = Find(inspection, asset);
            if (!parent)
                return Fail(inspection, PrefabErrors::DependencyUnavailable, asset);
            auto &path = inspection.operation.originChain;
            path.push_back({PrefabCompositionOrigin::Variant, inspection.operation.active.back(), asset, {}});
            const PopEdge pop{path};
            return Visit(inspection, *parent, nestedEdges, variantEdges + 1);
        }

        /** @brief Charges every repeated occurrence's actual local objects before an expanded object vector is allocated. */
        Result<void> CountObjects(Inspection &inspection, std::size_t count) {
            if (count > inspection.operation.limits.Policy().maximumObjectCount - inspection.expandedObjects)
                return Fail(inspection, PrefabErrors::ObjectCountExceeded);
            if (auto charged = inspection.operation.budget.Consume(count); charged.HasError())
                return charged;
            inspection.expandedObjects += count;
            return Result<void>::Success();
        }

        /** @brief Inspects one concrete source's canonically ordered placements, using distinct nested and variant edge counts. */
        Result<void> VisitConcrete(Inspection &inspection, const PrefabDocumentData &document, std::size_t nestedEdges,
                                   std::size_t variantEdges) {
            if (auto counted = CountObjects(inspection, document.objects.size()); counted.HasError())
                return counted;
            if (!document.composition)
                return Result<void>::Success();
            for (const auto &placement : document.composition->nestedPlacements) {
                if (nestedEdges >= inspection.operation.limits.Policy().maximumNestedPrefabDepth)
                    return Fail(inspection, PrefabErrors::HierarchyDepthExceeded);
                const auto asset = placement.sourcePrefab.Asset();
                const auto *nested = Find(inspection, asset);
                if (!nested)
                    return Fail(inspection, PrefabErrors::DependencyUnavailable, asset);
                auto &path = inspection.operation.originChain;
                path.emplace_back(PrefabCompositionOrigin::Nested, document.assetId, asset, placement.placementLocalId);
                const PopEdge pop{path};
                if (auto visited = Visit(inspection, *nested, nestedEdges + 1, variantEdges); visited.HasError())
                    return visited;
            }
            return Result<void>::Success();
        }

        /** @brief Charges all layer records before their schema-independent validation can inspect payloads. */
        Result<void> ChargeLayerRecords(Inspection &inspection, const std::optional<PrefabValueOverrideSet> &layer) {
            if (!layer)
                return Result<void>::Success();
            for (const auto count : {layer->records.size(), layer->conflicts.size(), layer->orphans.size(), layer->componentRecords.size(),
                                     layer->componentConflicts.size(), layer->componentOrphans.size()})
                if (auto charged = inspection.operation.budget.Consume(count); charged.HasError())
                    return charged;
            return Result<void>::Success();
        }

        /** @brief Revalidates every source and all its layer bounds under the current operation profile before materialization. */
        Result<void> AdmitSource(Inspection &inspection, const PrefabDependencySource &source) {
            const auto &data = source.document.Data();
            if (data.composition) {
                if (auto charged = ChargeLayerRecords(inspection, data.composition->variantOverrides); charged.HasError())
                    return charged;
                for (const auto &placement : data.composition->nestedPlacements)
                    if (auto charged = ChargeLayerRecords(inspection, placement.valueOverrides); charged.HasError())
                        return charged;
            }
            return source.document.ValidateLimits(inspection.operation.limits);
        }

        /** @brief Validates combined-edge cycles and whole-operation cancellation/work before descending or materializing data. */
        Result<void> Visit(Inspection &inspection, const PrefabDependencySource &source, std::size_t nestedEdges,
                           std::size_t variantEdges) {
            auto &operation = inspection.operation;
            const auto asset = source.document.Data().assetId;
            if (operation.cancellation.IsCancellationRequested())
                return Fail(inspection, PrefabErrors::Cancelled);
            if (std::ranges::find(operation.active, asset) != operation.active.end())
                return Fail(inspection, PrefabErrors::DependencyGraphInvalid, asset);
            if (auto charged = operation.budget.Consume(1); charged.HasError())
                return charged;
            if (auto admitted = AdmitSource(inspection, source); admitted.HasError())
                return admitted;
            operation.active.push_back(asset);
            const PopActive pop{operation.active};
            const auto &data = source.document.Data();
            if (data.composition && data.composition->variantParent)
                return VisitVariant(inspection, *data.composition, nestedEdges, variantEdges);
            return VisitConcrete(inspection, data, nestedEdges, variantEdges);
        }
    }  // namespace

    /** @copydoc PreflightSourceExpansion */
    Result<void> PreflightSourceExpansion(SourceExpansionState &state, const PrefabDependencySource &root) {
        Inspection inspection{state};
        return Visit(inspection, root, 0, 0);
    }
}  // namespace Horo::Prefab::Detail
