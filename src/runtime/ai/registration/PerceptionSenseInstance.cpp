#include "Horo/AI/PerceptionSenseProvider.h"

#include <algorithm>
#include <exception>
#include <new>
#include <string>
#include <string_view>

namespace Horo::AI {
    namespace {
        /** @brief Prevents nested provider dispatch while preserving admission on every return/exception path. */
        struct EvaluationScope final {
            bool &active;

            explicit EvaluationScope(bool &flag) noexcept : active(flag) {
                active = true;
            }

            ~EvaluationScope() {
                active = false;
            }
        };

        /** @brief Reads at most the diagnostic ceiling from a native exception, without an unbounded strlen first. */
        std::string_view ExceptionText(const std::exception &error) noexcept {
            const char *text = error.what();
            if (!text)
                return {};
            std::size_t length{};
            while (length < 512 && text[length] != '\0')
                ++length;
            return {text, length};
        }

        /** @brief Keeps diagnostic allocation failure inside the same sense-local exception boundary. */
        Error SenseFailure(const ErrorCodeDescriptor &descriptor, const std::exception *error = nullptr) noexcept {
            try {
                return MakeError(descriptor, error ? std::string{ExceptionText(*error)} : std::string{});
            } catch (const std::bad_alloc &) {
                // MakeError fills an empty message from the summary, which would allocate again here.
                return {.code = descriptor.code, .domain = descriptor.domain, .severity = descriptor.defaultSeverity};
            }
        }

        /** @brief Invokes one module-owned factory while its caller retains the exact executable owner. */
        Result<IPerceptionSenseProvider *> CreateProvider(const PerceptionSenseFactory &factory) {
            try {
                return Result<IPerceptionSenseProvider *>::Success(factory.create(factory.userData));
            } catch (const std::bad_alloc &) {
                return Result<IPerceptionSenseProvider *>::Failure(SenseFailure(AIErrors::PerceptionRegistryStorageUnavailable));
            } catch (...) {
                return Result<IPerceptionSenseProvider *>::Failure(SenseFailure(AIErrors::PerceptionProviderFailed));
            }
        }

        /** @brief Checks constant-time binding and capacity facts before bounded sequence validation. */
        bool MatchesBinding(const PerceptionSenseInput &input, const PerceptionSenseBinding &binding) noexcept {
            return input.agent == binding.agent && input.listener == binding.listener && input.spatialRevision > 0;
        }

        /** @brief Checks one bounded stable-identity-ordered source slice without live Scene reads. */
        bool ValidCandidates(const PerceptionSenseInput &input) noexcept {
            PerceptionSourceRef previous;
            for (const auto &candidate : input.candidates) {
                if (!candidate.source.IsValid() || candidate.source.sceneIncarnation != input.listener.sceneIncarnation ||
                    !(previous < candidate.source))
                    return false;
                previous = candidate.source;
            }
            return true;
        }

        /** @brief Validates all output before exposing any sample to the spatial owner. */
        bool ValidSamples(const PerceptionSenseResult &result, const std::size_t candidates) noexcept {
            if (result.count > result.samples.size())
                return false;
            for (std::size_t index = 0; index < result.count; ++index) {
                const auto candidate = result.samples[index].candidate;
                if (candidate >= candidates || (index != 0 && result.samples[index - 1].candidate >= candidate))
                    return false;
            }
            return true;
        }

        /** @brief Reserves each distinct owner allowance before either bounded validation or provider work. */
        bool Reserve(AiWorkBudget &agent, AiWorkBudget &global, const std::size_t units) noexcept {
            if (std::min(agent.Remaining(), global.Remaining()) < units)
                return false;
            static_cast<void>(agent.Consume(units));
            if (&agent != &global)
                static_cast<void>(global.Consume(units));
            return true;
        }
    }  // namespace

    /** @copydoc PerceptionSenseInstance::PerceptionSenseInstance */
    PerceptionSenseInstance::PerceptionSenseInstance(std::shared_ptr<const PerceptionSenseGeneration> generation, const std::size_t index,
                                                     PerceptionSenseBinding binding) noexcept
        : generation_(std::move(generation)), index_(index), binding_(std::move(binding)) {}

