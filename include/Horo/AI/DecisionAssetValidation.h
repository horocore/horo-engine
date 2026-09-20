#pragma once

/**
 * @file DecisionAssetValidation.h
 * @brief Stable blackboard binding and decision-asset validation contracts.
 */

#include "Horo/AI/BlackboardSchema.h"
#include "Horo/Foundation/Diagnostics.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::AI {
    struct DecisionGraphAssetIdentityTag;
    struct DecisionNodeIdentityTag;
    struct DecisionNodeTypeIdentityTag;
    struct DecisionProviderIdentityTag;

    /** @brief Stable identity of one persisted decision graph asset. */
    using DecisionGraphAssetId = AiStableIdentity<DecisionGraphAssetIdentityTag>;
    /** @brief Stable identity of one persisted decision graph node. */
    using DecisionNodeId = AiStableIdentity<DecisionNodeIdentityTag>;
    /** @brief Stable identity of one native, package, or script node descriptor. */
    using DecisionNodeTypeId = AiStableIdentity<DecisionNodeTypeIdentityTag>;
    /** @brief Stable identity of one descriptor provider. */
    using DecisionProviderId = AiStableIdentity<DecisionProviderIdentityTag>;

    /** @brief Current semantic decision-asset schema version. */
    inline constexpr std::uint32_t CurrentDecisionAssetSchemaVersion = 1;

    /**
     * @brief Inclusive version interval used for explicit compatibility admission.
     * @details A range is part of the authored contract; validators never select a version by display name or load order.
     */
    struct DecisionVersionRange final {
        std::uint32_t minimum{1};
        std::uint32_t maximum{std::numeric_limits<std::uint32_t>::max()};

        /** @brief Checks the reserved zero version and ordering rules. @return Whether the interval is well formed. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return minimum != 0 && maximum != 0 && minimum <= maximum;
        }

        /**
         * @brief Tests one version against this interval.
         * @param version Candidate version.
         * @return Whether the candidate is inside the inclusive interval.
         */
        [[nodiscard]] constexpr bool Contains(const std::uint32_t version) const noexcept {
            return IsValid() && version >= minimum && version <= maximum;
        }

        [[nodiscard]] constexpr auto operator<=>(const DecisionVersionRange &) const noexcept = default;
    };

    /** @brief Runtime plan family expected by one decision asset or dependency. */
    enum class DecisionPlanKind : std::uint8_t {
        BehaviorTree,
        StateMachine,
        Utility,
        Count,
    };

    /** @brief Origin family of a node descriptor contribution. */
    enum class DecisionDescriptorSourceKind : std::uint8_t {
        Native,
        Package,
        Script,
        Count,
    };

    /** @brief Stable provider metadata copied into a compiled plan without exposing provider ABI. */
    struct DecisionDescriptorOrigin final {
        DecisionDescriptorSourceKind kind{DecisionDescriptorSourceKind::Native}; /**< Contribution family. */
        DecisionProviderId provider;                                             /**< Stable provider identity. */
        std::uint32_t version{1};                                                /**< Descriptor contract version. */

        [[nodiscard]] constexpr auto operator<=>(const DecisionDescriptorOrigin &) const noexcept = default;
    };

    /**
     * @brief One typed blackboard contract required by a node descriptor or authored node use.
     * @details The key identity is the only lookup identity. Display labels and serialized array positions are not consulted.
     */
    struct DecisionBlackboardRequirement final {
        BlackboardKeyId key;                                                        /**< Stable key identity. */
        BlackboardValueKind kind{BlackboardValueKind::Boolean};                     /**< Required scalar element kind. */
        BlackboardValueCardinality cardinality{BlackboardValueCardinality::Scalar}; /**< Required cardinality. */
        BlackboardKeyAccess access{BlackboardKeyAccess::ReadOnly};                  /**< Minimum access required by the node. */
        BlackboardKeyPresence presence{BlackboardKeyPresence::Optional};            /**< Whether an instance value must be present. */
        bool requireDefault{}; /**< Whether schema admission must provide a default value. */
        SourceLocation source; /**< Authored source location for diagnostics. */
    };

    /**
     * @brief Descriptor contract supplied by native, package, or script node discovery.
     * @details A descriptor is inert metadata. Registering or validating it never starts provider code.
     */
    struct DecisionNodeDescriptor final {
        DecisionNodeTypeId type;                                 /**< Stable node type identity. */
        DecisionDescriptorOrigin origin;                         /**< Provider and descriptor version. */
        std::vector<DecisionBlackboardRequirement> requirements; /**< Typed keys required by every instance of this node type. */
        SourceLocation source;                                   /**< Descriptor definition location. */
    };

    /** @brief One authored decision node with stable identity and explicit typed bindings. */
    struct DecisionAssetNode final {
        DecisionNodeId id;                                                                    /**< Stable node identity. */
        DecisionNodeTypeId type;                                                              /**< Stable descriptor identity. */
        DecisionVersionRange descriptorVersion{1, std::numeric_limits<std::uint32_t>::max()}; /**< Accepted descriptor versions. */
        std::vector<DecisionBlackboardRequirement> requirements;                              /**< Node-specific typed key uses. */
        SourceLocation source;                                                                /**< Node source location. */
    };

    /** @brief Stable, versioned reference to another decision asset used as a subtree or subplan. */
    struct DecisionSubtreeReference final {
        DecisionGraphAssetId asset;                                    /**< Stable referenced asset identity. */
        DecisionPlanKind expectedKind{DecisionPlanKind::BehaviorTree}; /**< Required referenced plan family. */
        DecisionVersionRange requiredAssetVersion{CurrentDecisionAssetSchemaVersion,
                                                  CurrentDecisionAssetSchemaVersion}; /**< Accepted referenced asset schema versions. */
        DecisionNodeId ownerNode; /**< Optional node owning the reference; invalid means asset-level dependency. */
        SourceLocation source;    /**< Reference source location. */
    };

    /** @brief Borrowed semantic decision-asset source validated before plan publication. */
    struct DecisionAssetDescriptor final {
        DecisionGraphAssetId asset;                                     /**< Stable asset identity. */
        DecisionPlanKind kind{DecisionPlanKind::BehaviorTree};          /**< Persisted graph family. */
        std::uint32_t schemaVersion{CurrentDecisionAssetSchemaVersion}; /**< Asset schema version. */
        BlackboardSchemaId blackboardSchema;                            /**< Stable schema identity required by this asset. */
        DecisionVersionRange requiredBlackboardSchemaVersion{1,
                                                             std::numeric_limits<std::uint32_t>::max()}; /**< Accepted schema versions. */
        std::vector<DecisionAssetNode> nodes;           /**< Semantic nodes; order is not identity. */
        std::vector<DecisionSubtreeReference> subtrees; /**< Stable cross-asset dependencies. */
        SourceLocation source;                          /**< Asset source location. */
    };

    /** @brief Compile-time ceilings that untrusted decision metadata cannot raise. */
    struct DecisionAssetValidationHardLimits final {
        static constexpr std::size_t Assets = 512;
        static constexpr std::size_t NodeDescriptors = 4'096;
        static constexpr std::size_t Schemas = 512;
        static constexpr std::size_t NodesPerAsset = 1'024;
        static constexpr std::size_t SubtreesPerAsset = 512;
        static constexpr std::size_t RequirementsPerNode = 128;
        static constexpr std::size_t TotalRequirements = 8'192;
        static constexpr std::size_t DependencyDepth = 16;
        static constexpr std::size_t Diagnostics = 4'096;
    };

    /** @brief Product-selected bounded validation policy, always capped by DecisionAssetValidationHardLimits. */
    struct DecisionAssetValidationLimits final {
        std::size_t maximumAssets{DecisionAssetValidationHardLimits::Assets};
        std::size_t maximumNodeDescriptors{DecisionAssetValidationHardLimits::NodeDescriptors};
        std::size_t maximumSchemas{DecisionAssetValidationHardLimits::Schemas};
        std::size_t maximumNodesPerAsset{DecisionAssetValidationHardLimits::NodesPerAsset};
        std::size_t maximumSubtreesPerAsset{DecisionAssetValidationHardLimits::SubtreesPerAsset};
        std::size_t maximumRequirementsPerNode{DecisionAssetValidationHardLimits::RequirementsPerNode};
        std::size_t maximumTotalRequirements{DecisionAssetValidationHardLimits::TotalRequirements};
        std::size_t maximumDependencyDepth{DecisionAssetValidationHardLimits::DependencyDepth};
        std::size_t maximumDiagnostics{DecisionAssetValidationHardLimits::Diagnostics};
    };

    /** @brief One source-located finding emitted by decision-asset validation. */
    struct DecisionAssetValidationDiagnostic final {
        ErrorCode code;                                         /**< Stable AIErrors identity. */
        DiagnosticSeverity severity{DiagnosticSeverity::Error}; /**< Presentation severity. */
        std::string message;                                    /**< Finding-specific detail. */
        SourceLocation source;                                  /**< Exact authored source context. */
        std::string path;                                       /**< Machine-readable semantic path, never a runtime lookup key. */
    };

    /** @brief Immutable, deterministically ordered findings from one decision-asset validation pass. */
    class DecisionAssetValidationReport final {
    public:
        DecisionAssetValidationReport() = default;

        /** @brief Returns all owned findings. @return Borrowed view valid for this report's lifetime. */
        [[nodiscard]] std::span<const DecisionAssetValidationDiagnostic> Diagnostics() const noexcept;
        /** @brief Returns the number of unique findings. @return Finding count. */
        [[nodiscard]] std::size_t Size() const noexcept;
        /** @brief Returns whether no findings were emitted. @return True when validation was clean. */
        [[nodiscard]] bool Empty() const noexcept;
        /** @brief Returns whether at least one error or fatal finding exists. @return Activation-blocking state. */
        [[nodiscard]] bool HasErrors() const noexcept;

    private:
        explicit DecisionAssetValidationReport(std::vector<DecisionAssetValidationDiagnostic> diagnostics) noexcept;

        std::vector<DecisionAssetValidationDiagnostic> diagnostics_;

        friend class DecisionAssetCompiler;
    };

    /** @brief One resolved schema key captured into a flat immutable decision plan. */
    struct DecisionPlanBlackboardBinding final {
        BlackboardKeyId key;                                    /**< Stable schema key identity. */
        std::size_t schemaIndex{};                              /**< Precomputed index in the captured schema key array. */
        BlackboardValueKind kind{BlackboardValueKind::Boolean}; /**< Admitted element kind. */
        BlackboardValueCardinality cardinality{BlackboardValueCardinality::Scalar}; /**< Admitted cardinality. */
        BlackboardKeyAccess access{BlackboardKeyAccess::ReadOnly};                  /**< Schema access granted to the plan. */
        BlackboardKeyPresence presence{BlackboardKeyPresence::Optional};            /**< Schema presence policy. */
        std::optional<BlackboardValue> defaultValue; /**< Owned schema default resolved by stable key identity. */
    };

    /** @brief Flat node record with a contiguous range into the plan's blackboard bindings. */
    struct DecisionPlanNode final {
        DecisionNodeId id;                    /**< Stable authored node identity. */
        DecisionNodeTypeId type;              /**< Stable descriptor identity. */
        DecisionDescriptorOrigin origin;      /**< Resolved provider metadata. */
        std::size_t firstBlackboardBinding{}; /**< First binding in DecisionAssetPlan::Bindings(). */
        std::size_t blackboardBindingCount{}; /**< Number of bindings owned by this node. */
    };

    /** @brief Stable dependency-table entry; it contains no runtime pointer. */
    struct DecisionPlanDependency final {
        DecisionGraphAssetId asset;                            /**< Referenced asset identity. */
        DecisionPlanKind kind{DecisionPlanKind::BehaviorTree}; /**< Expected plan family. */
        std::uint32_t schemaVersion{};                         /**< Resolved referenced asset schema version. */
    };

    /**
     * @brief Immutable compiled decision plan with pre-resolved blackboard indices.
     * @details The plan owns the schema snapshot that admitted its bindings, so later registry changes cannot invalidate it.
     */
    class DecisionAssetPlan final {
    public:
        /**
         * @brief Admission token for constructing a plan after validation.
         * @details Only DecisionAssetCompiler can create this token; callers can inspect plans but cannot manufacture one.
         */
        class ConstructionToken final {
        private:
            constexpr ConstructionToken() noexcept = default;

            friend class DecisionAssetCompiler;
        };

        /**
         * @brief Compiler-admitted storage used to construct one immutable plan.
         * @details The private token member makes this aggregate formable only by DecisionAssetCompiler.
         */
        struct ConstructionData final {
            ConstructionToken token;                                              /**< Compiler-only admission capability. */
            DecisionGraphAssetId asset;                                           /**< Stable graph identity. */
            DecisionPlanKind kind{DecisionPlanKind::BehaviorTree};                /**< Plan family. */
            std::uint32_t schemaVersion{};                                        /**< Compiled asset schema version. */
            std::shared_ptr<const ::Horo::AI::BlackboardSchema> blackboardSchema; /**< Admitted schema snapshot. */
            std::vector<DecisionPlanNode> nodes;                                  /**< Nodes sorted by stable identity. */
            std::vector<DecisionPlanBlackboardBinding> bindings;                  /**< Pre-resolved blackboard bindings. */
            std::vector<DecisionPlanDependency> dependencies;                     /**< Resolved subtree dependencies. */
        };

        DecisionAssetPlan() = delete;

        /**
         * @brief Constructs an immutable plan admitted by DecisionAssetCompiler.
         * @param data Compiler-admitted plan storage.
         */
        DecisionAssetPlan(ConstructionData data) noexcept;

        /** @brief Returns the stable graph identity. @return Asset identity. */
        [[nodiscard]] constexpr DecisionGraphAssetId Asset() const noexcept {
            return asset_;
        }

        /** @brief Returns the plan family. @return Decision kind. */
        [[nodiscard]] constexpr DecisionPlanKind Kind() const noexcept {
            return kind_;
        }

        /** @brief Returns the compiled asset schema version. @return Non-zero version. */
        [[nodiscard]] constexpr std::uint32_t SchemaVersion() const noexcept {
            return schemaVersion_;
        }

        /** @brief Returns the exact admitted blackboard schema snapshot. @return Shared immutable schema. */
        [[nodiscard]] std::shared_ptr<const ::Horo::AI::BlackboardSchema> BlackboardSchema() const noexcept {
            return blackboardSchema_;
        }

        /** @brief Returns nodes sorted by stable node identity. @return Borrowed flat node view. */
        [[nodiscard]] std::span<const DecisionPlanNode> Nodes() const noexcept;
        /** @brief Returns all resolved key bindings in node and stable-key order. @return Borrowed flat binding view. */
        [[nodiscard]] std::span<const DecisionPlanBlackboardBinding> Bindings() const noexcept;
        /** @brief Returns stable subplan dependencies. @return Borrowed dependency-table view. */
        [[nodiscard]] std::span<const DecisionPlanDependency> Dependencies() const noexcept;
        /**
         * @brief Returns one node's precomputed binding range.
         * @param node Stable authored node identity.
         * @return Contiguous bindings or an empty view when the node is absent.
         */
        [[nodiscard]] std::span<const DecisionPlanBlackboardBinding> BindingsForNode(DecisionNodeId node) const noexcept;

        DecisionAssetPlan(const DecisionAssetPlan &) = delete;
        DecisionAssetPlan &operator=(const DecisionAssetPlan &) = delete;
        DecisionAssetPlan(DecisionAssetPlan &&) noexcept = default;
        DecisionAssetPlan &operator=(DecisionAssetPlan &&) = delete;

    private:
        DecisionGraphAssetId asset_;
        DecisionPlanKind kind_{};
        std::uint32_t schemaVersion_{};
        std::shared_ptr<const ::Horo::AI::BlackboardSchema> blackboardSchema_;
        std::vector<DecisionPlanNode> nodes_;
        std::vector<DecisionPlanBlackboardBinding> bindings_;
        std::vector<DecisionPlanDependency> dependencies_;

        friend class DecisionAssetCompiler;
    };

    /** @brief Result of one bounded compilation attempt; invalid sources carry source-located findings and no plan. */
    struct DecisionAssetCompilation final {
        DecisionAssetValidationReport validation;
        std::shared_ptr<const DecisionAssetPlan> plan;

        /** @brief Reports whether a plan is safe to publish. @return True only for a clean validation pass. */
        [[nodiscard]] bool IsValid() const noexcept {
            return plan != nullptr && !validation.HasErrors();
        }
    };

    /**
     * @brief Compiles one semantic decision asset against immutable schema and node-descriptor catalogs.
     * @details The catalogs are borrowed synchronously. Missing, ambiguous, and incompatible references become
     * source-located findings; no string or display-name fallback is attempted.
     */
    class DecisionAssetCompiler final {
    public:
        /**
         * @brief Validates and compiles one decision asset.
         * @param asset Root asset source to compile.
         * @param assetCatalog Referenced asset sources; the root may also be present once.
         * @param nodeDescriptors Native, package, and script descriptor candidates.
         * @param schemas Immutable blackboard schema candidates.
         * @param limits Bounded validation and plan-admission policy.
         * @return Compilation findings and an immutable plan when valid, or a typed capacity/storage failure.
         * @post A failed or invalid compilation does not mutate any input or publish a plan.
         */
        [[nodiscard]] static Result<DecisionAssetCompilation> Compile(const DecisionAssetDescriptor &asset,
                                                                      std::span<const DecisionAssetDescriptor> assetCatalog,
                                                                      std::span<const DecisionNodeDescriptor> nodeDescriptors,
                                                                      std::span<const std::shared_ptr<const BlackboardSchema>> schemas,
                                                                      const DecisionAssetValidationLimits &limits = {});
    };

    /** @brief Outcome returned when a candidate compilation is offered to the active-plan slot. */
    struct DecisionAssetActivationResult final {
        DecisionAssetValidationReport validation;
        std::shared_ptr<const DecisionAssetPlan> activePlan; /**< Previous plan on rejection, newly published plan on success. */
        bool activated{};                                    /**< True only when the candidate replaced the active plan. */
    };

    /**
     * @brief Owns the last valid plan and preserves it across invalid recompilations.
     * @details This class is a safe-point publication seam; each offer either publishes a complete plan or leaves the prior plan unchanged.
     */
    class DecisionAssetPlanStore final {
    public:
        DecisionAssetPlanStore() = default;

        /** @brief Returns the currently active immutable plan, if any. @return Shared plan snapshot. */
        [[nodiscard]] std::shared_ptr<const DecisionAssetPlan> ActivePlan() const noexcept {
            return activePlan_;
        }

        /**
         * @brief Offers one compilation for publication.
         * @param candidate Completed compilation from DecisionAssetCompiler::Compile.
         * @return Activation evidence; invalid candidates leave ActivePlan() unchanged.
         * @throws std::bad_alloc only if copying the activation evidence cannot allocate.
         */
        [[nodiscard]] Result<DecisionAssetActivationResult> TryActivate(DecisionAssetCompilation candidate);

        DecisionAssetPlanStore(const DecisionAssetPlanStore &) = delete;
        DecisionAssetPlanStore &operator=(const DecisionAssetPlanStore &) = delete;

    private:
        std::shared_ptr<const DecisionAssetPlan> activePlan_;
    };
}  // namespace Horo::AI
