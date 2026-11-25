#pragma once
/** @file AiReducedPopulation.h
 * @brief Optional owner-lane AI population for coarse simulation outside the canonical Scene.
 */
#include "Horo/AI/AIIdentity.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::AI {
    /** @brief Stationary population sample in exact signed world millimetres. */
    struct AiReducedAgent final {
        AgentId agent;
        std::array<std::int64_t, 3> positionMillimeters;
    };

    /** @brief Explicit limits for one reduced provider. Full agents remain separately budgeted by the Scene. */
    struct AiReducedPopulationLimits final {
        static constexpr std::size_t HardAgents = 1'048'576;
        static constexpr std::size_t HardTransitions = 65'536;

        std::size_t maximumAgents{};
        std::size_t maximumTransitions{};
        std::uint64_t promotionRadiusMillimeters{}; /**< Inclusive axis-aligned radius around the simulation anchor. */
    };

    /** @brief Owns compact fixed positions for identities whose canonical Scene representation is absent.
     * @details Only the authority owner lane may use the provider. The manifest is fixed at creation
     * for this incarnation. No graph, task, Blackboard, or Scene object is created by this provider.
     */
    class AiReducedPopulation final {
        struct State;

    public:
        /** @brief One prepared batch of authority transfers. Dropping it preserves the current population. */
        class Transition final {
        public:
            Transition(Transition &&) noexcept;
            Transition &operator=(Transition &&) noexcept;
            ~Transition();

            /** @brief Revalidates the owner lane and exact population revision before an aggregate Scene commit.
             * @return Success or typed stale-publication failure.
             */
            [[nodiscard]] Result<void> ValidatePublication() const;

            /** @brief Commits all prepared transfers without invoking providers or allocating.
             * @return Success or a typed stale-publication failure with no change.
             * @pre The Scene owner validates both candidates before publishing either, on the same lane with no intervening callbacks.
             */
            [[nodiscard]] Result<void> Publish() const;

        private:
            friend class AiReducedPopulation;
            struct Change final {
                std::size_t slot;
                std::optional<std::array<std::int64_t, 3>> demotion;
            };
            Transition(const std::shared_ptr<State> &state, std::vector<Change> changes);

            std::weak_ptr<State> state_;
            std::vector<Change> changes_;
            std::uint64_t revision_{};
        };

        AiReducedPopulation(AiReducedPopulation &&) noexcept;
        AiReducedPopulation &operator=(AiReducedPopulation &&) noexcept;
        ~AiReducedPopulation();

        /** @brief Captures the reduced identity manifest without constructing canonical agents.
         * @param limits Explicit positive capacity and promotion radius.
         * @param initial Strictly increasing valid identities and their initial positions.
         * @return Owner-lane population or typed admission failure.
         */
        [[nodiscard]] static Result<AiReducedPopulation> Create(AiReducedPopulationLimits limits, std::span<const AiReducedAgent> initial);

        /** @brief Selects the first eligible reduced agents in persistent identity order.
         * @param anchor Exact signed world-millimetre position from the authority.
         * @param output Caller-owned proposal storage of at least the configured transition limit.
         * @return Filled prefix size or a typed owner/capacity failure. Proposing does not change ownership.
         */
        [[nodiscard]] Result<std::size_t> EvaluatePromotions(const std::array<std::int64_t, 3> &anchor,
                                                             std::span<AiReducedAgent> output) const;

        /** @brief Prepares an all-or-nothing change of representation using the same persistent identities.
         * @param promotions Strictly increasing reduced identities with a separately prepared canonical admission.
         * @param demotions Strictly increasing full identities and their final authoritative Scene positions.
         * @return A detached batch or typed validation/capacity failure with no mutation.
         */
        [[nodiscard]] Result<Transition> PrepareTransition(std::span<const AgentId> promotions,
                                                           std::span<const AiReducedAgent> demotions) const;

    private:
        explicit AiReducedPopulation(std::shared_ptr<State> state) noexcept;

        std::shared_ptr<State> state_;
    };
}  // namespace Horo::AI