    /** @copydoc PerceptionSenseInstance::Initialize */
    Result<void> PerceptionSenseInstance::Initialize() {
        auto created = CreateProvider(generation_->factories_[index_]);
        if (created.HasValue())
            provider_ = created.Value();
        if (!generation_->CanAdmit() || binding_.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        if (created.HasError())
            return Result<void>::Failure(std::move(created).ErrorValue());
        return provider_ ? Result<void>::Success() : Result<void>::Failure(SenseFailure(AIErrors::PerceptionProviderFailed));
    }

    /** @copydoc PerceptionSenseInstance::~PerceptionSenseInstance */
    PerceptionSenseInstance::~PerceptionSenseInstance() {
        if (std::this_thread::get_id() != generation_->owner_)
            std::terminate();
        if (provider_) {
            const auto &factory = generation_->factories_[index_];
            factory.destroy(factory.userData, provider_);
        }
    }

    /** @copydoc PerceptionSenseInstance::AdmitInput */
    Result<void> PerceptionSenseInstance::AdmitInput(const PerceptionSenseInput &input) const {
        if (!CanEvaluate())
            return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        if (!MatchesBinding(input, binding_))
            return Result<void>::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        if (input.candidates.size() > generation_->contracts_[index_].maximumCandidates)
            return Result<void>::Failure(MakeError(AIErrors::PerceptionSpatialLimitExceeded));
        if (lastTick_ && input.tick <= *lastTick_)
            return Result<void>::Failure(MakeError(AIErrors::PerceptionMemoryTimeInvalid));
        if (input.spatialRevision < lastSpatialRevision_)
            return Result<void>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        return Result<void>::Success();
    }

    /** @copydoc PerceptionSenseInstance::RunProvider */
    Result<std::size_t> PerceptionSenseInstance::RunProvider(const PerceptionSenseInput &input,
                                                             const std::span<PerceptionSenseSample> output, AiWorkBudget &budget) {
        try {
            auto evaluated = provider_->Evaluate(input, output, budget);
            if (budget.Exhausted()) {
                failed_ = true;
                return Result<std::size_t>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
            }
            return evaluated;
        } catch (const std::exception &error) {
            failed_ = true;
            return Result<std::size_t>::Failure(SenseFailure(AIErrors::PerceptionProviderFailed, &error));
        } catch (...) {
            failed_ = true;
            return Result<std::size_t>::Failure(SenseFailure(AIErrors::PerceptionProviderFailed));
        }
    }

    /** @copydoc PerceptionSenseInstance::Evaluate */
    Result<PerceptionSenseResult> PerceptionSenseInstance::Evaluate(const PerceptionSenseInput &input, AiWorkBudget &agentBudget,
                                                                    AiWorkBudget &globalBudget) {
        using Output = Result<PerceptionSenseResult>;
        if (auto admitted = AdmitInput(input); admitted.HasError())
            return Output::Failure(admitted.ErrorValue());
        const EvaluationScope scope{evaluating_};
        const auto &contract = generation_->contracts_[index_];
        if (!Reserve(agentBudget, globalBudget, contract.workUnits + PerceptionSenseGeneration::ValidationWorkUnits))
            return Output::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
        if (!ValidCandidates(input))
            return Output::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        lastTick_ = input.tick;
        lastSpatialRevision_ = input.spatialRevision;
        AiWorkBudget local{contract.workUnits};
        PerceptionSenseResult result;
        auto captured = input;
        captured.cancellation = binding_.cancellation;
        auto evaluated = RunProvider(captured, std::span{result.samples}.first(contract.maximumCandidates), local);
        if (!generation_->CanAdmit() || binding_.cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        if (evaluated.HasError())
            return Output::Failure(std::move(evaluated).ErrorValue());
        result.count = evaluated.Value();
        if (!ValidSamples(result, input.candidates.size())) {
            failed_ = true;
            return Output::Failure(SenseFailure(AIErrors::PerceptionProviderFailed));
        }
        return Output::Success(result);
    }

    /** @copydoc PerceptionSenseInstance::CanEvaluate */
    bool PerceptionSenseInstance::CanEvaluate() const noexcept {
        return generation_->CanAdmit() && !failed_ && !evaluating_ && !binding_.cancellation.IsCancellationRequested();
    }

    /** @copydoc PerceptionSenseInstance::Binding */
    const PerceptionSenseBinding &PerceptionSenseInstance::Binding() const noexcept {
        return binding_;
    }

    /** @copydoc PerceptionSenseInstance::Contract */
    const PerceptionSenseContract &PerceptionSenseInstance::Contract() const noexcept {
        return generation_->contracts_[index_];
    }
}  // namespace Horo::AI
