#include "DecisionSourceWire.h"

#include <bit>
#include <cmath>

namespace Horo::AI::Detail {
    namespace {
        /** @brief Writes a finite canonical double; negative zero has the same policy meaning as zero. */
        void WriteNumber(DecisionWriter &writer, double value) {
            if (!std::isfinite(value)) {
                writer.Reject();
                return;
            }
            if (value == 0)
                value = 0;
            writer.Unsigned(std::bit_cast<std::uint64_t>(value));
        }

        /** @brief Rejects non-finite values while reading a canonical scalar policy. */
        double ReadNumber(DecisionReader &reader) {
            const double value = std::bit_cast<double>(reader.Unsigned());
            if (!std::isfinite(value))
                reader.Reject();
            return value;
        }

        /** @brief Writes the typed scalar key and fixed response policy in authored evaluation order. */
        void WriteConsideration(DecisionWriter &writer, const UtilityConsideration &value) {
            writer.IdentityValue(value.key);
            WriteNumber(writer, value.minimum);
            WriteNumber(writer, value.maximum);
            WriteNumber(writer, value.weight);
            writer.Enumeration(value.curve);
            WriteNumber(writer, value.parameter);
            writer.Unsigned(value.invert, 1);
        }

        /** @brief Reads one bounded scalar consideration. */
        UtilityConsideration ReadConsideration(DecisionReader &reader) {
            UtilityConsideration value;
            value.key = reader.IdentityValue<BlackboardKeyId>();
            value.minimum = ReadNumber(reader);
            value.maximum = ReadNumber(reader);
            value.weight = ReadNumber(reader);
            value.curve = reader.Enumeration(UtilityResponseCurve::Count);
            value.parameter = ReadNumber(reader);
            value.invert = reader.Boolean();
            return value;
        }

        /** @brief Writes the option policy followed by its shared task-provider binding. */
        void WriteOption(DecisionWriter &writer, const UtilityDecisionOption &value) {
            writer.IdentityValue(value.option.id);
            writer.IdentityValue(value.option.action);
            WriteNumber(writer, value.option.priority);
            WriteNumber(writer, value.option.compensation);
            WriteSequence<UtilityConsideration>(writer, value.option.considerations, UtilitySelector::MaximumConsiderations,
                                                WriteConsideration);
            WriteProviderNode(writer, value.provider, value.properties);
        }

        /** @brief Reads one option and its actual provider configuration. */
        UtilityDecisionOption ReadOption(DecisionReader &reader) {
            UtilityDecisionOption value;
            value.option.id = reader.IdentityValue<DecisionNodeId>();
            value.option.action = reader.IdentityValue<TaskId>();
            value.option.priority = ReadNumber(reader);
            value.option.compensation = ReadNumber(reader);
            value.option.considerations =
                ReadSequence<UtilityConsideration>(reader, UtilitySelector::MaximumConsiderations, ReadConsideration);
            ReadProviderNode(reader, value.provider, value.properties);
            return value;
        }
    }  // namespace

    /** @copydoc WriteUtility */
    void WriteUtility(DecisionWriter &writer, const UtilityDecisionSource &source) {
        writer.IdentityValue(source.root);
        WriteRequirements(writer, source.requirements);
        std::vector<const UtilityDecisionOption *> options;
        for (const auto &option : source.options)
            options.push_back(&option);
        std::ranges::sort(options, {}, [](const auto *option) {
            return option->option.id;
        });
        writer.Count(options.size(), UtilitySelector::MaximumOptions);
        for (const auto *option : options)
            WriteOption(writer, *option);
    }

    /** @copydoc ReadUtility */
    UtilityDecisionSource ReadUtility(DecisionReader &reader) {
        UtilityDecisionSource source;
        source.root = reader.IdentityValue<DecisionNodeId>();
        source.requirements =
            ReadSequence<DecisionBlackboardRequirement>(reader, DecisionAssetValidationHardLimits::RequirementsPerNode, ReadRequirement);
        source.options = ReadSequence<UtilityDecisionOption>(reader, UtilitySelector::MaximumOptions, ReadOption);
        return source;
    }
}  // namespace Horo::AI::Detail
