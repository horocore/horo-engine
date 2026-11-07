#pragma once
/** @file LuaDecisionNode.h
 * @brief Bounded Lua implementation of the generated decision-node contract.
 */
#include "Horo/AI/DecisionNodeRegistration.h"
#include "Horo/Gameplay/LuaBehavior.h"

namespace Horo::Gameplay {
    /** @brief Immutable source generation; each acquired executor owns a separate budgeted VM.
     * @details Retire closes new calls, while outstanding executors retain their exact source for abort/cleanup.
     * Replacement consists of compiling a new program and retiring the old owner at the decision safe point.
     */
    class LuaDecisionNodeProgram final {
    public:
        /** @brief Generates metadata and executable factories from a sandboxed node table.
         * @param source Owned Lua source returning role, ports, properties, requirements and lifecycle callbacks.
         * @param type Sidecar/manifest-owned persistent type. @param provider Exact selected provider identity.
         * @param version Non-zero descriptor version. @param limits Existing Lua memory/instruction budgets.
         * @param sourceLocation Actual captured chunk/source identity; line/column zero honestly indicate unavailable precision.
         */
        [[nodiscard]] static Result<std::unique_ptr<LuaDecisionNodeProgram>> Compile(
            std::string source, AI::DecisionNodeTypeId type, AI::DecisionProviderId provider, std::uint32_t version,
            SourceLocation sourceLocation, LuaBehaviorLimits limits = {},
            std::shared_ptr<const AI::DecisionNodeGeneration> providerGeneration = {});
        ~LuaDecisionNodeProgram();
        LuaDecisionNodeProgram(const LuaDecisionNodeProgram &) = delete;
        LuaDecisionNodeProgram &operator=(const LuaDecisionNodeProgram &) = delete;
        /** @brief Acquires the real generation-owned declaration/factory pair; fails after retirement or off-owner. */
        [[nodiscard]] Result<std::shared_ptr<const AI::DecisionNodeGeneration>> Acquire() const;
        /** @brief Closes callback admission without destroying leased VMs or source generations. */
        void Retire() noexcept;

    private:
        struct Image;
        class Instance;

        explicit LuaDecisionNodeProgram(std::shared_ptr<Image> image) : image_(std::move(image)) {}

        std::shared_ptr<Image> image_;
    };
}  // namespace Horo::Gameplay
