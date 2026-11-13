#include "Horo/AI/UtilitySelector.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Horo::AI {
    namespace {
        [[nodiscard]] bool UnitValue(const double value) noexcept {
            return std::isfinite(value) && value >= 0 && value <= 1;
        }

        [[nodiscard]] bool ValidParameter(const UtilityConsideration &consideration) noexcept {
            switch (consideration.curve) {
                case UtilityResponseCurve::Linear:
                    return std::isfinite(consideration.parameter);
                case UtilityResponseCurve::Power:
                    return consideration.parameter >= 0.125 && consideration.parameter <= 8;
                case UtilityResponseCurve::Logistic:
                    return consideration.parameter >= 1 && consideration.parameter <= 64;
                case UtilityResponseCurve::Step:
                    return UnitValue(consideration.parameter);
                default:
                    return false;
            }
        }

        [[nodiscard]] bool ValidConsideration(const BlackboardSchema &schema, const UtilityConsideration &consideration) noexcept {
            const auto keys = schema.Keys();
            const auto key = std::ranges::find(keys, consideration.key, &BlackboardKeyDescriptor::key);
            const auto width = consideration.maximum - consideration.minimum;
            return key != keys.end() && key->kind == BlackboardValueKind::Scalar &&
                   key->cardinality == BlackboardValueCardinality::Scalar && std::isfinite(width) && width > 0 &&
                   UnitValue(consideration.weight) && ValidParameter(consideration);
        }

        [[nodiscard]] bool ValidOption(const BlackboardSchema &schema, const UtilityOption &option) noexcept {
            if (!option.id.IsValid() || !option.action.IsValid() || !UnitValue(option.priority) || !UnitValue(option.compensation))
                return false;
            return std::ranges::all_of(option.considerations, [&schema](const UtilityConsideration &consideration) {
                return ValidConsideration(schema, consideration);
            });
        }

        [[nodiscard]] double Respond(const UtilityConsideration &consideration, const double input) noexcept {
            const auto bounded = std::clamp(input, consideration.minimum, consideration.maximum);
            auto value = (bounded - consideration.minimum) / (consideration.maximum - consideration.minimum);
            if (consideration.invert)
                value = 1 - value;
            switch (consideration.curve) {
                case UtilityResponseCurve::Power:
                    return std::pow(value, consideration.parameter);
                case UtilityResponseCurve::Logistic: {
                    const auto logistic = [&consideration](const double position) {
                        return 1 / (1 + std::exp(-consideration.parameter * (position - 0.5)));
                    };
                    const auto bottom = logistic(0);
                    return std::clamp((logistic(value) - bottom) / (logistic(1) - bottom), 0.0, 1.0);
                }
                case UtilityResponseCurve::Step:
                    return value >= consideration.parameter ? 1 : 0;
                default:
                    return value;
            }
        }

        struct EvidenceWriter final {
            std::span<UtilityScoreRecord> output;
            UtilityEvaluation &result;

            void Append(const UtilityScoreRecord &record) const noexcept {
                if (result.evidenceWritten < output.size())
                    output[result.evidenceWritten++] = record;
                else if (!output.empty())
                    ++result.evidenceDropped;
            }
        };

        [[nodiscard]] Result<std::optional<double>> ReadScore(const BlackboardSnapshot &snapshot,
                                                              const UtilityConsideration &consideration) {
            auto read = snapshot.Read(consideration.key);
            if (read.HasError())
                return Result<std::optional<double>>::Failure(std::move(read).ErrorValue());
            if (!read.Value())
                return Result<std::optional<double>>::Success(std::nullopt);
            const auto *scalar = std::get_if<BlackboardScalarValue>(&*read.Value());
            const auto *input = scalar ? std::get_if<double>(scalar) : nullptr;
            if (!input || !std::isfinite(*input))
                return Result<std::optional<double>>::Success(std::nullopt);
            const auto score = Respond(consideration, *input);
            return Result<std::optional<double>>::Success(UnitValue(score) ? std::optional{score} : std::nullopt);
        }

        [[nodiscard]] Result<std::optional<double>> ScoreOption(const BlackboardSnapshot &snapshot, const UtilityOption &option,
                                                                EvidenceWriter writer) {
            auto score = option.priority;
            bool eligible = true;
            const auto compensation = option.compensation * (1 - 1.0 / static_cast<double>(option.considerations.size()));
            for (const auto &consideration : option.considerations) {
                auto response = ReadScore(snapshot, consideration);
                if (response.HasError())
                    return response;
                writer.Append({option.id, consideration.key, response.Value()});
                if (!response.Value()) {
                    eligible = false;
                    continue;
                }
                const auto value = *response.Value();
                const auto adjusted = value + (1 - value) * compensation * value;
                score *= std::lerp(1.0, adjusted, consideration.weight);
            }
            return Result<std::optional<double>>::Success(eligible && UnitValue(score) ? std::optional{score} : std::nullopt);
        }

        /** @brief Keeps the first stable option on an equal score. */
        bool IsBetterScore(const std::optional<double> score, const std::optional<UtilitySelection> &selection) noexcept {
            return score && (!selection || *score > selection->score);
        }

        /** @brief Reserves complete evaluation against each distinct budget before reading keys. */
        bool ReserveWork(AiWorkBudget &agent, AiWorkBudget &global, const std::size_t units) noexcept {
            if (std::min(agent.Remaining(), global.Remaining()) < units)
                return false;
            (void)agent.Consume(units);
            if (&agent != &global)
                (void)global.Consume(units);
            return true;
        }
    }  // namespace

    /** @copydoc UtilitySelector::Capture */
    Result<UtilitySelector> UtilitySelector::Capture(std::shared_ptr<const BlackboardSchema> schema,
                                                     const std::span<const UtilityOption> options) {
        using CaptureResult = Result<UtilitySelector>;
        if (!schema || options.empty() || options.size() > MaximumOptions)
            return CaptureResult::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        UtilitySelector result;
        result.schema_ = std::move(schema);
        result.options_.reserve(options.size());
        for (const auto &option : options) {
            if (option.considerations.empty() || option.considerations.size() > MaximumConsiderations ||
                !ValidOption(*result.schema_, option))
                return CaptureResult::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
            result.options_.push_back(option);
            result.workUnits_ += 1 + option.considerations.size();
        }
        std::ranges::sort(result.options_, {}, &UtilityOption::id);
        if (std::ranges::adjacent_find(result.options_, {}, &UtilityOption::id) != result.options_.end())
            return CaptureResult::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        return CaptureResult::Success(std::move(result));
    }

    /** @copydoc UtilitySelector::Evaluate */
    Result<UtilityEvaluation> UtilitySelector::Evaluate(const BlackboardSnapshot &snapshot, const AgentHandle agent,
                                                        AiWorkBudget &agentBudget, AiWorkBudget &globalBudget,
                                                        const std::span<UtilityScoreRecord> evidence) const {
        using Evaluation = Result<UtilityEvaluation>;
        if (!agent.IsValid() || snapshot.Binding().agent != agent || snapshot.Schema() != schema_)
            return Evaluation::Failure(MakeError(AIErrors::BlackboardInstanceStale));
        auto revision = snapshot.Revision();
        if (revision.HasError())
            return Evaluation::Failure(std::move(revision).ErrorValue());
        UtilityEvaluation result{.blackboardRevision = revision.Value()};
        if (!ReserveWork(agentBudget, globalBudget, workUnits_)) {
            result.budgetExhausted = true;
            return Evaluation::Success(result);
        }
        const EvidenceWriter writer{evidence, result};
        for (const auto &option : options_) {
            auto score = ScoreOption(snapshot, option, writer);
            if (score.HasError())
                return Evaluation::Failure(std::move(score).ErrorValue());
            writer.Append({option.id, {}, score.Value()});
            if (IsBetterScore(score.Value(), result.selection))
                result.selection = UtilitySelection{option.id, option.action, *score.Value()};
        }
        return Evaluation::Success(result);
    }

    /** @copydoc UtilitySelector::Options */
    std::span<const UtilityOption> UtilitySelector::Options() const noexcept {
        return options_;
    }

    /** @copydoc UtilitySelector::Storage */
    DecisionKernelStorage UtilitySelector::Storage() const noexcept {
        DecisionKernelStorage storage{.compiledBytes = sizeof(UtilitySelector) + options_.capacity() * sizeof(UtilityOption)};
        for (const auto &option : options_)
            storage.compiledBytes += option.considerations.capacity() * sizeof(UtilityConsideration);
        return storage;
    }
}  // namespace Horo::AI
