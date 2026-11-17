#pragma once
/** @file DecisionPlanCodec.h
 * @brief Bounded canonical source/cooked transport; runtime ticks never parse document JSON.
 */
#include "Horo/AI/DecisionGraphCompiler.h"

namespace Horo::AI {
    struct DecisionCodecLimits final {
        std::size_t maximumBytes{16U * 1024U * 1024U};
        DecisionCompilationLimits compilation;
    };

    class DecisionPlanCodec final {
    public:
        static constexpr std::uint32_t SourceVersion = 1;
        static constexpr std::uint32_t CookedVersion = 1;
        /** @brief Encodes complete owned canonical family source, with no editor presentation state. */
        [[nodiscard]] static Result<std::vector<std::uint8_t>> EncodeSource(std::span<const DecisionGraphSource> sources,
                                                                            DecisionGraphAssetId root, DecisionCodecLimits limits = {});
        /** @brief Decodes and validates a bounded source capture before returning any candidate. */
        [[nodiscard]] static Result<std::vector<DecisionGraphSource>> DecodeSource(std::span<const std::uint8_t> bytes,
                                                                                   DecisionGraphAssetId &root,
                                                                                   DecisionCodecLimits limits = {});
        /** @brief Encodes a compiler-admitted closure with its canonical semantic inputs and declared layout. */
        [[nodiscard]] static Result<std::vector<std::uint8_t>> EncodeCooked(const CookedDecisionPlan &plan,
                                                                            DecisionCodecLimits limits = {});
        /** @brief Validates format, content identity, limits and current provider/schema compatibility before admission. */
        [[nodiscard]] static Result<DecisionGraphCompilation> DecodeCooked(std::span<const std::uint8_t> bytes,
                                                                           const DecisionCompilationEnvironment &environment,
                                                                           DecisionCodecLimits limits = {});
    };
}  // namespace Horo::AI
