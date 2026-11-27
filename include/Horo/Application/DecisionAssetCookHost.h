#pragma once
/** @file DecisionAssetCookHost.h
 * @brief Real captured-source decision cooking through the existing asset transaction owner.
 */
#include "Horo/AI/DecisionGraphCompiler.h"
#include "Horo/AI/LocalLearnedPolicy.h"
#include "Horo/Assets/AssetCookService.h"

namespace Horo::AI {
    class QuerySchemaRegistry;
}

namespace Horo::Application {
    namespace DecisionCookErrors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor Stale;
        extern const ErrorCodeDescriptor Cancelled;
    }  // namespace DecisionCookErrors

    inline constexpr std::string_view DecisionBehaviorTreeSourceType = AI::DecisionAssetType(AI::DecisionPlanKind::BehaviorTree);
    inline constexpr std::string_view DecisionStateMachineSourceType = AI::DecisionAssetType(AI::DecisionPlanKind::StateMachine);
    inline constexpr std::string_view DecisionUtilitySourceType = AI::DecisionAssetType(AI::DecisionPlanKind::Utility);

    /** @brief Decodes one captured registered graph using the cooker's canonical family and project-path checks. */
    [[nodiscard]] Result<AI::DecisionGraphSource> DecodeRegisteredDecisionGraphSource(const Assets::AssetCookPinnedSource &file,
                                                                                      AI::DecisionCompilationLimits limits = {});

    /** @brief Captured model capabilities and task declarations for one asset package target. */
    struct LocalPolicyAssetCookAdmission final {
        AssetCookTargetId packageTarget;
        AI::LocalPolicyCookTarget target;
        AI::AiCapabilityDescriptorSet tasks;
        std::optional<AI::BlackboardSchemaId> blackboard;
    };

    /** @brief Creates the target-scoped resource cooker used by the Scene and release package hosts. */
    [[nodiscard]] Result<Assets::CookerContribution> MakeLocalPolicyAssetCooker(const LocalPolicyAssetCookAdmission &admission);

    struct DecisionAssetCookRequest final {
        Assets::AssetCookRequest assets; /**< Actual pinned registry/files, project lease/fence and durable publication authority. */
        AI::DecisionCompilationEnvironment environment;
        AI::DecisionCompilationLimits limits;
        std::vector<Assets::CookerContribution>
            additionalCookers; /**< Existing host strategies for non-decision records, not an alternate registry. */
        std::uint64_t maximumPreparedBytes{64ULL * 1024ULL * 1024ULL};
        std::shared_ptr<const AI::QuerySchemaRegistry> querySchema; /**< Explicit target descriptors for built-in query cooking. */
        std::optional<LocalPolicyAssetCookAdmission> localPolicy;
    };

    /** @brief Domain adapter; AssetCookService remains the sole generation/cache/publication owner.
     * @details Every decision source is a canonical one-graph source envelope read from the actual immutable file
     * capture. Graph IDs are decoded, never fabricated from AssetId. Real source paths supply honest coarse
     * diagnostic provenance. Required assets.validateHostInputs retains the host's schema/settings/package/read
     * authority through joined publication; this adapter additionally rechecks files and actual generation admission.
     * Caller keyProjection is rejected: this producer retains complete whole-capture identity and binds exact
     * prepared output identities, including diagnostic paths. Other cookers retain their ordinary complete keys.
     */
    class DecisionAssetCookHost final {
    public:
        [[nodiscard]] static Result<Assets::AssetCookReport> Cook(JobSystem &jobs, const DecisionAssetCookRequest &request,
                                                                  const CancellationToken &cancellation = {});
    };
}  // namespace Horo::Application
