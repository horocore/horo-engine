#pragma once
/** @file DecisionPlanCodec.h
 * @brief Bounded canonical source/cooked transport; runtime ticks never parse document JSON.
 */
#include "Horo/AI/DecisionGraphCompiler.h"

namespace Horo::AI {
    /** @brief Canonical semantic asset type shared by decision cooking and authoritative runtime admission. */
    inline constexpr std::string_view BlackboardSchemaAssetType = "core.blackboard_schema";
    inline constexpr std::string_view BlackboardSchemaSourceExtension = ".horo_bb"; /**< Native authored Blackboard schema suffix. */

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

    /** @brief Source-only presentation of one stable key. Missing labels preserve unadorned semantic sources. */
    struct BlackboardSchemaAuthoringRow final {
        static constexpr std::size_t MaximumLabelBytes = 128;
        BlackboardKeyId key;
        std::optional<std::string> displayName;

        bool operator==(const BlackboardSchemaAuthoringRow &) const noexcept = default;
    };

    /** @brief Complete typed source. Cooking projects only the admitted semantic schema. */
    struct BlackboardAuthoredSchema final {
        BlackboardSchema schema;
        std::vector<BlackboardSchemaAuthoringRow> rows;
    };

    /** @brief Checks nonempty bounded UTF-8 authoring text. @param displayName Explicit source label. @return Valid text. */
    [[nodiscard]] bool IsValidBlackboardDisplayName(std::string_view displayName) noexcept;

    /** @brief Source-only transport wrapping the existing immutable semantic codec. */
    class BlackboardSchemaSourceCodec final {
    public:
        static constexpr std::uint32_t Version = 1;
        static constexpr std::size_t MaximumBytes =
            BlackboardSchemaCodec::MaximumBytes + MaximumBlackboardKeys * (BlackboardSchemaAuthoringRow::MaximumLabelBytes + 16U) + 32U;

        /** @brief Encodes complete source rows with the exact admitted schema.
         * @param schema Current immutable semantic schema.
         * @param rows Every stable key exactly once, in authored order.
         * @return Canonical source bytes or typed invalid/capacity error.
         */
        [[nodiscard]] static Result<std::vector<std::uint8_t>> Encode(const BlackboardSchema &schema,
                                                                      std::span<const BlackboardSchemaAuthoringRow> rows);
        /** @brief Reads current authored source or the earlier semantic-only .horo_bb.
         * @param bytes Complete bounded registered source.
         * @return Validated schema and ordered rows. Legacy assets produce one unlabeled row per sorted key.
         */
        [[nodiscard]] static Result<BlackboardAuthoredSchema> Decode(std::span<const std::uint8_t> bytes);
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
