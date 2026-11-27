#include "Horo/AI/AIErrors.h"
#include "Horo/AI/DecisionAssetValidation.h"

#include <algorithm>
#include <ranges>
#include <tuple>
#include <utility>

namespace Horo::AI {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }
    }  // namespace

    DecisionAssetValidationReport::DecisionAssetValidationReport(std::vector<DecisionAssetValidationDiagnostic> diagnostics) noexcept
        : diagnostics_(std::move(diagnostics)) {}

    /** @copydoc DecisionAssetValidationReport::Diagnostics */
    std::span<const DecisionAssetValidationDiagnostic> DecisionAssetValidationReport::Diagnostics() const noexcept {
        return diagnostics_;
    }

    /** @copydoc DecisionAssetValidationReport::Size */
    std::size_t DecisionAssetValidationReport::Size() const noexcept {
        return diagnostics_.size();
    }

    /** @copydoc DecisionAssetValidationReport::Empty */
    bool DecisionAssetValidationReport::Empty() const noexcept {
        return diagnostics_.empty();
    }

    /** @copydoc DecisionAssetValidationReport::HasErrors */
    bool DecisionAssetValidationReport::HasErrors() const noexcept {
        return std::ranges::any_of(diagnostics_, [](const auto &diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error || diagnostic.severity == DiagnosticSeverity::Fatal;
        });
    }

    DecisionAssetPlan::DecisionAssetPlan(ConstructionData data) noexcept
        : asset_(data.asset), kind_(data.kind), schemaVersion_(data.schemaVersion), blackboardSchema_(std::move(data.blackboardSchema)),
          nodes_(std::move(data.nodes)), bindings_(std::move(data.bindings)), dependencies_(std::move(data.dependencies)) {}

    /** @copydoc DecisionAssetPlan::HasEquivalentBindings */
    bool DecisionAssetPlan::HasEquivalentBindings(const DecisionAssetPlan &candidate) const noexcept {
        if (asset_ != candidate.asset_ || kind_ != candidate.kind_ || schemaVersion_ != candidate.schemaVersion_ ||
            blackboardSchema_ != candidate.blackboardSchema_)
            return false;
        const bool sameNodes = std::ranges::equal(nodes_, candidate.nodes_, [](const auto &a, const auto &b) {
            return std::tie(a.id, a.type, a.origin, a.firstBlackboardBinding, a.blackboardBindingCount, a.intrinsic) ==
                   std::tie(b.id, b.type, b.origin, b.firstBlackboardBinding, b.blackboardBindingCount, b.intrinsic);
        });
        return sameNodes && std::ranges::equal(bindings_, candidate.bindings_, [](const auto &a, const auto &b) {
            return std::tie(a.key, a.schemaIndex, a.kind, a.cardinality, a.access, a.presence, a.defaultValue) ==
                   std::tie(b.key, b.schemaIndex, b.kind, b.cardinality, b.access, b.presence, b.defaultValue);
        });
    }

    /** @copydoc DecisionAssetPlan::Nodes */
    std::span<const DecisionPlanNode> DecisionAssetPlan::Nodes() const noexcept {
        return nodes_;
    }

    /** @copydoc DecisionAssetPlan::Bindings */
    std::span<const DecisionPlanBlackboardBinding> DecisionAssetPlan::Bindings() const noexcept {
        return bindings_;
    }

    /** @copydoc DecisionAssetPlan::Dependencies */
    std::span<const DecisionPlanDependency> DecisionAssetPlan::Dependencies() const noexcept {
        return dependencies_;
    }

    std::uint64_t DecisionAssetPlan::StorageBytes() const noexcept {
        return sizeof(DecisionAssetPlan) + nodes_.capacity() * sizeof(DecisionPlanNode) +
               bindings_.capacity() * sizeof(DecisionPlanBlackboardBinding) + dependencies_.capacity() * sizeof(DecisionPlanDependency);
    }

    /** @copydoc DecisionAssetPlan::BindingsForNode */
    std::span<const DecisionPlanBlackboardBinding> DecisionAssetPlan::BindingsForNode(const DecisionNodeId node) const noexcept {
        const auto found = std::ranges::lower_bound(nodes_, node, {}, &DecisionPlanNode::id);
        if (found == nodes_.end() || found->id != node)
            return {};
        return std::span<const DecisionPlanBlackboardBinding>{bindings_}.subspan(found->firstBlackboardBinding,
                                                                                 found->blackboardBindingCount);
    }

    /** @copydoc DecisionAssetPlanStore::TryActivate */
    Result<DecisionAssetActivationResult> DecisionAssetPlanStore::TryActivate(DecisionAssetCompilation candidate) {
        if (candidate.plan == nullptr && candidate.validation.Empty())
            return Failure<DecisionAssetActivationResult>(AIErrors::DecisionAssetActivationInvalid);
        if (candidate.plan == nullptr || candidate.validation.HasErrors())
            return Result<DecisionAssetActivationResult>::Success(
                {.validation = std::move(candidate.validation), .activePlan = activePlan_, .activated = false});

        activePlan_ = std::move(candidate.plan);
        return Result<DecisionAssetActivationResult>::Success(
            {.validation = std::move(candidate.validation), .activePlan = activePlan_, .activated = true});
    }
}  // namespace Horo::AI
