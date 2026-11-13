#pragma once
/** @file DecisionGraphCompiler.h
 * @brief Complete typed graph compilation, executable closure and checked storage declarations.
 */
#include "Horo/AI/BehaviorTree.h"
#include "Horo/AI/DecisionControl.h"
#include "Horo/AI/DecisionNodeRegistration.h"
#include "Horo/AI/StateMachine.h"
#include "Horo/AI/UtilityDecision.h"
#include "Horo/Foundation/Sha256.h"

#include <variant>

namespace Horo::AI {
    /** @brief Captured source header plus the canonical family semantic model.
     * BehaviorTreeNode will own its typed control and optional call directly; StateMachineState will own its call/terminal.
     * This wrapper does not keep a second node table or a project-maintained operation registry.
     */
    struct DecisionGraphSource final {
        DecisionGraphAssetId asset;
        std::uint32_t schemaVersion{1};
        BlackboardSchemaId blackboard;
        DecisionVersionRange blackboardVersion;
        SourceLocation location;
        TaskId executionTask; /**< Persistent HSM runner task definition; BT and Utility actions own authored task definitions. */
        std::variant<std::shared_ptr<const BehaviorTreeAsset>, StateMachineAssetDescriptor, UtilityDecisionSource> body;
    };

    struct DecisionCompilationLimits final {
        std::size_t maximumAssets{128};
        std::size_t maximumNodes{4096};
        std::size_t maximumCallDepth{16};
        std::size_t maximumOccurrences{256};
        std::size_t maximumExecutionDepth{16U * 1024U};
        std::uint64_t maximumTaskSlots{1U << 20U};
        std::uint64_t maximumCoreBytes{64ULL * 1024ULL * 1024ULL};
        std::uint64_t maximumProviderBytes{256ULL * 1024ULL * 1024ULL};
        DecisionMemoryPolicy memoryPolicy{DecisionMemoryPolicy::CoreBound};
    };

    /** @brief Host-captured actual providers, schemas and target grants; compilation never invokes provider factories. */
    struct DecisionCompilationEnvironment final {
        std::shared_ptr<const DecisionNodeGeneration> nodes;
        std::vector<std::shared_ptr<const BlackboardSchema>> schemas;
        AiCapabilitySet capabilities;
        std::uint32_t runtimeLayoutVersion{1};
    };

    /** @brief Core counters/bounds; provider declarations remain labelled by their actual guarantee. */
    struct DecisionStorageRequirements final {
        std::uint64_t compiledBytes{};
        std::uint64_t coreInstanceBytes{};
        std::uint64_t scratchBytes{};
        std::uint64_t nativeDeclaredBytes{};
        std::uint64_t enforcedVmBytes{};
        std::size_t unknownNativeProviders{};
        std::size_t nativeDeclaredProviders{};
        std::size_t occurrences{};
        std::size_t maximumCallDepth{};
        std::size_t maximumExecutionDepth{};
        std::uint64_t taskSlots{};
    };

    /** @brief One resolved call in a particular plan; family and versions were validated before this index existed. */
    struct DecisionCallSite final {
        DecisionNodeId node;
        std::size_t calleePlan{};
    };

    /** @brief One independent activation allocation, including repeated and parallel references to the same plan. */
    struct DecisionCallOccurrence final {
        std::size_t plan{};
        std::size_t parent{};
        DecisionNodeId callNode;
        std::size_t subtreeEnd{};      /**< Exclusive preorder bound for iterative reset/cancel without recursive destruction. */
        std::uint64_t firstTaskSlot{}; /**< Relative owner-reserved range; no task identity is synthesized. */
    };

    struct DecisionCompiledGraph final {
        DecisionGraphAssetId asset;
        std::uint32_t schemaVersion{};
        TaskId executionTask;
        std::shared_ptr<const DecisionAssetPlan> bindings;
        std::variant<std::shared_ptr<const BehaviorTreeExecutionPlan>, std::shared_ptr<const CookedStateMachinePlan>,
                     std::shared_ptr<const CookedUtilityPlan>>
            execution;
        std::vector<DecisionNodeConfiguration> providerProperties;
        std::vector<DecisionCallSite> calls;
        DecisionStorageRequirements localStorage;
    };

    /** @brief Immutable admitted executable closure; only compiler/codec can construct it. */
    class CookedDecisionPlan final {
    public:
        [[nodiscard]] std::span<const DecisionCompiledGraph> Graphs() const noexcept;
        [[nodiscard]] std::span<const DecisionCallOccurrence> Occurrences() const noexcept;
        [[nodiscard]] const DecisionStorageRequirements &Storage() const noexcept;
        [[nodiscard]] const Sha256Digest &Digest() const noexcept;
        [[nodiscard]] DecisionGraphAssetId Root() const noexcept;
        [[nodiscard]] AiCapabilitySet Capabilities() const noexcept;
        [[nodiscard]] bool IsCurrent() const noexcept;

    private:
        CookedDecisionPlan() = default;
        friend class DecisionGraphCompiler;
        friend class DecisionPlanCodec;
        friend class DecisionGraphInstance;
        std::vector<DecisionCompiledGraph> graphs_;
        std::vector<DecisionCallOccurrence> occurrences_;
        DecisionStorageRequirements storage_;
        Sha256Digest digest_;
        DecisionGraphAssetId root_;
        std::vector<std::uint8_t> sourceBytes_;
        Sha256Digest environmentIdentity_;
        std::uint32_t runtimeLayoutVersion_{};
        AiCapabilitySet capabilities_;
        std::shared_ptr<const DecisionNodeGeneration> providers_;
    };

    struct DecisionGraphCompilation final {
        DecisionAssetValidationReport validation;
        std::shared_ptr<const CookedDecisionPlan> plan;
    };

    class DecisionGraphCompiler final {
    public:
        [[nodiscard]] static Result<DecisionGraphCompilation> Compile(DecisionGraphAssetId root,
                                                                      std::span<const DecisionGraphSource> sources,
                                                                      const DecisionCompilationEnvironment &environment,
                                                                      DecisionCompilationLimits limits = {});
    };

    /** @brief Owner-lane last-valid publication; no disk write, world mutation or state migration is implied. */
    class DecisionPlanStore final {
    public:
        [[nodiscard]] Result<bool> TryActivate(DecisionGraphCompilation candidate);
        [[nodiscard]] std::shared_ptr<const CookedDecisionPlan> Current() const noexcept;

    private:
        std::shared_ptr<const CookedDecisionPlan> current_;
        std::thread::id owner_{std::this_thread::get_id()};
    };
}  // namespace Horo::AI
