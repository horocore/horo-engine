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

        /** @brief Binds a finite authored response to the immutable sorted schema. */
        [[nodiscard]] std::optional<std::size_t> BindConsideration(const BlackboardSchema &schema,
                                                                   const UtilityConsideration &consideration) noexcept {
            const auto keys = schema.Keys();
            const auto key = std::ranges::lower_bound(keys, consideration.key, {}, &BlackboardKeyDescriptor::key);
            const auto width = consideration.maximum - consideration.minimum;
            if (key == keys.end() || key->key != consideration.key || key->kind != BlackboardValueKind::Scalar ||
                key->cardinality != BlackboardValueCardinality::Scalar || !std::isfinite(width) || width <= 0 ||
                !UnitValue(consideration.weight) || !ValidParameter(consideration))
                return std::nullopt;
            return static_cast<std::size_t>(key - keys.begin());
        }

        /** @brief Rejects malformed option identity, bounds and coefficients before copying the option. */
        [[nodiscard]] bool ValidOption(const UtilityOption &option) noexcept {
            return option.id.IsValid() && option.action.IsValid() && UnitValue(option.priority) && UnitValue(option.compensation) &&
                   !option.considerations.empty() && option.considerations.size() <= UtilitySelector::MaximumConsiderations;
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

        /** @brief Projects the borrowed value to the authored normalized score without copying the value payload. */
        [[nodiscard]] std::optional<double> ReadScore(const BlackboardValue *value, const UtilityConsideration &consideration) noexcept {
            const auto *scalar = std::get_if<BlackboardScalarValue>(value);
            const auto *input = scalar ? std::get_if<double>(scalar) : nullptr;
            if (!input || !std::isfinite(*input))
                return std::nullopt;
            const auto score = Respond(consideration, *input);
            return UnitValue(score) ? std::optional{score} : std::nullopt;
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

    /** @brief Writes only to caller-provided evidence storage and accounts for truncation. */
    struct UtilitySelector::EvidenceWriter final {
        std::span<UtilityScoreRecord> output;
        UtilityEvaluation &result;

        void Append(const UtilityScoreRecord &record) const noexcept {
            if (result.evidenceWritten < output.size())
                output[result.evidenceWritten++] = record;
            else if (!output.empty())
                ++result.evidenceDropped;
        }
    };

    /** @copydoc UtilitySelector::ScoreOption */
    Result<std::optional<double>> UtilitySelector::ScoreOption(const BlackboardSnapshot &snapshot, const UtilityOption &option,
                                                               const std::span<const std::size_t> slots, const EvidenceWriter &writer) {
        auto score = option.priority;
        bool eligible = true;
        bool withheld = false;
        const auto compensation = option.compensation * (1 - 1.0 / static_cast<double>(option.considerations.size()));
        for (std::size_t index = 0; index < option.considerations.size(); ++index) {
            const auto &consideration = option.considerations[index];
            const auto slot = slots[index];
            auto value = snapshot.ReadBound(slot);
            if (value.HasError())
                return Result<std::optional<double>>::Failure(std::move(value).ErrorValue());
            const auto response = ReadScore(value.Value(), consideration);
            const bool readAuthorized = snapshot.authorizations_[slot] != nullptr;
            writer.Append({option.id, consideration.key, readAuthorized ? std::nullopt : response});
            withheld |= readAuthorized;
            if (!response) {
                eligible = false;
                continue;
            }
            const auto adjusted = *response + (1 - *response) * compensation * *response;
            score *= std::lerp(1.0, adjusted, consideration.weight);
        }
        const auto total = eligible && UnitValue(score) ? std::optional{score} : std::nullopt;
        writer.Append({option.id, {}, withheld ? std::nullopt : total});
        return Result<std::optional<double>>::Success(total);
    }

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
            if (!ValidOption(option))
                return CaptureResult::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
            result.options_.push_back(option);
            result.workUnits_ += 1 + option.considerations.size();
        }
        std::ranges::sort(result.options_, {}, &UtilityOption::id);
        if (std::ranges::adjacent_find(result.options_, {}, &UtilityOption::id) != result.options_.end())
            return CaptureResult::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        result.slots_.reserve(result.workUnits_ - options.size());
        for (const auto &option : result.options_) {
            for (const auto &consideration : option.considerations) {
                const auto slot = BindConsideration(*result.schema_, consideration);
                if (!slot)
                    return CaptureResult::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
                result.slots_.push_back(*slot);
            }
        }
        return CaptureResult::Success(std::move(result));
    }

    /** @copydoc UtilitySelector::Evaluate */
    Result<UtilityEvaluation> UtilitySelector::Evaluate(const BlackboardSnapshot &snapshot, const AgentHandle agent,
                                                        AiWorkBudget &agentBudget, AiWorkBudget &globalBudget,
                                                        const std::span<UtilityScoreRecord> evidence) const {
        return Evaluate(snapshot, agent, agentBudget, globalBudget, evidence, {});
    }

    Result<UtilityEvaluation> UtilitySelector::Evaluate(const BlackboardSnapshot &snapshot, const AgentHandle agent,
                                                        AiWorkBudget &agentBudget, AiWorkBudget &globalBudget,
                                                        const std::span<UtilityScoreRecord> evidence, const TaskId preferred) const {
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
        std::optional<UtilitySelection> preference;
        auto slots = std::span<const std::size_t>{slots_};
        for (const auto &option : options_) {
            const auto count = option.considerations.size();
            auto score = ScoreOption(snapshot, option, slots.first(count), writer);
            if (score.HasError())
                return Evaluation::Failure(std::move(score).ErrorValue());
            slots = slots.subspan(count);
            if (IsBetterScore(score.Value(), result.selection))
                result.selection = UtilitySelection{option.id, option.action, *score.Value()};
            if (option.action == preferred && IsBetterScore(score.Value(), preference))
                preference = UtilitySelection{option.id, option.action, *score.Value()};
        }
        if (preference)
            result.selection = preference;
        return Evaluation::Success(result);
    }

    /** @copydoc UtilitySelector::Options */
    std::span<const UtilityOption> UtilitySelector::Options() const noexcept {
        return options_;
    }

    /** @copydoc UtilitySelector::Storage */
    DecisionKernelStorage UtilitySelector::Storage() const noexcept {
        DecisionKernelStorage storage{.compiledBytes = sizeof(UtilitySelector) + options_.capacity() * sizeof(UtilityOption) +
                                                       slots_.capacity() * sizeof(std::size_t)};
        for (const auto &option : options_)
            storage.compiledBytes += option.considerations.capacity() * sizeof(UtilityConsideration);
        return storage;
    }
}  // namespace Horo::AI
