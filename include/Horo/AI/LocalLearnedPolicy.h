#pragma once

/** @file LocalLearnedPolicy.h @brief Optional bounded integer inference without simulation authority. */

#include "Horo/AI/AICapabilityDescriptors.h"
#include "Horo/AI/DecisionIdentity.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Sha256.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::AI {
    class AiWorkBudget;

    inline constexpr std::string_view LocalPolicyAssetType{"core.local_learned_policy"};

    using LocalPolicyModelId = AiStableIdentity<struct LocalPolicyModelIdentityTag>;
    using LocalPolicyFeatureId = AiStableIdentity<struct LocalPolicyFeatureIdentityTag>;
    using LocalPolicyTargetId = AiStableIdentity<struct LocalPolicyTargetIdentityTag>;

    struct LocalPolicyFeature final {
        LocalPolicyFeatureId id;
        std::int16_t value{};
    };

    /** @brief One signed integer dot product with an ordered row of feature weights. */
    struct LocalPolicyRowSource final {
        TaskId task;
        std::int32_t bias{};
        std::span<const std::int16_t> weights;
    };

    /** @brief Authoring input to the opt-in CPU reference model cooker. */
    struct LocalPolicySource final {
        LocalPolicyModelId model;
        std::uint32_t tensorSchemaVersion{};
        std::span<const LocalPolicyTargetId> targets;
        std::span<const LocalPolicyFeatureId> features;
        std::span<const LocalPolicyRowSource> rows;
    };

    struct LocalPolicyCookTarget final {
        LocalPolicyTargetId id;
        bool supportsInt16LinearV1{};
        std::size_t maximumModelBytes{};
        std::size_t maximumWorkUnits{};
    };

    /** @brief Exact simulation-owned request identity. Carries no live capabilities or addresses. */
    struct LocalPolicyRequestIdentity final {
        AgentHandle agent;
        std::uint64_t sceneGeneration{};
        DecisionGraphAssetId plan;
        std::uint64_t providerGeneration{};
        std::uint64_t inputRevision{};
        std::uint64_t requestGeneration{};
        std::uint64_t deadlineTick{};
        bool operator==(const LocalPolicyRequestIdentity &) const noexcept = default;
    };

    enum class LocalPolicyStatus : std::uint8_t {
        Proposed,
        InvalidInput,
        BudgetExhausted,
        Cancelled,
        Expired,
        Stale,
        MissingTask,
        Consumed
    };

    struct LocalPolicyOutcome final {
        LocalPolicyStatus status{LocalPolicyStatus::InvalidInput};
        TaskId task;
    };

    class LocalLearnedPolicy;

    /** @brief One detached result; only the authoritative safe point can extract its task once. */
    class LocalPolicyProposal final {
    public:
        LocalPolicyProposal() = default;
        LocalPolicyProposal(const LocalPolicyProposal &) = delete;
        LocalPolicyProposal &operator=(const LocalPolicyProposal &) = delete;
        LocalPolicyProposal(LocalPolicyProposal &&other) noexcept;
        LocalPolicyProposal &operator=(LocalPolicyProposal &&) = delete;

        [[nodiscard]] LocalPolicyStatus Status() const noexcept {
            return status_;
        }

    private:
        friend class LocalLearnedPolicy;
        friend class LocalPolicyRequestSlot;
        LocalPolicyStatus status_{LocalPolicyStatus::InvalidInput};
        LocalPolicyRequestIdentity identity_;
        Sha256Digest modelDigest_;
        TaskId task_;
    };

    /** @brief One noncopyable reservation created by the decision owner for each submitted request. */
    class LocalPolicyRequestSlot final {
    public:
        explicit LocalPolicyRequestSlot(const LocalPolicyRequestIdentity &identity) noexcept : identity_(identity) {}

        LocalPolicyRequestSlot(const LocalPolicyRequestSlot &) = delete;
        LocalPolicyRequestSlot &operator=(const LocalPolicyRequestSlot &) = delete;

        /** @brief Consumes exactly one matching result at the existing simulation safe point. */
        [[nodiscard]] LocalPolicyOutcome Consume(LocalPolicyProposal &proposal, const LocalPolicyRequestIdentity &current,
                                                 const LocalLearnedPolicy &model, const AiCapabilityDescriptorSet &tasks,
                                                 std::uint64_t tick, const CancellationToken &cancellation = {}) noexcept;

    private:
        LocalPolicyRequestIdentity identity_;
        bool retired_{};
    };

    /** @brief Owned cooked Int16LinearV1 model. Integer arithmetic and stable task ID tie breaks are reproducible. */
    class LocalLearnedPolicy final {
    public:
        static constexpr std::size_t MaximumFeatures = 64;
        static constexpr std::size_t MaximumRows = 16;
        static constexpr std::size_t MaximumTargets = 16;

        /** @brief Validates target, tensor shape, schema, registered task declarations and resource admission. */
        [[nodiscard]] static Result<LocalLearnedPolicy> Cook(const LocalPolicySource &source, const LocalPolicyCookTarget &target,
                                                             const AiCapabilityDescriptorSet &tasks,
                                                             std::optional<BlackboardSchemaId> blackboard = {});

        /** @brief Produces canonical target-specific bytes. The SHA-256 of the bytes is Digest(). */
        [[nodiscard]] Result<std::vector<std::uint8_t>> EncodeCooked() const;
        /** @brief Verifies the expected artifact and repeats target, budget, schema and task admission before loading. */
        [[nodiscard]] static Result<LocalLearnedPolicy> DecodeCooked(std::span<const std::uint8_t> bytes,
                                                                     const Sha256Digest &expectedDigest,
                                                                     const LocalPolicyCookTarget &target,
                                                                     const AiCapabilityDescriptorSet &tasks,
                                                                     std::optional<BlackboardSchemaId> blackboard = {});

        [[nodiscard]] const Sha256Digest &Digest() const noexcept {
            return digest_;
        }

        /** @brief A model without a schema is usable with any explicitly supplied tensor. */
        [[nodiscard]] bool SupportsBlackboard(BlackboardSchemaId schema) const noexcept {
            return !blackboard_ || blackboard_ == schema;
        }

        [[nodiscard]] std::size_t RequiredWork() const noexcept;

        [[nodiscard]] std::span<const LocalPolicyFeatureId> Features() const noexcept {
            return std::span{features_}.first(featureCount_);
        }

        [[nodiscard]] LocalPolicyTargetId Target() const noexcept {
            return target_;
        }

        [[nodiscard]] std::uint32_t TensorSchemaVersion() const noexcept {
            return tensorSchemaVersion_;
        }

        /** @brief Runs the bounded inference against exact typed features. Fall back at the caller on a typed failure. */
        [[nodiscard]] LocalPolicyProposal Infer(const LocalPolicyRequestIdentity &identity, LocalPolicyTargetId target,
                                                const Sha256Digest &expectedModel, std::uint32_t tensorSchemaVersion,
                                                std::span<const LocalPolicyFeature> features, std::uint64_t tick, AiWorkBudget &work,
                                                const CancellationToken &cancellation = {}) const noexcept;

    private:
        friend class LocalPolicyRequestSlot;

        struct Row final {
            TaskId task;
            std::int32_t bias{};
            std::array<std::int16_t, MaximumFeatures> weights{};
        };

        LocalLearnedPolicy() = default;
        template <typename Write> void WriteCanonical(Write write) const;
        [[nodiscard]] Sha256Digest Fingerprint() const noexcept;
        LocalPolicyModelId model_;
        LocalPolicyTargetId target_;
        std::optional<BlackboardSchemaId> blackboard_;
        std::uint32_t tensorSchemaVersion_{};
        std::array<LocalPolicyFeatureId, MaximumFeatures> features_{};
        std::array<Row, MaximumRows> rows_{};
        std::size_t featureCount_{};
        std::size_t rowCount_{};
        Sha256Digest digest_;
    };
}  // namespace Horo::AI
