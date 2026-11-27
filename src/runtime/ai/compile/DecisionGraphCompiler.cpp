#include "Horo/AI/DecisionGraphCompiler.h"

#include "DecisionClosureLayout.h"
#include "DecisionCompilationBuilder.h"
#include "DecisionCompileLimits.h"
#include "Horo/AI/DecisionGraphInstance.h"
#include "Horo/AI/DecisionPlanCodec.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::AI {
    namespace {
        Result<Detail::DecisionCompilationCandidate> PrepareCompilation(DecisionGraphAssetId root,
                                                                        std::span<const DecisionGraphSource> sources,
                                                                        const DecisionCompilationEnvironment &environment,
                                                                        const DecisionCompilationLimits &limits,
                                                                        DecisionClosureAdmission admission) {
            DecisionCodecLimits codec{.compilation = limits};
            auto encoded = DecisionPlanCodec::EncodeSource(sources, root, codec);
            if (encoded.HasError())
                return Result<Detail::DecisionCompilationCandidate>::Failure(encoded.ErrorValue());
            DecisionGraphAssetId capturedRoot;
            auto captured = DecisionPlanCodec::DecodeSource(encoded.Value(), capturedRoot, codec);
            if (captured.HasError())
                return Result<Detail::DecisionCompilationCandidate>::Failure(captured.ErrorValue());
            return Detail::BuildDecisionCompilation(capturedRoot, captured.Value(), environment, limits, admission);
        }

        SourceLocation RootLocation(DecisionGraphAssetId root, std::span<const DecisionGraphSource> sources) {
            const auto bounded = sources.first(std::min<std::size_t>(sources.size(), 128));
            const auto found = std::ranges::find(bounded, root, &DecisionGraphSource::asset);
            return found == bounded.end() ? SourceLocation{"<decision-capture>", 0, 0} : found->location;
        }

        Result<void> ChargeEnvelope(DecisionStorageRequirements &storage, std::size_t bytes, const DecisionCompilationLimits &limits) {
            const auto owned =
                static_cast<std::uint64_t>(bytes) + sizeof(CookedDecisionPlan) + storage.occurrences * sizeof(DecisionCallOccurrence);
            storage.coreInstanceBytes += sizeof(DecisionGraphInstance);
            if (owned > std::numeric_limits<std::uint64_t>::max() - storage.compiledBytes)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            storage.compiledBytes += owned;
            if (storage.compiledBytes > limits.maximumCoreBytes)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            auto remaining = limits.maximumCoreBytes - storage.compiledBytes;
            if (storage.coreInstanceBytes > remaining)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            remaining -= storage.coreInstanceBytes;
            if (storage.scratchBytes > remaining)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            return Result<void>::Success();
        }

        /** @brief Bounds the number of state transitions that can re-enter a call site in one step. */
        std::size_t TransitionLimit(const CookedStateMachinePlan &machine) noexcept {
            return machine.Transitions().empty() ? 0 : machine.MaximumTransitionsPerStep();
        }

        /** @brief Reserves local state visits, action phases and transition searches. */
        std::size_t MachineWork(const CookedStateMachinePlan &machine) noexcept {
            const auto depth = std::min(machine.States().size(), StateMachineLimits::Depth);
            std::size_t search = depth * machine.Transitions().size();
            for (const auto &transition : machine.Transitions()) {
                // Each transition can reach its guards only at the one matching source in the active path.
                search += 1 + transition.guards.size() + (transition.event.has_value() ? StateMachineLimits::EventsPerStep : 0);
            }
            return 1 + 3 * depth + TransitionLimit(machine) * (search + 4 * depth);
        }

        /** @brief Reserves selection and action work, including dormant actions restarted by ancestor calls. */
        std::size_t UtilityWork(const CookedUtilityPlan &utility, const bool composed) noexcept {
            std::size_t actions{};
            for (const auto &action : utility.Actions()) {
                const auto work = action->MaximumDecisionWorkUnits();
                actions = composed ? actions + work : std::max(actions, work);
            }
            return utility.Selector().MaximumWorkUnits() + actions;
        }

        /** @brief Bounds one visit to an occurrence without charging any child evaluation. */
        std::size_t OccurrenceWork(const DecisionCompiledGraph &graph, const bool composed) noexcept {
            if (const auto *tree = std::get_if<std::shared_ptr<const BehaviorTreeExecutionPlan>>(&graph.execution))
                return (*tree)->MaximumDecisionWorkUnits();
            if (const auto *machine = std::get_if<std::shared_ptr<const CookedStateMachinePlan>>(&graph.execution))
                return MachineWork(**machine);
            return UtilityWork(*std::get<std::shared_ptr<const CookedUtilityPlan>>(graph.execution), composed);
        }

        /** @brief Returns the number of transitions that may revisit calls of this graph. */
        std::size_t CallTransitionLimit(const DecisionCompiledGraph &graph) noexcept {
            if (const auto *machine = std::get_if<std::shared_ptr<const CookedStateMachinePlan>>(&graph.execution))
                return TransitionLimit(**machine);
            return 0;
        }

        /** @brief Bounds reset passes from ancestors, plus this occurrence's evaluation and retirement. */
        std::size_t OccurrencePasses(const CookedDecisionPlan &plan, std::size_t index) noexcept {
            const auto occurrences = plan.Occurrences();
            std::size_t passes = 2;
            while (index != 0) {
                index = occurrences[index].parent;
                passes += 1 + CallTransitionLimit(plan.Graphs()[occurrences[index].plan]);
            }
            return passes;
        }

        /** @brief Reserves the whole closure before any child can execute. */
        std::optional<std::size_t> ClosureWork(const CookedDecisionPlan &plan) noexcept {
            std::size_t work{};
            const auto occurrences = plan.Occurrences();
            for (std::size_t index = 0; index < occurrences.size(); ++index) {
                const auto &occurrence = occurrences[index];
                const auto &graph = plan.Graphs()[occurrence.plan];
                const auto local = OccurrenceWork(graph, occurrences.size() > 1);
                if (occurrences.size() == 1)
                    return local;
                const auto passes = OccurrencePasses(plan, index);
                // Reserve dispatch plus both the failed phase's cleanup and the owner's retirement.
                // A machine transition may also exit and re-enter the same call.
                const auto lookups = (4 + 2 * CallTransitionLimit(graph)) * graph.calls.size() * (occurrence.subtreeEnd - index - 1);
                if (const auto available = std::numeric_limits<std::size_t>::max() - work;
                    lookups > available || local > (available - lookups) / passes)
                    return std::nullopt;
                work += passes * local + lookups;
            }
            return work;
        }

    }  // namespace

    Result<DecisionGraphCompilation> DecisionGraphCompiler::Compile(DecisionGraphAssetId root, std::span<const DecisionGraphSource> sources,
                                                                    const DecisionCompilationEnvironment &environment,
                                                                    DecisionCompilationLimits limits) {
        const auto reject = [&](const Error &error) {
            std::vector<DecisionAssetValidationDiagnostic> diagnostics;
            diagnostics.emplace_back(error.code, DiagnosticSeverity::Error, error.message, RootLocation(root, sources),
                                     "executableClosure");
            return Result<DecisionGraphCompilation>::Success({DecisionAssetValidationReport{std::move(diagnostics)}, {}});
        };
        if (!Detail::ValidDecisionLimits(limits))
            return reject(MakeError(AIErrors::DecisionAssetLimitExceeded));
        try {
            auto environmentIdentity = Detail::DecisionEnvironmentIdentity(environment);
            if (environmentIdentity.HasError())
                return reject(environmentIdentity.ErrorValue());
            auto built = PrepareCompilation(root, sources, environment, limits, DecisionClosureAdmission{});
            if (built.HasError())
                return reject(built.ErrorValue());
            auto candidate = std::move(built).Value();
            if (!candidate.diagnostics.empty())
                return Result<DecisionGraphCompilation>::Success({DecisionAssetValidationReport{std::move(candidate.diagnostics)}, {}});
            auto layout = Detail::BuildDecisionClosureLayout(candidate.graphs, candidate.root, limits);
            if (layout.HasError())
                return reject(layout.ErrorValue());
            auto sourceBytes = DecisionPlanCodec::EncodeSource(candidate.sources, root, {.compilation = limits});
            if (sourceBytes.HasError())
                return reject(sourceBytes.ErrorValue());
            auto storage = layout.Value().storage;
            if (auto charged = ChargeEnvelope(storage, sourceBytes.Value().capacity(), limits); charged.HasError())
                return reject(charged.ErrorValue());
            auto plan = std::shared_ptr<CookedDecisionPlan>(new CookedDecisionPlan());
            plan->graphs_ = std::move(candidate.graphs);
            plan->occurrences_ = std::move(layout).Value().occurrences;
            plan->storage_ = storage;
            plan->root_ = root;
            plan->sourceBytes_ = std::move(sourceBytes).Value();
            plan->environmentIdentity_ = environmentIdentity.Value();
            plan->runtimeLayoutVersion_ = environment.runtimeLayoutVersion;
            plan->capabilities_ = environment.capabilities;
            plan->providers_ = environment.nodes;
            auto cooked = DecisionPlanCodec::EncodeCooked(*plan, {.compilation = limits});
            if (cooked.HasError())
                return reject(cooked.ErrorValue());
            plan->digest_ = ComputeSha256(std::as_bytes(std::span{cooked.Value()}));
            return Result<DecisionGraphCompilation>::Success({{}, std::move(plan)});
        } catch (const std::bad_alloc &) {
            return Result<DecisionGraphCompilation>::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    std::span<const DecisionCompiledGraph> CookedDecisionPlan::Graphs() const noexcept {
        return graphs_;
    }

    std::span<const DecisionCallOccurrence> CookedDecisionPlan::Occurrences() const noexcept {
        return occurrences_;
    }

    const DecisionStorageRequirements &CookedDecisionPlan::Storage() const noexcept {
        return storage_;
    }

    const Sha256Digest &CookedDecisionPlan::Digest() const noexcept {
        return digest_;
    }

    DecisionGraphAssetId CookedDecisionPlan::Root() const noexcept {
        return root_;
    }

    AiCapabilitySet CookedDecisionPlan::Capabilities() const noexcept {
        return capabilities_;
    }

    bool CookedDecisionPlan::IsCurrent() const noexcept {
        return providers_ && providers_->CanUse();
    }

    /** @copydoc CookedDecisionPlan::MaximumDecisionWorkUnits */
    std::optional<std::size_t> CookedDecisionPlan::MaximumDecisionWorkUnits() const noexcept {
        return ClosureWork(*this);
    }

    Result<bool> DecisionPlanStore::TryActivate(DecisionGraphCompilation candidate) {
        if (std::this_thread::get_id() != owner_)
            return Result<bool>::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        if (!candidate.plan || candidate.validation.HasErrors())
            return Result<bool>::Success(false);
        if (!candidate.plan->IsCurrent())
            return Result<bool>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        current_ = std::move(candidate.plan);
        return Result<bool>::Success(true);
    }

    std::shared_ptr<const CookedDecisionPlan> DecisionPlanStore::Current() const noexcept {
        return std::this_thread::get_id() == owner_ ? current_ : nullptr;
    }
}  // namespace Horo::AI
