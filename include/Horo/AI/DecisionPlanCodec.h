#pragma once
/** @file DecisionPlanCodec.h
 * @brief Bounded canonical source/cooked transport; runtime ticks never parse document JSON.
 */
#include "Horo/AI/DecisionGraphCompiler.h"

namespace Horo::AI {
    /** @brief Canonical semantic asset type shared by decision cooking and authoritative runtime admission. */
    inline constexpr std::string_view BlackboardSchemaAssetType = "core.blackboard_schema";

    /** @brief Bounded semantic .horo_bb transport used for source and immutable cooked schema payloads. */
    class BlackboardSchemaCodec final {
    public:
        static constexpr std::uint32_t Version = 1;
        static constexpr std::size_t MaximumBytes = 1024U * 1024U;

        /** @brief Encodes one admitted schema. @param schema Current immutable schema. @return Canonical bytes or typed error. */
        [[nodiscard]] static Result<std::vector<std::uint8_t>> Encode(const BlackboardSchema &schema);
        /** @brief Reads one versioned typed schema. @param bytes Bounded input. @return Validated owned schema or typed error. */
        [[nodiscard]] static Result<BlackboardSchema> Decode(std::span<const std::uint8_t> bytes);
    };

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
