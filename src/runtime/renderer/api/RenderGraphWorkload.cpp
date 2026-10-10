#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>

namespace Horo::Render {
    namespace {
        /** @brief Bounded native identity and use interval evidence for independent alias validation. */
        struct AliasUse {
            std::uint64_t instance;
            std::size_t first;
            std::size_t last;
            RenderGraphResourceKind kind;
            RenderGraphResourceClass resourceClass;
        };

        /** @brief Matches exactly four distinct whole-buffer storage uses and finite dispatch bounds. */
        [[nodiscard]] bool MatchesLightCulling(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                               const RenderGraphLightCulling &culling) {
            const auto &dispatch = culling.dispatch;
            const LightCullingBudget budget{.maximumLights = std::max(dispatch.referencesPerCluster, std::max(1U, dispatch.lightCount)),
                                            .maximumClusters = dispatch.clusterCount,
                                            .referencesPerCluster = dispatch.referencesPerCluster};
            const auto uses = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
            if (pass.kind != RenderPassKind::Compute || !culling.kernel || culling.tableRevision == 0 || !budget.IsValid() ||
                dispatch.reserved != 0 || uses.size() != 4)
                return false;
            const std::array ids{culling.lights, culling.clusters, culling.membership, culling.references};
            for (std::size_t index = 0; index < ids.size(); ++index) {
                if (ids[index].owner != request.graph.Owner() || ids[index].value == 0 ||
                    ids[index].value > request.graph.Resources().size() ||
                    request.graph.Resources()[ids[index].value - 1].kind != RenderGraphResourceKind::Buffer)
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (ids[previous] == ids[index])
                        return false;
                const auto found = std::ranges::find(uses, ids[index], &RenderGraphResourceUsage::resource);
                if (found == uses.end() || found->kind != RenderGraphUsageKind::Storage ||
                    found->access != (index < 2 ? RenderGraphAccess::Read : RenderGraphAccess::Write))
                    return false;
            }
            return true;
        }

