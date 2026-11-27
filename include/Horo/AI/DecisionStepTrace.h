#pragma once

/**
 * @file DecisionStepTrace.h
 * @brief Redacted, bounded evidence from one decision evaluation.
 */

#include "Horo/AI/AITaskLifecycle.h"
#include "Horo/AI/BlackboardInstance.h"
#include "Horo/AI/DecisionIdentity.h"
#include "Horo/Foundation/Sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Horo::AI {
    /** @brief Evidence types produced by admitted decision kernels. */
    enum class DecisionStepRecordKind : std::uint8_t {
        BehaviorNode,
        StateMachineStep,
        UtilityScore,
        StateMachineTransition, /**< Committed edge identity and ordinal; state/lastTransition are unused. */
    };

    /** @brief One occurrence address in the exact cooked closure identified by the trace digest. */
    struct DecisionTraceOccurrence final {
        DecisionGraphAssetId graph;
        DecisionNodeId caller;
        std::uint32_t occurrence{}; /**< Cooked preorder identity within this exact digest. */
        std::uint32_t parent{};     /**< Root uses its own index. */

        bool operator==(const DecisionTraceOccurrence &) const = default;
    };

    /** @brief Read-only identity of an exact running task in a cooked occurrence. */
    struct DecisionActiveTask final {
        DecisionTraceOccurrence occurrence;
        DecisionNodeId node;
        TaskId task;
        TaskHandle execution;
    };

    /** @brief Bounded stable identities and outcomes without provider payloads or blackboard values. */
    struct DecisionStepRecord final {
        DecisionStepRecordKind kind{DecisionStepRecordKind::BehaviorNode};
        DecisionTraceOccurrence occurrence;
        DecisionNodeId node;
        TaskId task;
        TaskHandle handle;
        AiTaskState state{AiTaskState::Idle};
        DecisionNodeId lastTransition;
        std::uint32_t transitionCount{};    /**< Total for StateMachineStep; one-based commit ordinal for StateMachineTransition. */
        BlackboardKeyId key;                /**< Utility consideration identity; invalid identifies the option total. */
        std::optional<double> utilityScore; /**< Normalized evidence; empty for ineligible or read-authorized scores. */

        bool operator==(const DecisionStepRecord &) const = default;
    };

    /** @brief Correlation with the exact immutable plan and frozen blackboard input, without copying input values. */
    struct DecisionTraceHeader final {
        static constexpr std::uint32_t CurrentVersion = 4;

        std::uint32_t version{CurrentVersion};
        DecisionGraphAssetId root;
        Sha256Digest planDigest;
        BlackboardInstanceBinding blackboard; /**< Exact agent, schema publication and blackboard instance. */
        std::uint64_t tick{};
        std::uint64_t blackboardRevision{};
        AiTaskState outcome{AiTaskState::Idle};

        /** @brief Correlates retained evidence with a declared decision input using metadata only.
         * @param digest Exact cooked plan identity used by the replay host.
         * @param expectedTick Committed decision tick.
         * @param binding Exact runtime, agent, schema and instance generation.
         * @param revision Revision of the frozen blackboard.
         * @return True if this recorded observation matches. This grants no value read or execution authority.
         */
        [[nodiscard]] bool MatchesObservation(const Sha256Digest &digest, std::uint64_t expectedTick,
                                              const BlackboardInstanceBinding &binding, std::uint64_t revision) const noexcept;
    };

    /** @brief Caller-owned single-step buffer. Copy or retain it explicitly before reusing it for another step. */
    class DecisionStepTrace final {
    public:
        static constexpr std::size_t MaximumRecords = 128;

        /** @brief Returns immutable correlation metadata. @return Header for the last admitted traced step. */
        [[nodiscard]] const DecisionTraceHeader &Header() const noexcept {
            return header_;
        }

        /** @brief Returns the valid record prefix in runtime observation order. @return Borrowed evidence until the next write. */
        [[nodiscard]] std::span<const DecisionStepRecord> Records() const noexcept {
            return std::span{records_}.first(count_);
        }

        /** @brief Reports evidence omitted when the bounded buffer filled. @return Number of omitted records for this step. */
        [[nodiscard]] std::size_t DroppedRecords() const noexcept {
            return dropped_;
        }

    private:
        friend class BehaviorTreeInstance;
        friend class DecisionGraphInstance;

        void Append(const DecisionStepRecord &record) noexcept {
            if (count_ == records_.size()) {
                ++dropped_;
                return;
            }
            records_[count_++] = record;
        }

        DecisionTraceHeader header_{};
        std::array<DecisionStepRecord, MaximumRecords> records_{};
        std::size_t count_{};
        std::size_t dropped_{};
    };
}  // namespace Horo::AI