        /** @brief Requires each operation to account for exactly its retained semantic uses. */
        [[nodiscard]] bool MatchesWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                           const RenderGraphWorkload &workload) {
            using enum RenderGraphAccess;
            const auto uses = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
            if (const auto *culling = std::get_if<RenderGraphLightCulling>(&workload))
                return MatchesLightCulling(request, pass, *culling);
            if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload)) {
                return pass.kind == RenderPassKind::Graphics && color->operations.IsValid() &&
                       color->operations.loadOperation != AttachmentLoadOperation::DontCare &&
                       color->operations.storeOperation == AttachmentStoreOperation::Store && uses.size() == 1 &&
                       uses[0].resource == color->texture && uses[0].kind == RenderGraphUsageKind::ColorAttachment &&
                       uses[0].access != Read &&
                       (color->operations.loadOperation != AttachmentLoadOperation::Load || uses[0].access == ReadWrite);
            }
            if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload)) {
                if (pass.kind != RenderPassKind::Copy || uses.size() != 2 || copy->byteCount == 0 || copy->source == copy->destination)
                    return false;
                bool source = false;
                bool destination = false;
                for (const auto &use : uses) {
                    source |= use.resource == copy->source && use.kind == RenderGraphUsageKind::CopySource && use.access == Read;
                    destination |=
                        use.resource == copy->destination && use.kind == RenderGraphUsageKind::CopyDestination && use.access == Write;
                }
                return source && destination;
            }
            if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&workload))
                return pass.kind == RenderPassKind::Graphics && uses.empty() && primary->IsValid();
            return pass.kind == RenderPassKind::Graphics && uses.empty();
        }

        /** @brief Validates exact pass semantics and records retained first/last uses without allocation. */
        [[nodiscard]] bool RecordUses(const RenderGraphExecutionRequest &request, const std::span<AliasUse> aliases) {
            const auto &graph = request.graph;
            for (std::size_t index = 0; index < graph.Passes().size(); ++index) {
                const auto &pass = graph.Passes()[index];
                if (!pass.usages.IsValidFor(graph.Usages().size()) || !pass.queue.IsValid() || pass.queue != graph.Passes().front().queue ||
                    request.workloads[index].pass != pass.pass || !MatchesWorkload(request, pass, request.workloads[index].workload))
                    return false;
                for (const auto &use : graph.Usages().subspan(pass.usages.offset, pass.usages.count)) {
                    if (use.resource.owner != graph.Owner() || use.resource.value == 0 || use.resource.value > graph.Resources().size())
                        return false;
                    auto &alias = aliases[use.resource.value - 1U];
                    alias.first = std::min(alias.first, index);
                    alias.last = index;
                }
            }
            return true;
        }

        /** @brief Rejects overlapping transient native objects and reuse of imported physical storage. */
        [[nodiscard]] bool CompatibleAliases(const std::span<AliasUse> aliases) {
            std::sort(aliases.begin(), aliases.end(), [](const AliasUse &left, const AliasUse &right) {
                return std::tie(left.kind, left.instance, left.first) < std::tie(right.kind, right.instance, right.first);
            });
            for (std::size_t index = 1; index < aliases.size(); ++index) {
                const auto &previous = aliases[index - 1];
                const auto &current = aliases[index];
                if (previous.kind != current.kind || previous.instance != current.instance)
                    continue;
                const bool previousTransient = previous.resourceClass == RenderGraphResourceClass::Transient;
                const bool currentTransient = current.resourceClass == RenderGraphResourceClass::Transient;
                if ((previousTransient || currentTransient) && (!previousTransient || !currentTransient || previous.last >= current.first))
                    return false;
            }
            return true;
        }

        /** @brief Checks native binding coverage and independently validates the actual reused object intervals. */
        [[nodiscard]] Result<void> ValidateInstances(const RenderGraphExecutionRequest &request, const std::span<AliasUse> aliases) {
            const auto &graph = request.graph;
            std::size_t usedCount{};
            bool hasTransient{};
            for (std::size_t index = 0; index < graph.Resources().size(); ++index) {
                const auto &resource = graph.Resources()[index];
                if (request.resources[index].resource != resource.id)
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
                if (resource.resourceClass == RenderGraphResourceClass::Transient) {
                    if (!request.transientResourcesAdmitted)
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
                    if (aliases[index].first == std::numeric_limits<std::size_t>::max())
                        continue;
                    hasTransient = true;
                }
                if (request.resources[index].instance == 0)
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
                if (aliases[index].first != std::numeric_limits<std::size_t>::max()) {
                    auto &alias = aliases[index];
                    alias.instance = request.resources[index].instance;
                    alias.kind = resource.kind;
                    alias.resourceClass = resource.resourceClass;
                    aliases[usedCount++] = alias;
                }
            }
            if (hasTransient && !CompatibleAliases(aliases.first(usedCount)))
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateRenderGraphExecutionRequest */
    Result<void> ValidateRenderGraphExecutionRequest(const RenderGraphExecutionRequest &request) {
        const auto &graph = request.graph;
        if (!graph.Owner().IsValid() || graph.Resources().size() > RenderGraphLimits::HardMaxResources ||
            graph.Passes().size() > RenderGraphLimits::HardMaxPasses || request.workloads.size() != graph.Passes().size() ||
            request.resources.size() != graph.Resources().size())
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (!graph.ReleaseTransfers().empty() || !graph.AcquireTransfers().empty())
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload,
                                                   "Single-queue workload admission does not implement ownership transfers."));
        std::array<AliasUse, RenderGraphLimits::HardMaxResources> storage;
        const auto aliases = std::span{storage}.first(graph.Resources().size());
        std::fill(aliases.begin(), aliases.end(),
                  AliasUse{0, std::numeric_limits<std::size_t>::max(), 0, RenderGraphResourceKind::Buffer,
                           RenderGraphResourceClass::Persistent});
        if (!RecordUses(request, aliases))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        return ValidateInstances(request, aliases);
    }
}  // namespace Horo::Render
